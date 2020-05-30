/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "txn.h"
#include "engine.h"
#include "tuple.h"
#include "journal.h"
#include <fiber.h>
#include "xrow.h"
#include "errinj.h"
#include "small/mempool.h"

struct tx_value
{
	struct tuple *tuple;
	struct txn_stmt *add_stmt;
	struct txn_stmt *del_stmt;
	struct rlist reader_list;
};

static uint32_t
tx_value_key_hash(const struct tuple *a)
{

	return (uint32_t)(((uintptr_t)a) >> 3);
}

#define mh_name _value
#define mh_key_t struct tuple *
#define mh_node_t struct tx_value
#define mh_arg_t int
#define mh_hash(a, arg) (tx_value_key_hash((a)->tuple))
#define mh_hash_key(a, arg) (tx_value_key_hash(a))
#define mh_cmp(a, b, arg) ((a)->tuple != (b)->tuple)
#define mh_cmp_key(a, b, arg) ((a) != (b)->tuple)
#define MH_SOURCE
#include "salad/mhash.h"

struct tx_manager
{
	struct mempool tx_value_pool;
	struct mh_value_t *values;
};

static struct tx_manager tx_manager_core;

struct tx_read_tracker {
	struct txn *reader;
	struct tx_value *value;
	struct rlist in_reader_list;
	struct rlist in_read_set;
};

struct tx_conflict_tracker {
	struct txn *wreaker;
	struct txn *victim;
	struct rlist in_conflict_list;
	struct rlist in_conflicted_by_list;
};

double too_long_threshold;

/* Txn cache. */
static struct stailq txn_cache = {NULL, &txn_cache.first};

static int
txn_on_stop(struct trigger *trigger, void *event);

static int
txn_on_yield(struct trigger *trigger, void *event);

static void
txn_run_rollback_triggers(struct txn *txn, struct rlist *triggers);

static int
txn_add_redo(struct txn *txn, struct txn_stmt *stmt, struct request *request)
{
	/* Create a redo log row. */
	int size;
	struct xrow_header *row;
	row = region_alloc_object(&txn->region, struct xrow_header, &size);
	if (row == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_object", "row");
		return -1;
	}
	if (request->header != NULL) {
		*row = *request->header;
	} else {
		/* Initialize members explicitly to save time on memset() */
		row->type = request->type;
		row->replica_id = 0;
		row->lsn = 0;
		row->sync = 0;
		row->tm = 0;
	}
	/*
	 * Group ID should be set both for requests not having a
	 * header, and for the ones who have it. This is because
	 * even if a request has a header, the group id could be
	 * omitted in it, and is default - 0. Even if the space's
	 * real group id is different.
	 */
	struct space *space = stmt->space;
	row->group_id = space != NULL ? space_group_id(space) : 0;
	row->bodycnt = xrow_encode_dml(request, &txn->region, row->body);
	if (row->bodycnt < 0)
		return -1;
	stmt->row = row;
	return 0;
}

/** Initialize a new stmt object within txn. */
static struct txn_stmt *
txn_stmt_new(struct region *region, struct space *space)
{
	struct txn_stmt *stmt;
	size_t stmt_size = sizeof(struct txn_stmt);
	if (space != NULL)
		stmt_size += space->index_count * sizeof(struct tuple *);
	stmt = (struct txn_stmt *)
		region_aligned_alloc(region, stmt_size,
				     alignof(struct txn_stmt));
	if (stmt == NULL) {
		diag_set(OutOfMemory, stmt_size, "region_alloc_object", "stmt");
		return NULL;
	}

	/* Initialize members explicitly to save time on memset() */
	stmt->txn_owner = in_txn();
	stmt->space = NULL;
	stmt->old_tuple = NULL;
	stmt->new_tuple = NULL;
	stmt->engine_savepoint = NULL;
	stmt->row = NULL;
	stmt->has_triggers = false;
	stmt->track_count = space == NULL ? 0 : space->index_count;
	memset(stmt->track, 0, stmt->track_count * sizeof(stmt->track[0]));
	return stmt;
}

static inline void
txn_stmt_unref_tuples(struct txn_stmt *stmt)
{
	if (stmt->old_tuple != NULL)
		tuple_unref(stmt->old_tuple);
	if (stmt->new_tuple != NULL)
		tuple_unref(stmt->new_tuple);
	for (size_t i = 0; i < stmt->track_count; i++)
		if (stmt->track[i] != NULL)
			tuple_unref(stmt->track[i]);
}

/*
 * Undo changes done by a statement and run the corresponding
 * rollback triggers.
 *
 * Note, a trigger set by a particular statement must be run right
 * after the statement is rolled back, because rollback triggers
 * installed by DDL statements restore the schema cache, which is
 * necessary to roll back previous statements. For example, to roll
 * back a DML statement applied to a space whose index is dropped
 * later in the same transaction, we must restore the dropped index
 * first.
 */
