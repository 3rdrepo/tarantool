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

static uint32_t
txm_story_key_hash(const struct tuple *a)
{
	uintptr_t u = (uintptr_t)a;
	if (sizeof(uintptr_t) <= sizeof(uint32_t))
		return u;
	else
		return u ^ (u >> 32);
}

#define mh_name _history
#define mh_key_t struct tuple *
#define mh_node_t struct txm_story
#define mh_arg_t int
#define mh_hash(a, arg) (txm_story_key_hash((a)->tuple))
#define mh_hash_key(a, arg) (txm_story_key_hash(a))
#define mh_cmp(a, b, arg) ((a)->tuple != (b)->tuple)
#define mh_cmp_key(a, b, arg) ((a) != (b)->tuple)
#define MH_SOURCE
#include "salad/mhash.h"

struct tx_manager
{
	/** Last prepare-sequence-number that was assigned to preped TX. */
	int64_t last_psn;
	/** Mempools for tx_story objects with difference index count. */
	struct mempool txm_story_pool[BOX_INDEX_MAX];
	/** Hash table tuple -> txm_story of that tuple. */
	struct mh_history_t *history;
	/** List of all txm_story objects. */
	struct rlist all_stories;
	/** Iterator that sequentially traverses all txm_story objects. */
	struct rlist *traverse_all_stories;
};

/** The one and only instance of tx_manager. */
static struct tx_manager txm;

struct tx_read_tracker {
	struct txn *reader;
	struct txm_story *story;
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

static struct tx_value *
tx_value_get(struct tuple *tuple);

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
	size_t history_size = space == NULL ? 0 :
			      2 * space->index_count * sizeof(struct tuple *);
	size_t stmt_size = sizeof(struct txn_stmt) + history_size;
	struct txn_stmt *stmt = (struct txn_stmt *)
		region_aligned_alloc(region, stmt_size,
				     alignof(struct txn_stmt));
	if (stmt == NULL) {
		diag_set(OutOfMemory, stmt_size, "region_alloc_object", "stmt");
		return NULL;
	}

	/* Initialize members explicitly to save time on memset() */
	stmt->txn = in_txn();
	stmt->space = NULL;
	stmt->old_tuple = NULL;
	stmt->new_tuple = NULL;
	stmt->add_story = NULL;
	stmt->del_story = NULL;
	stmt->next_in_del_list = NULL;
	stmt->engine_savepoint = NULL;
	stmt->row = NULL;
	stmt->has_triggers = false;
	stmt->preserve_old_tuple = false;
	stmt->index_count = space == NULL ? 0 : space->index_count;
	rlist_create(&stmt->in_value_delete_list);
	memset(stmt->history, 0, history_size);
	return stmt;
}