static void
txn_rollback_one_stmt(struct txn *txn, struct txn_stmt *stmt)
{
	if (txn->engine != NULL && stmt->space != NULL)
		engine_rollback_statement(txn->engine, txn, stmt);
	if (stmt->has_triggers)
		txn_run_rollback_triggers(txn, &stmt->on_rollback);
}

static void
txn_rollback_to_svp(struct txn *txn, struct stailq_entry *svp)
{
	struct txn_stmt *stmt;
	struct stailq rollback;
	stailq_cut_tail(&txn->stmts, svp, &rollback);
	stailq_reverse(&rollback);
	stailq_foreach_entry(stmt, &rollback, next) {
		txn_rollback_one_stmt(txn, stmt);
		if (stmt->row != NULL && stmt->row->replica_id == 0) {
			assert(txn->n_new_rows > 0);
			txn->n_new_rows--;
			if (stmt->row->group_id == GROUP_LOCAL)
				txn->n_local_rows--;
		}
		if (stmt->row != NULL && stmt->row->replica_id != 0) {
			assert(txn->n_applier_rows > 0);
			txn->n_applier_rows--;
		}
		txn_stmt_unref_tuples(stmt);
		stmt->space = NULL;
		stmt->row = NULL;
	}
}

/*
 * Return a txn from cache or create a new one if cache is empty.
 */
inline static struct txn *
txn_new(void)
{
	if (!stailq_empty(&txn_cache))
		return stailq_shift_entry(&txn_cache, struct txn, in_txn_cache);

	/* Create a region. */
	struct region region;
	region_create(&region, &cord()->slabc);

	/* Place txn structure on the region. */
	int size;
	struct txn *txn = region_alloc_object(&region, struct txn, &size);
	if (txn == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_object", "txn");
		return NULL;
	}
	assert(region_used(&region) == sizeof(*txn));
	txn->region = region;
	rlist_create(&txn->read_set);
	rlist_create(&txn->conflict_list);
	rlist_create(&txn->conflicted_by_list);
	return txn;
}

/*
 * Free txn memory and return it to a cache.
 */
inline static void
txn_free(struct txn *txn)
{
	struct tx_read_tracker *tracker, *tmp;
	rlist_foreach_entry_safe(tracker, &txn->read_set,
				 in_read_set, tmp) {
		rlist_del(&tracker->in_reader_list);
		rlist_del(&tracker->in_read_set);
	}
	assert(rlist_empty(&txn->conflict_list));

	struct tx_conflict_tracker *entry, *next;
	rlist_foreach_entry_safe(entry, &txn->conflict_list,
				 in_conflict_list, next) {
		rlist_del(&entry->in_conflict_list);
		rlist_del(&entry->in_conflicted_by_list);
	}
	rlist_foreach_entry_safe(entry, &txn->conflicted_by_list,
				 in_conflicted_by_list, next) {
		rlist_del(&entry->in_conflict_list);
		rlist_del(&entry->in_conflicted_by_list);
	}
	assert(rlist_empty(&txn->conflict_list));
	assert(rlist_empty(&txn->conflicted_by_list));

	struct txn_stmt *stmt;
	stailq_foreach_entry(stmt, &txn->stmts, next)
		txn_stmt_unref_tuples(stmt);

	/* Truncate region up to struct txn size. */
	region_truncate(&txn->region, sizeof(struct txn));
	stailq_add(&txn_cache, &txn->in_txn_cache);
}

struct txn *
txn_begin(void)
{
	static int64_t tsn = 0;
	assert(! in_txn());
	struct txn *txn = txn_new();
	if (txn == NULL)
		return NULL;
	assert(rlist_empty(&txn->conflict_list));
	assert(rlist_empty(&txn->conflicted_by_list));

	/* Initialize members explicitly to save time on memset() */
	stailq_create(&txn->stmts);
	txn->n_new_rows = 0;
	txn->n_local_rows = 0;
	txn->n_applier_rows = 0;
	txn->flags = 0;
	txn->in_sub_stmt = 0;
	txn->id = ++tsn;
	txn->status = TXN_INPROGRESS;
	txn->signature = -1;
	txn->engine = NULL;
	txn->engine_tx = NULL;
	txn->fk_deferred_count = 0;
	rlist_create(&txn->savepoints);
	txn->fiber = NULL;
	fiber_set_txn(fiber(), txn);
	/* fiber_on_yield is initialized by engine on demand */
	trigger_create(&txn->fiber_on_stop, txn_on_stop, NULL, NULL);
	trigger_add(&fiber()->on_stop, &txn->fiber_on_stop);
	/*
	 * By default all transactions may yield.
	 * It's a responsibility of an engine to disable yields
	 * if they are not supported.
	 */
	txn_set_flag(txn, TXN_CAN_YIELD);
	return txn;
}

int
txn_begin_in_engine(struct engine *engine, struct txn *txn)
{
	if (engine->flags & ENGINE_BYPASS_TX)
		return 0;
	if (txn->engine == NULL) {
		txn->engine = engine;
		return engine_begin(engine, txn);
	} else if (txn->engine != engine) {
		/**
		 * Only one engine can be used in
		 * a multi-statement transaction currently.
		 */
		diag_set(ClientError, ER_CROSS_ENGINE_TRANSACTION);
		return -1;
	}
	return 0;
}

int
txn_begin_stmt(struct txn *txn, struct space *space)
{
	assert(txn == in_txn());
	assert(txn != NULL);
	if (txn->in_sub_stmt > TXN_SUB_STMT_MAX) {
		diag_set(ClientError, ER_SUB_STMT_MAX);
		return -1;
	}
	struct txn_stmt *stmt = txn_stmt_new(&txn->region, space);
	if (stmt == NULL)
		return -1;

	/* Set the savepoint for statement rollback. */
	txn->sub_stmt_begin[txn->in_sub_stmt] = stailq_last(&txn->stmts);
	txn->in_sub_stmt++;
	stailq_add_tail_entry(&txn->stmts, stmt, next);

	if (space == NULL)
		return 0;

	struct engine *engine = space->engine;
	if (txn_begin_in_engine(engine, txn) != 0)
		goto fail;

	stmt->space = space;
	if (engine_begin_statement(engine, txn) != 0)
		goto fail;

	return 0;
fail:
	txn_rollback_stmt(txn);
	return -1;
}

bool
txn_is_distributed(struct txn *txn)
{
	assert(txn == in_txn());
	/**
	 * Transaction has both new and applier rows, and some of
	 * the new rows need to be replicated back to the
	 * server of transaction origin.
	 */
	return (txn->n_new_rows > 0 && txn->n_applier_rows > 0 &&
		txn->n_new_rows != txn->n_local_rows);
}

/**
 * End a statement.
 */
int
txn_commit_stmt(struct txn *txn, struct request *request)
{
	assert(txn->in_sub_stmt > 0);
	/*
	 * Run on_replace triggers. For now, disallow mutation
	 * of tuples in the trigger.
	 */
	struct txn_stmt *stmt = txn_current_stmt(txn);

	/* Create WAL record for the write requests in non-temporary spaces.
	 * stmt->space can be NULL for IRPOTO_NOP.
	 */
	if (stmt->space == NULL || !space_is_temporary(stmt->space)) {
		if (txn_add_redo(txn, stmt, request) != 0)
			goto fail;
		assert(stmt->row != NULL);
		if (stmt->row->replica_id == 0) {
			++txn->n_new_rows;
			if (stmt->row->group_id == GROUP_LOCAL)
				++txn->n_local_rows;

		} else {
			++txn->n_applier_rows;
		}
	}
	/*
	 * If there are triggers, and they are not disabled, and
	 * the statement found any rows, run triggers.
	 * XXX:
	 * - vinyl doesn't set old/new tuple, so triggers don't
	 *   work for it
	 * - perhaps we should run triggers even for deletes which
	 *   doesn't find any rows
	 */
	if (stmt->space != NULL && !rlist_empty(&stmt->space->on_replace) &&
	    stmt->space->run_triggers && (stmt->old_tuple || stmt->new_tuple)) {
		int rc = 0;
		if(!space_is_temporary(stmt->space)) {
			rc = trigger_run(&stmt->space->on_replace, txn);
		} else {
			/*
			 * There is no row attached to txn_stmt for
			 * temporary spaces, since DML operations on them
			 * are not written to WAL. Fake a row to pass operation
			 * type to lua on_replace triggers.
			 */
			assert(stmt->row == NULL);
			struct xrow_header temp_header;
			temp_header.type = request->type;
			stmt->row = &temp_header;
			rc = trigger_run(&stmt->space->on_replace, txn);
			stmt->row = NULL;
		}
		if (rc != 0)
			goto fail;
	}
	--txn->in_sub_stmt;
	return 0;
fail:
	txn_rollback_stmt(txn);
	return -1;
}

/*
 * A helper function to process on_commit triggers.
 */
static void
txn_run_commit_triggers(struct txn *txn, struct rlist *triggers)
{
	/*
	 * Commit triggers must be run in the same order they
	 * were added so that a trigger sees the changes done
	 * by previous triggers (this is vital for DDL).
	 */
	if (trigger_run_reverse(triggers, txn) != 0) {
		/*
		 * As transaction couldn't handle a trigger error so
		 * there is no option except panic.
		 */
		diag_log();
		unreachable();
		panic("commit trigger failed");
	}
}

/*
 * A helper function to process on_rollback triggers.
 */