static inline void
txn_stmt_destroy(struct txn_stmt *stmt)
{
	if (stmt->new_tuple != NULL)
		tx_untrack(stmt->new_tuple, stmt, true);
	if (*txn_stmt_history_pred(stmt, 0) != NULL)
		tx_untrack(*txn_stmt_history_pred(stmt, 0), stmt, false);
	else if (stmt->old_tuple != NULL)
		tx_untrack(stmt->old_tuple, stmt, false);

	if (stmt->old_tuple != NULL)
		tuple_unref(stmt->old_tuple);
	if (stmt->new_tuple != NULL)
		tuple_unref(stmt->new_tuple);
	for (size_t i = 0; i < 2 * stmt->index_count; i++)
		if (stmt->history[i] != NULL)
			tuple_unref(stmt->history[i]);
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
		txn_stmt_destroy(stmt);
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
		txn_stmt_destroy(stmt);

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

	/*
	 * A conflict have happened; there is no reason to continue the TX.
	 */
	if (txn->status == TXN_CONFLICTED) {
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
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
		/* Triggers see old_tuple and that tuple must remain the same */
		stmt->preserve_old_tuple = true;
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
	txn->psn = ++txm.last_psn;

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
	 * Somebody else has written some value that we have read.
	 * The transaction is not possible.
	 */
	if (txn->status == TXN_CONFLICTED) {
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
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

	struct tx_conflict_tracker *entry, *next;
	rlist_foreach_entry_safe(entry, &txn->conflict_list,
				 in_conflict_list, next) {
		if (entry->victim->status == TXN_INPROGRESS)
			entry->victim->status = TXN_CONFLICTED;
		rlist_del(&entry->in_conflict_list);
		rlist_del(&entry->in_conflicted_by_list);
	}

	trigger_clear(&txn->fiber_on_stop);
	if (!txn_has_flag(txn, TXN_CAN_YIELD))
		trigger_clear(&txn->fiber_on_yield);

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
	for (size_t i = 0; i < BOX_INDEX_MAX; i++) {
		size_t item_size = sizeof(struct txm_story) +
			i * sizeof(struct txm_story_link);
		mempool_create(&txm.txm_story_pool[i],
			       cord_slab_cache(), item_size);
	}
	txm.history = mh_history_new();
	txm.traverse_all_stories = &txm.all_stories;
}

void
tx_manager_free()
{
	mh_history_delete(txm.history);
	for (size_t i = 0; i < BOX_INDEX_MAX; i++)
		mempool_destroy(&txm.txm_story_pool[i]);
}

int
txm_cause_conflict(struct txn *wreaker, struct txn *victim)
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

struct txm_story *
txm_story_new(struct tuple *tuple, struct txn_stmt *stmt, uint32_t index_count)
{
	assert(!tuple->is_dirty);
	assert(index_count < BOX_INDEX_MAX);
	struct mempool *pool = &txm.txm_story_pool[index_count];
	struct txm_story *story = (struct txm_story *)mempool_alloc(pool);
	if (story == NULL) {
		size_t item_size = sizeof(struct txm_story) +
			index_count * sizeof(struct txm_story_link);
		diag_set(OutOfMemory, item_size,
			 "tx_manager", "tx story");
		return story;
	}
	story->tuple = tuple;

	struct txm_story *empty;
	mh_int_t pos = mh_history_put(txm.history, story, &empty, 0);
	if (pos == mh_end(txm.history)) {
		mempool_free(pool, story);
		diag_set(OutOfMemory, pos + 1,
			 "tx_manager", "tx history hash table");
		return NULL;
	}
	tuple->is_dirty = true;
	tuple_ref(tuple);

	story->index_count = index_count;
	story->add_stmt = stmt;
	story->add_psn = 0;
	story->del_stmt = NULL;
	story->del_psn = 0;
	rlist_create(&story->reader_list);
	rlist_add(&txm.all_stories, &story->in_all_stories);
	memset(story->link, 0, sizeof(story->link[0]) * index_count);
	return story;
}

/** Temporary allocated on region that stores a conflicting TX. */
struct txn_conflict
{
	struct txn *wreaker;
	struct txn_conflict *next;
};

int
txm_check_and_link_add_story(struct txm_story *story, struct txn_stmt *stmt,
			     enum dup_replace_mode mode)
{
	for (uint32_t i = 0; i < story->index_count; i++) {
		assert(!story->link[i].is_old_story);
		struct tuple *next_tuple = story->link[i].old_tuple;
		if (next_tuple != NULL && next_tuple->is_dirty) {
			struct txm_story *next = txm_story_get(next_tuple);
			assert(next->link[i].new_story == NULL);
			story->link[i].is_old_story = true;
			story->link[i].old_story = next;
			next->link[i].new_story = story;
		}
	}

	struct region *region = &stmt->txn->region;
	size_t region_svp = region_used(region);
	struct txn_conflict *collected_conflicts = NULL;

	for (uint32_t i = 0; i < story->index_count; i++) {
		struct tuple *visible = NULL;
		struct txm_story *node = story;
		while (true) {
			if (!node->link[i].is_old_story) {
				/*
				 * the tuple is so old that we doesn't
				 * know its story.
				 */
				visible = node->link[i].old_tuple;
				assert(visible == NULL || !visible->is_dirty);
				break;
			}
			node = node->link[i].old_story;

			if (node->del_psn != 0) {
				/* deleted by at least prepared TX. */
				break;
			}
			if (node->del_stmt != NULL &&
			    node->del_stmt->txn == stmt->txn)
				break; /* deleted by us. */
			if (node->add_psn != 0) {
				/* added by at least prepared TX. */
				visible = node->tuple;
				break;
			}
			if (node->add_stmt == NULL) {
				/*
				 * the tuple is so old that we lost
				 * the beginning of its story.
				 */
				visible = node->tuple;
				break;
			}
			if (node->add_stmt->txn == stmt->txn) {
				/* added by us. */
				visible = node->tuple;
				break;
			}
			/*
			 * we skip the story but once the story is committed
			 * before out TX that will mean that we shouldn't
			 * skip it and our TX will be conflicted.
			 */
			size_t err_size;
			struct txn_conflict *next_conflict;
			next_conflict =
				region_alloc_object(region,
						    struct txn_conflict,
						    &err_size);
			if (next_conflict == NULL) {
				diag_set(OutOfMemory, err_size,
					 "txn_region", "txn conflict");
				goto fail;
			}
			next_conflict->wreaker = node->add_stmt->txn;
			next_conflict->next = collected_conflicts;
			collected_conflicts = next_conflict;
		}

		int errcode;
		errcode = replace_check_dup(stmt->old_tuple, visible,
					    i == 0 ? mode : DUP_INSERT);
		if (errcode != 0) {
			struct space *sp = stmt->space;
			if (sp != NULL)
				diag_set(ClientError, errcode,
					 sp->index[i]->def->name,
					 space_name(sp));
			goto fail;
		}
	}

	if (story->link[0].is_old_story) {
		stmt->next_in_del_list = story->link[0].old_story->del_stmt;
		story->link[0].old_story->del_stmt = stmt;
		for (uint32_t i = 0; i < story->index_count; i++) {
			if (story->link[i].is_old_story)
				continue;
			if (story->link[i].old_tuple != NULL)
				tuple_ref(story->link[i].old_tuple);
		}
	} else if (story->link[0].old_tuple != NULL) {
		struct tuple *old_tuple = story->link[0].old_tuple;
		struct txm_story *del_story;
		del_story = txm_story_new(old_tuple, NULL, story->index_count);
		if (del_story == NULL)
			goto fail;
		del_story->del_stmt = stmt;
		for (uint32_t i = 0; i < story->index_count; i++) {
			if (story->link[i].is_old_story)
				continue;
			if (story->link[i].old_tuple == old_tuple) {
				story->link[i].is_old_story = true;
				story->link[i].old_story = del_story;
			} else if (story->link[i].old_tuple != NULL) {
				tuple_ref(story->link[i].old_tuple);
			}
		}
	}

	while (collected_conflicts != NULL) {
		if (txm_cause_conflict(collected_conflicts->wreaker,
				       stmt->txn) != 0) {
			goto fail;
		}
		collected_conflicts = collected_conflicts->next;
	}
	stmt->add_story = story;

	region_truncate(region, region_svp);
	return 0;

fail:
	for (uint32_t j = story->index_count; j > 0; j--) {
		uint32_t i = j - 1;
		if (story->link[i].is_old_story) {
			struct txm_story *next = story->link[i].old_story;
			story->link[i].is_old_story = false;
			story->link[i].old_tuple = next->tuple;
			next->link[i].new_story = NULL;
		}
	}
	region_truncate(region, region_svp);
	return -1;
}

int
txm_link_del_story(struct tuple *old_tuple, struct txn_stmt *stmt,
		   uint32_t index_count)
{
	if (old_tuple->is_dirty) {
		struct txm_story *story = txm_story_get(old_tuple);
		stmt->next_in_del_list = story->del_stmt;
		story->del_stmt = stmt;
		stmt->del_story = story;
		return 0;
	}
	struct txm_story *del_story;
	del_story = txm_story_new(old_tuple, NULL, index_count);
	if (del_story == NULL)
		return -1;
	del_story->del_stmt = stmt;
	stmt->del_story = del_story;
	return 0;
}

void
txm_unlink_add_story(struct txn_stmt *stmt)
{
	assert(stmt->add_story != NULL);

	struct txm_story *story = stmt->add_story;

	for (uint32_t i = 0; i < story->index_count; i++) {
		struct txm_story_link *from = &story->link[i];
		if (from->new_story == NULL) {
			struct tuple *unused;
			struct index *index = stmt->space->index[i];
			struct tuple *rollback = from->is_old_story ?
					 from->old_story->tuple :
					 from->old_tuple;
			if (index_replace(index, story->tuple, rollback,
					  DUP_INSERT, &unused) != 0) {
				diag_log();
				unreachable();
				panic("failed to rollback change");
			}
			if (i == 0 && rollback != NULL)
				tuple_ref(rollback);
		} else {
			struct txm_story *new_story = from->new_story;
			struct txm_story_link *to = &new_story->link[i];
			assert(to->is_old_story);
			assert(to->old_story == story);
			to->is_old_story = from->is_old_story;
			if (from->is_old_story) {
				to->old_story = from->old_story;
				from->old_story = NULL;
			} else {
				to->old_tuple = from->old_tuple;
				from->old_tuple = NULL;
			}
			from->is_old_story = false;
		}
	}

	stmt->add_story = NULL;
}

void
txm_unlink_del_story(struct txn_stmt *stmt)
{
	assert(stmt->del_story != NULL);

	struct txm_story *story = stmt->del_story;

	stmt->del_story = NULL;
}

struct txm_story *
txm_story_get(struct tuple *tuple)
{
	assert(tuple->is_dirty);

	mh_int_t pos = mh_history_find(txm.history, tuple, 0);
	assert(pos != mh_end(txm.history));
	return mh_history_node(txm.history, pos);
}

void
txm_story_delete(struct txm_story *story)
{
	if (txm.traverse_all_stories == &story->in_all_stories)
		txm.traverse_all_stories = rlist_next(txm.traverse_all_stories);
	rlist_del(&story->in_all_stories);
	tuple_unref(story->tuple);
	story->tuple->is_dirty = false;

	for (uint32_t i = 0; i < story->index_count; i++) {
		if (!story->link[i].is_old_story &&
		    story->link[i].old_tuple != NULL) {
			tuple_unref(story->link[i].old_tuple);
		}
	}

#ifndef NDEBUG
	const char poison_char = '?';
	size_t item_size = sizeof(struct txm_story) +
		story->index_count * sizeof(struct txm_story_link);
	memset(story, poison_char, item_size);
#endif

	struct mempool *pool = &txm.txm_story_pool[story->index_count];
	mempool_free(pool, story);
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

		struct txn_stmt *del_stmt;
		rlist_foreach_entry(del_stmt, &value->delete_stmt_list,
				    in_value_delete_list) {
			if (txn != NULL && del_stmt->txn_owner == txn) {
				own_change = true;
				break;
			} else if (tx_can_see(del_stmt, prepared_ok)) {
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

			tuple = *txn_stmt_history_pred(value->add_stmt, index);
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
	rlist_create(&value->delete_stmt_list);
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
		tuple_ref(tuple);
		tuple_set_dirty(tuple);
	}
	return value;
}

static void
tx_value_check(struct tx_value *value)
{
	if (value->add_stmt != NULL || value->del_stmt != NULL ||
	    !rlist_empty(&value->reader_list) ||
	    !rlist_empty(&value->delete_stmt_list))
		return;
	struct mh_value_t *ht = tx_manager_core.values;
	mh_int_t pos = mh_value_find(ht, value->tuple, 0);

	assert(pos != mh_end(ht));
	assert(mh_value_node(ht, pos) == value);
	mh_value_del(ht, pos, 0);

	assert(tuple_is_dirty(value->tuple));
	tuple_set_clean(value->tuple);
	tuple_unref(value->tuple);

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
tx_track(struct tuple *tuple, struct txn_stmt *stmt, bool add_or_del)
{
	struct tx_value *value = tx_value_get(tuple);
	if (value == NULL)
		return -1;

	if (add_or_del) {
		assert(value->add_stmt == NULL);
		value->add_stmt = stmt;
	} else {
		rlist_add(&value->delete_stmt_list, &stmt->in_value_delete_list);
	}

	return 0;
}

void
tx_untrack(struct tuple *tuple, struct txn_stmt *stmt, bool add_or_del)
{
	assert(tuple_is_dirty(tuple));
	struct tx_value *value = tx_value_get(tuple);
	assert(value != NULL);
	assert(value->tuple == tuple);

	if (add_or_del) {
		assert(value->add_stmt == stmt); (void)stmt;
		value->add_stmt = NULL;
	} else {
		rlist_del(&stmt->in_value_delete_list);
		if (value->del_stmt == stmt)
			value->del_stmt = NULL;
	}

	tx_value_check(value);
}

void
tx_track_succ_slow(struct tuple *pred, struct tuple *succ, size_t index)
{
	assert(tuple_is_dirty(pred));
	assert(tuple_is_dirty(succ));
	struct tx_value *value = tx_value_get(pred);
	assert(value != NULL);
	*txn_stmt_history_succ(value->add_stmt, index) = succ;
	tuple_ref(succ);
}

void
tx_history_link(struct txn_stmt *stmt)
{
	assert(stmt->new_tuple != NULL);
	for (size_t i = 0; i < stmt->index_count; i++) {
		struct tuple **pred = txn_stmt_history_pred(stmt, i);
		if (*pred == NULL || !tuple_is_dirty(*pred))
			continue;
		struct tx_value *value = tx_value_get(*pred);
		assert(value != NULL);
		assert(value->add_stmt != NULL);
		*txn_stmt_history_succ(value->add_stmt, i) = stmt->new_tuple;
	}
}

void
tx_history_unlink(struct txn_stmt *stmt)
{
	assert(stmt->new_tuple != NULL);
	for (size_t i = 0; i < stmt->index_count; i++) {
		struct tuple **succ = txn_stmt_history_pred(stmt, i);
		struct tuple **pred = txn_stmt_history_pred(stmt, i);

		if (*succ != NULL) {
			assert(tuple_is_dirty(*succ));
			struct tx_value *value = tx_value_get(*succ);
			assert(value->add_stmt != NULL);
			*txn_stmt_history_pred(value->add_stmt, i) =
				*txn_stmt_history_pred(stmt, i);
		}

		if (*pred != NULL && tuple_is_dirty(*pred)) {
			struct tx_value *value = tx_value_get(*pred);
			assert(value->add_stmt != NULL);
			*txn_stmt_history_succ(value->add_stmt, i) =
				*txn_stmt_history_succ(stmt, i);
		}
	}
}

int
tx_history_prepare(struct txn_stmt *stmt)
{
	if (stmt->new_tuple == NULL) {
		assert(stmt->old_tuple != NULL);
		assert(tuple_is_dirty(stmt->old_tuple));
		struct tx_value *value = tx_value_get(stmt->old_tuple);

		struct txn_stmt *other_stmt, *tmp;
		rlist_foreach_entry_safe(other_stmt, &value->delete_stmt_list,
					 in_value_delete_list, tmp) {
			if (other_stmt != stmt)
				other_stmt->txn_owner->status = TXN_CONFLICTED;
			rlist_del(&other_stmt->in_value_delete_list);
		}
		value->del_stmt = stmt;
	} else {
		struct tuple *pred = *txn_stmt_history_pred(stmt, 0);
		while (pred != NULL && tuple_is_dirty(pred)) {
			struct tx_value *value = tx_value_get(pred);
			if (value->add_stmt == NULL || value->add_stmt == stmt)
				break;
			if (value->add_stmt->txn_owner->status = TXN_PREPARED)
				break;
		}
	}
}

void
tx_prepare_cause_conflicts(struct txn_stmt *stmt)
{
}