static void
txn_run_rollback_triggers(struct txn *txn, struct rlist *triggers)
{
	if (trigger_run(triggers, txn) != 0) {
		/*
		 * As transaction couldn't handle a trigger error so
		 * there is no option except panic.
		 */
		diag_log();
		unreachable();
		panic("rollback trigger failed");
	}
}

/**
 * Complete transaction processing.
 */
static void
txn_complete(struct txn *txn)
{
	/*
	 * Note, engine can be NULL if transaction contains
	 * IPROTO_NOP statements only.
	 */
	if (txn->signature < 0) {
		txn->status = TXN_ABORTED;
		/* Undo the transaction. */
		struct txn_stmt *stmt;
		stailq_reverse(&txn->stmts);
		stailq_foreach_entry(stmt, &txn->stmts, next)
			txn_rollback_one_stmt(txn, stmt);
		if (txn->engine)
			engine_rollback(txn->engine, txn);
		if (txn_has_flag(txn, TXN_HAS_TRIGGERS))
			txn_run_rollback_triggers(txn, &txn->on_rollback);
	} else {
		txn->status = TXN_COMMITTED;
		/* Commit the transaction. */
		if (txn->engine != NULL)
			engine_commit(txn->engine, txn);
		if (txn_has_flag(txn, TXN_HAS_TRIGGERS))
			txn_run_commit_triggers(txn, &txn->on_commit);

		double stop_tm = ev_monotonic_now(loop());
		if (stop_tm - txn->start_tm > too_long_threshold) {
			int n_rows = txn->n_new_rows + txn->n_applier_rows;
			say_warn_ratelimited("too long WAL write: %d rows at "
					     "LSN %lld: %.3f sec", n_rows,
					     txn->signature - n_rows + 1,
					     stop_tm - txn->start_tm);
		}
	}
	/*
	 * If there is no fiber waiting for the transaction then
	 * the transaction could be safely freed. In the opposite case
	 * the fiber is in duty to free this transaction.
	 */
	if (txn->fiber == NULL)
		txn_free(txn);
	else {
		txn_set_flag(txn, TXN_IS_DONE);
		if (txn->fiber != fiber())
			/* Wake a waiting fiber up. */
			fiber_wakeup(txn->fiber);
	}
}

void
txn_complete_async(struct journal_entry *entry)
{
	struct txn *txn = entry->complete_data;
	txn->signature = entry->res;
	/*
	 * Some commit/rollback triggers require for in_txn fiber
	 * variable to be set so restore it for the time triggers
	 * are in progress.
	 */
	assert(in_txn() == NULL);
	fiber_set_txn(fiber(), txn);
	txn_complete(txn);
	fiber_set_txn(fiber(), NULL);
}

static struct journal_entry *
txn_journal_entry_new(struct txn *txn)
{
	struct journal_entry *req;
	struct txn_stmt *stmt;

	assert(txn->n_new_rows + txn->n_applier_rows > 0);

	req = journal_entry_new(txn->n_new_rows + txn->n_applier_rows,
				&txn->region, txn);
	if (req == NULL)
		return NULL;

	struct xrow_header **remote_row = req->rows;
	struct xrow_header **local_row = req->rows + txn->n_applier_rows;

	stailq_foreach_entry(stmt, &txn->stmts, next) {
		if (stmt->has_triggers) {
			txn_init_triggers(txn);
			rlist_splice(&txn->on_commit, &stmt->on_commit);
		}

		/* A read (e.g. select) request */
		if (stmt->row == NULL)
			continue;

		if (stmt->row->replica_id == 0)
			*local_row++ = stmt->row;
		else
			*remote_row++ = stmt->row;

		req->approx_len += xrow_approx_len(stmt->row);
	}

	assert(remote_row == req->rows + txn->n_applier_rows);
	assert(local_row == remote_row + txn->n_new_rows);

	return req;
}

/*
 * Prepare a transaction using engines.
 */
static int
txn_prepare(struct txn *txn)
{
	if (txn_has_flag(txn, TXN_IS_ABORTED_BY_YIELD)) {
		assert(!txn_has_flag(txn, TXN_CAN_YIELD));
		diag_set(ClientError, ER_TRANSACTION_YIELD);
		diag_log();
		return -1;
	}
	/*
	 * If transaction has been started in SQL, deferred
	 * foreign key constraints must not be violated.
	 * If not so, just rollback transaction.
	 */
	if (txn->fk_deferred_count != 0) {
		diag_set(ClientError, ER_FOREIGN_KEY_CONSTRAINT);
		return -1;
	}
	/*
	 * Perform transaction conflict resolution. Engine == NULL when
	 * we have a bunch of IPROTO_NOP statements.
	 */
	if (txn->engine != NULL) {
		if (engine_prepare(txn->engine, txn) != 0)
			return -1;
	}
	trigger_clear(&txn->fiber_on_stop);
	if (!txn_has_flag(txn, TXN_CAN_YIELD))
		trigger_clear(&txn->fiber_on_yield);

	if (txn->status == TXN_CONFLITED) {
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		diag_log();
		return -1;
	}

	assert(txn->status == TXN_INPROGRESS);
	struct tx_conflict_tracker *tr;
	rlist_foreach_entry(tr, &txn->conflict_list, in_conflict_list) {
		if (tr->victim->status == TXN_INPROGRESS)
			tr->victim->status = TXN_CONFLITED;
	}

	txn->start_tm = ev_monotonic_now(loop());
	txn->status = TXN_PREPARED;
	return 0;
}

/**
 * Complete transaction early if it is barely nop.
 */
static bool
txn_commit_nop(struct txn *txn)
{
	if (txn->n_new_rows + txn->n_applier_rows == 0) {
		txn->signature = 0;
		txn_complete(txn);
		fiber_set_txn(fiber(), NULL);
		return true;
	}

	return false;
}

int
txn_commit_async(struct txn *txn)
{
	struct journal_entry *req;

	ERROR_INJECT(ERRINJ_TXN_COMMIT_ASYNC, {
		diag_set(ClientError, ER_INJECTION,
			 "txn commit async injection");
		/*
		 * Log it for the testing sake: we grep
		 * output to mark this event.
		 */
		diag_log();
		txn_rollback(txn);
		return -1;
	});

	if (txn_prepare(txn) != 0) {
		txn_rollback(txn);
		return -1;
	}

	if (txn_commit_nop(txn))
		return 0;

	req = txn_journal_entry_new(txn);
	if (req == NULL) {
		txn_rollback(txn);
		return -1;
	}

	fiber_set_txn(fiber(), NULL);
	if (journal_write_async(req) != 0) {
		fiber_set_txn(fiber(), txn);
		txn_rollback(txn);

		diag_set(ClientError, ER_WAL_IO);
		diag_log();
		return -1;
	}

	return 0;
}

int
txn_commit(struct txn *txn)
{
	struct journal_entry *req;

	txn->fiber = fiber();

	if (txn_prepare(txn) != 0) {
		txn_rollback(txn);
		txn_free(txn);
		return -1;
	}

	if (txn_commit_nop(txn)) {
		txn_free(txn);
		return 0;
	}

	req = txn_journal_entry_new(txn);
	if (req == NULL) {
		txn_rollback(txn);
		txn_free(txn);
		return -1;
	}

	fiber_set_txn(fiber(), NULL);
	if (journal_write(req) != 0) {
		fiber_set_txn(fiber(), txn);
		txn_rollback(txn);
		txn_free(txn);

		diag_set(ClientError, ER_WAL_IO);
		diag_log();
		return -1;
	}

	if (!txn_has_flag(txn, TXN_IS_DONE)) {
		txn->signature = req->res;
		txn_complete(txn);
	}

	int res = txn->signature >= 0 ? 0 : -1;
	if (res != 0) {
		diag_set(ClientError, ER_WAL_IO);
		diag_log();
	}

	/* Synchronous transactions are freed by the calling fiber. */
	txn_free(txn);
	return res;
}

void
txn_rollback_stmt(struct txn *txn)
{
	if (txn == NULL || txn->in_sub_stmt == 0)
		return;
	txn->in_sub_stmt--;
	txn_rollback_to_svp(txn, txn->sub_stmt_begin[txn->in_sub_stmt]);
}

void
txn_rollback(struct txn *txn)
{
	assert(txn == in_txn());
	txn->status = TXN_ABORTED;
	trigger_clear(&txn->fiber_on_stop);
	if (!txn_has_flag(txn, TXN_CAN_YIELD))
		trigger_clear(&txn->fiber_on_yield);
	txn->signature = -1;
	txn_complete(txn);
	fiber_set_txn(fiber(), NULL);
}

int
txn_check_singlestatement(struct txn *txn, const char *where)
{
	if (!txn_is_first_statement(txn)) {
		diag_set(ClientError, ER_MULTISTATEMENT_TRANSACTION, where);
		return -1;
	}
	return 0;
}

void
txn_can_yield(struct txn *txn, bool set)
{
	assert(txn == in_txn());
	if (set) {
		assert(!txn_has_flag(txn, TXN_CAN_YIELD));
		txn_set_flag(txn, TXN_CAN_YIELD);
		trigger_clear(&txn->fiber_on_yield);
	} else {
		assert(txn_has_flag(txn, TXN_CAN_YIELD));
		txn_clear_flag(txn, TXN_CAN_YIELD);
		trigger_create(&txn->fiber_on_yield, txn_on_yield, NULL, NULL);
		trigger_add(&fiber()->on_yield, &txn->fiber_on_yield);
	}
}

int64_t
box_txn_id(void)
{
	struct txn *txn = in_txn();
	if (txn != NULL)
		return txn->id;
	else
		return -1;
}

bool
box_txn(void)
{
	return in_txn() != NULL;
}

int
box_txn_begin(void)
{
	if (in_txn()) {
		diag_set(ClientError, ER_ACTIVE_TRANSACTION);
		return -1;
	}
	if (txn_begin() == NULL)
		return -1;
	return 0;
}

int
box_txn_commit(void)
{
	struct txn *txn = in_txn();
	/**
	 * COMMIT is like BEGIN or ROLLBACK
	 * a "transaction-initiating statement".
	 * Do nothing if transaction is not started,
	 * it's the same as BEGIN + COMMIT.
	*/
	if (! txn)
		return 0;
	if (txn->in_sub_stmt) {
		diag_set(ClientError, ER_COMMIT_IN_SUB_STMT);
		return -1;
	}
	int rc = txn_commit(txn);
	fiber_gc();
	return rc;
}

int
box_txn_rollback(void)
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		return 0;
	if (txn && txn->in_sub_stmt) {
		diag_set(ClientError, ER_ROLLBACK_IN_SUB_STMT);
		return -1;
	}
	txn_rollback(txn); /* doesn't throw */
	fiber_gc();
	return 0;
}

void *
box_txn_alloc(size_t size)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		/* There are no transaction yet - return an error. */
		diag_set(ClientError, ER_NO_TRANSACTION);
		return NULL;
	}
	union natural_align {
		void *p;
		double lf;
		long l;
	};
	return region_aligned_alloc(&txn->region, size,
	                            alignof(union natural_align));
}

struct txn_savepoint *
txn_savepoint_new(struct txn *txn, const char *name)
{
	assert(txn == in_txn());
	int name_len = name != NULL ? strlen(name) : 0;
	struct txn_savepoint *svp;
	static_assert(sizeof(svp->name) == 1,
		      "name member already has 1 byte for 0 termination");
	size_t size = sizeof(*svp) + name_len;
	svp = (struct txn_savepoint *)region_aligned_alloc(&txn->region, size,
							   alignof(*svp));
	if (svp == NULL) {
		diag_set(OutOfMemory, size, "region_aligned_alloc", "svp");
		return NULL;
	}
	svp->stmt = stailq_last(&txn->stmts);
	svp->in_sub_stmt = txn->in_sub_stmt;
	svp->fk_deferred_count = txn->fk_deferred_count;
	if (name != NULL) {
		/*
		 * If savepoint with given name already exists,
		 * erase it from the list. This has to be done
		 * in accordance with ANSI SQL compliance.
		 */
		struct txn_savepoint *old_svp =
			txn_savepoint_by_name(txn, name);
		if (old_svp != NULL)
			rlist_del(&old_svp->link);
		memcpy(svp->name, name, name_len + 1);
	} else {
		svp->name[0] = 0;
	}
	rlist_add_entry(&txn->savepoints, svp, link);
	return svp;
}

struct txn_savepoint *
txn_savepoint_by_name(struct txn *txn, const char *name)
{
	assert(txn == in_txn());
	struct txn_savepoint *sv;
	rlist_foreach_entry(sv, &txn->savepoints, link) {
		if (strcmp(sv->name, name) == 0)
			return sv;
	}
	return NULL;
}

box_txn_savepoint_t *
box_txn_savepoint(void)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		diag_set(ClientError, ER_NO_TRANSACTION);
		return NULL;
	}
	return txn_savepoint_new(txn, NULL);
}

int
box_txn_rollback_to_savepoint(box_txn_savepoint_t *svp)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		diag_set(ClientError, ER_NO_TRANSACTION);
		return -1;
	}
	struct txn_stmt *stmt = svp->stmt == NULL ? NULL :
			stailq_entry(svp->stmt, struct txn_stmt, next);
	if (stmt != NULL && stmt->space == NULL && stmt->row == NULL) {
		/*
		 * The statement at which this savepoint was
		 * created has been rolled back.
		 */
		diag_set(ClientError, ER_NO_SUCH_SAVEPOINT);
		return -1;
	}
	if (svp->in_sub_stmt != txn->in_sub_stmt) {
		diag_set(ClientError, ER_NO_SUCH_SAVEPOINT);
		return -1;
	}
	txn_rollback_to_svp(txn, svp->stmt);
	/* Discard from list all newer savepoints. */
	RLIST_HEAD(discard);
	rlist_cut_before(&discard, &txn->savepoints, &svp->link);
	txn->fk_deferred_count = svp->fk_deferred_count;
	return 0;
}

void
txn_savepoint_release(struct txn_savepoint *svp)
{
	struct txn *txn = in_txn();
	assert(txn != NULL);
	/* Make sure that savepoint hasn't been released yet. */
	struct txn_stmt *stmt = svp->stmt == NULL ? NULL :
				stailq_entry(svp->stmt, struct txn_stmt, next);
	assert(stmt == NULL || (stmt->space != NULL && stmt->row != NULL));
	(void) stmt;
	/*
	 * Discard current savepoint alongside with all
	 * created after it savepoints.
	 */
	RLIST_HEAD(discard);
	rlist_cut_before(&discard, &txn->savepoints, rlist_next(&svp->link));
}

static int
txn_on_stop(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	txn_rollback(in_txn());                 /* doesn't yield or fail */
	fiber_gc();
	return 0;
}

/**
 * Memtx yield-in-transaction trigger callback.
 *
 * In case of a yield inside memtx multi-statement transaction
 * we must, first of all, roll back the effects of the transaction
 * so that concurrent transactions won't see dirty, uncommitted
 * data.
 *
 * Second, we must abort the transaction, since it has been rolled
 * back implicitly. The transaction can not be rolled back
 * completely from within a yield trigger, since a yield trigger
 * can't fail. Instead, we mark the transaction as aborted and
 * raise an error on attempt to commit it.
 *
 * So much hassle to be user-friendly until we have a true
 * interactive transaction support in memtx.
 */
static int
txn_on_yield(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	struct txn *txn = in_txn();
	assert(txn != NULL);
	assert(!txn_has_flag(txn, TXN_CAN_YIELD));
	txn_rollback_to_svp(txn, NULL);
	txn_set_flag(txn, TXN_IS_ABORTED_BY_YIELD);
	return 0;
}

void
tx_manager_init()
{
	mempool_create(&tx_manager_core.tx_value_pool,
		       cord_slab_cache(), sizeof(struct tx_value));
	tx_manager_core.values = mh_value_new();
}

void
tx_manager_free()
{
	mh_value_delete(tx_manager_core.values);
}

int
tx_cause_conflict(struct txn *wreaker, struct txn *victim)
{
	struct tx_conflict_tracker *tracker = NULL;
	struct rlist *r1 = wreaker->conflict_list.next;
	struct rlist *r2 = wreaker->conflicted_by_list.next;
	while (r1 != &wreaker->conflict_list &&
	       r2 != &wreaker->conflicted_by_list) {
		tracker = rlist_entry(r1, struct tx_conflict_tracker,
				      in_conflict_list);
		if (tracker->wreaker == wreaker && tracker->victim == victim)
			break;
		tracker = rlist_entry(r2, struct tx_conflict_tracker,
				      in_conflicted_by_list);
		if (tracker->wreaker == wreaker && tracker->victim == victim)
			break;
		tracker = NULL;
		r1 = r1->next;
		r2 = r2->next;
	}
	if (tracker != NULL) {
		/* Move to the beginning of a list
		 * for a case of subsequent lookups */
		rlist_del(&tracker->in_conflict_list);
		rlist_del(&tracker->in_conflicted_by_list);
	} else {
		size_t size;
		tracker = region_alloc_object(&victim->region,
					      struct tx_conflict_tracker,
					      &size);
		if (tracker == NULL) {
			diag_set(OutOfMemory, size, "tx region",
				 "conflict_tracker");
			return -1;
		}
	}
	tracker->wreaker = wreaker;
	tracker->victim = victim;
	rlist_add(&wreaker->conflict_list, &tracker->in_conflict_list);
	rlist_add(&wreaker->conflicted_by_list, &tracker->in_conflicted_by_list);
	return 0;
}

static bool
tx_can_see(const struct txn_stmt *stmt, bool prepared_ok)
{
	return (stmt->txn_owner->status == TXN_PREPARED && prepared_ok) ||
	       (stmt->txn_owner->status == TXN_COMMITTED);
}

struct tuple *
tx_manager_tuple_clarify_slow(struct tuple *tuple, uint32_t index,
			      uint32_t mk_index, bool prepared_ok)
{
	struct txn *txn = in_txn();
	struct tuple *result = NULL;
	bool own_change = false;
	while (true) {
		struct mh_value_t *ht = tx_manager_core.values;
		mh_int_t pos = mh_value_find(ht, tuple, 0);
		assert(pos != mh_end(ht));
		struct tx_value *value = mh_value_node(ht, pos);
		assert(value->add_stmt != NULL ||
		       value->del_stmt != NULL);

		if (value->del_stmt != NULL) {
			struct txn_stmt *stmt = value->del_stmt;
			assert(tuple == stmt->old_tuple);

			if (txn != NULL && stmt->txn_owner == txn) {
				own_change = true;
				break;
			} else if (tx_can_see(stmt, prepared_ok)) {
				result = tuple;
				break;
			}
		}

		if (value->add_stmt != NULL) {
			struct txn_stmt *stmt = value->add_stmt;
			assert(tuple == stmt->new_tuple);
			if (txn != NULL && stmt->txn_owner == txn) {
				own_change = true;
				result = tuple;
			} else if (tx_can_see(stmt, prepared_ok)) {
				result = tuple;
				break;
			}

			tuple = value->add_stmt->track[index];
		}
	}
	if (!own_change)
		tx_track_read(result);
	(void)mk_index; /* TODO: multiindex */
	return result;
}

static struct tx_value *
tx_value_new(struct tuple *tuple)
{
	struct mempool *pool = &tx_manager_core.tx_value_pool;
	struct tx_value *value = (struct tx_value *)mempool_alloc(pool);
	if (value == NULL) {
		diag_set(OutOfMemory, sizeof(struct tx_value),
			 "tx_manager", "tx track value");
		return NULL;
	}
	value->tuple = tuple;
	value->add_stmt = NULL;
	value->del_stmt = NULL;
	rlist_create(&value->reader_list);
	return value;
}

static void
tx_value_delete(struct tx_value *value)
{
	struct mempool *pool = &tx_manager_core.tx_value_pool;
	mempool_free(pool, value);
}

static struct tx_value *
tx_value_get(struct tuple *tuple)
{
	struct mh_value_t *ht = tx_manager_core.values;
	struct tx_value *value;
	if (tuple_is_dirty(tuple)) {
		mh_int_t pos = mh_value_find(ht, tuple, 0);
		assert(pos != mh_end(ht));
		value = mh_value_node(ht, pos);
		assert(value->tuple == tuple);
	} else {
		value = tx_value_new(tuple);
		if (value == NULL)
			return NULL;
		struct tx_value *empty;
		mh_int_t pos = mh_value_put(ht, value, &empty, 0);
		if (pos == mh_end(ht)) {
			tx_value_delete(value);
			diag_set(OutOfMemory, pos + 1,
				 "tx_manager", "tx track hash table");
			return NULL;
		}
		tuple_set_dirty(tuple);
	}
	return value;
}

static void
tx_value_check(struct tx_value *value)
{
	if (value->add_stmt != NULL || value->del_stmt != NULL ||
	    !rlist_empty(&value->reader_list))
		return;
	struct mh_value_t *ht = tx_manager_core.values;
	mh_int_t pos = mh_value_find(ht, value->tuple, 0);

	assert(pos != mh_end(ht));
	assert(mh_value_node(ht, pos) == value);
	mh_value_del(ht, pos, 0);

	assert(tuple_is_dirty(value->tuple));
	tuple_set_clean(value->tuple);

	tx_value_delete(value);
}

int
tx_track_read(struct tuple *tuple)
{
	if (tuple == NULL)
		return 0;
	struct txn *txn = in_txn();
	if (txn == NULL)
		return 0;

	struct tx_value *value = tx_value_get(tuple);
	if (value == NULL)
		return -1;

	struct tx_read_tracker *tracker = NULL;

	struct rlist *r1 = value->reader_list.next;
	struct rlist *r2 = txn->read_set.next;
	while (r1 != &value->reader_list && r2 != &txn->read_set) {
		tracker = rlist_entry(r1, struct tx_read_tracker,
				      in_reader_list);
		if (tracker->reader == txn)
			break;
		tracker = rlist_entry(r2, struct tx_read_tracker,
				      in_read_set);
		if (tracker->value == value)
			break;
		tracker = NULL;
		r1 = r1->next;
		r2 = r2->next;
	}
	if (tracker != NULL) {
		/* Move to the beginning of a list
		 * for a case of subsequent lookups */
		rlist_del(&tracker->in_reader_list);
		rlist_del(&tracker->in_read_set);
	} else {
		size_t size;
		tracker = region_alloc_object(&txn->region,
					      struct tx_read_tracker,
					      &size);
		if (tracker == NULL) {
			diag_set(OutOfMemory, size, "tx region",
				 "read_tracker");
			tx_value_check(value);
			return -1;
		}
		tracker->reader = txn;
	}
	rlist_add(&value->reader_list, &tracker->in_reader_list);
	return 0;
}

int
tx_track_slow(struct tuple *tuple, struct txn_stmt *stmt, bool add_or_del)
{
	struct tx_value *value = tx_value_get(tuple);
	if (value == NULL)
		return -1;

	if (add_or_del) {
		assert(value->add_stmt == NULL);
		value->add_stmt = stmt;
	} else {
		assert(value->del_stmt == NULL);
		value->del_stmt = stmt;
	}

	return 0;
}

void
tx_untrack_slow(struct tuple *tuple, struct txn_stmt *stmt, bool add_or_del)
{
	assert(tuple_is_dirty(tuple));
	struct tx_value *value = tx_value_get(tuple);
	assert(value != NULL);
	assert(value->tuple == tuple);

	if (add_or_del) {
		assert(value->add_stmt == stmt); (void)stmt;
		value->add_stmt = NULL;
	} else {
		assert(value->del_stmt == stmt); (void)stmt;
		value->del_stmt = NULL;
	}

	tx_value_check(value);
}
