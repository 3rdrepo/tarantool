/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
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
#include "txn_limbo.h"
#include "replication.h"

struct txn_limbo txn_limbo;

static inline void
txn_limbo_create(struct txn_limbo *limbo)
{
	rlist_create(&limbo->queue);
	limbo->instance_id = REPLICA_ID_NIL;
	vclock_create(&limbo->vclock);
}

struct txn_limbo_entry *
txn_limbo_append(struct txn_limbo *limbo, uint32_t id, struct txn *txn)
{
	assert(txn_has_flag(txn, TXN_WAIT_ACK));
	if (id == 0)
		id = instance_id;
	if (limbo->instance_id != id) {
		if (limbo->instance_id == REPLICA_ID_NIL ||
		    rlist_empty(&limbo->queue)) {
			limbo->instance_id = id;
		} else {
			diag_set(ClientError, ER_UNCOMMITTED_FOREIGN_SYNC_TXNS,
				 limbo->instance_id);
			return NULL;
		}
	}
	size_t size;
	struct txn_limbo_entry *e = region_alloc_object(&txn->region,
							typeof(*e), &size);
	if (e == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_object", "e");
		return NULL;
	}
	e->txn = txn;
	e->lsn = -1;
	e->ack_count = 0;
	e->is_commit = false;
	e->is_rollback = false;
	rlist_add_tail_entry(&limbo->queue, e, in_queue);
	return e;
}

static inline void
txn_limbo_remove(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	assert(!rlist_empty(&entry->in_queue));
	assert(rlist_first_entry(&limbo->queue, struct txn_limbo_entry,
				 in_queue) == entry);
	(void) limbo;
	rlist_del_entry(entry, in_queue);
}

void
txn_limbo_abort(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	entry->is_rollback = true;
	txn_limbo_remove(limbo, entry);
}

void
txn_limbo_assign_lsn(struct txn_limbo *limbo, struct txn_limbo_entry *entry,
		     int64_t lsn)
{
	assert(limbo->instance_id != REPLICA_ID_NIL);
	entry->lsn = lsn;
	++entry->ack_count;
	vclock_follow(&limbo->vclock, limbo->instance_id, lsn);
}

static bool
txn_limbo_check_complete(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	struct vclock_iterator iter;
	vclock_iterator_init(&iter, &limbo->vclock);
	int ack_count = 0;
	int64_t lsn = entry->lsn;
	vclock_foreach(&iter, vc)
		ack_count += vc.lsn >= lsn;
	assert(ack_count >= entry->ack_count);
	entry->ack_count = ack_count;
	entry->is_commit = ack_count >= replication_synchro_quorum;
	return entry->is_commit;
}

void
txn_limbo_wait_complete(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	struct txn *txn = entry->txn;
	assert(entry->lsn > 0);
	assert(!txn_has_flag(txn, TXN_IS_DONE));
	assert(txn_has_flag(txn, TXN_WAIT_ACK));
	if (txn_limbo_check_complete(limbo, entry)) {
		txn_limbo_remove(limbo, entry);
		return;
	}
	bool cancellable = fiber_set_cancellable(false);
	while (!txn_limbo_entry_is_complete(entry))
		fiber_yield();
	fiber_set_cancellable(cancellable);
	// TODO: implement rollback.
	assert(!entry->is_rollback);
	assert(entry->is_commit);
	txn_limbo_remove(limbo, entry);
	txn_clear_flag(txn, TXN_WAIT_ACK);
}

/**
 * Write a confirmation entry to WAL. After it's written all the
 * transactions waiting for confirmation may be finished.
 */
static int
txn_limbo_write_confirm(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	struct xrow_header row;
	struct request request = {
		.header = &row,
	};

	if (xrow_encode_confirm(&row, limbo->instance_id, entry->lsn) < 0)
		return -1;

	struct txn *txn = txn_begin();
	if (txn == NULL)
		return -1;

	if (txn_begin_stmt(txn, NULL) != 0)
		goto rollback;
	if (txn_commit_stmt(txn, &request) != 0)
		goto rollback;

	return txn_commit(txn);
rollback:
	txn_rollback(txn);
	return -1;
}

void
txn_limbo_read_confirm(struct txn_limbo *limbo, int64_t lsn)
{
	assert(limbo->instance_id != REPLICA_ID_NIL &&
	       limbo->instance_id != instance_id);
	struct txn_limbo_entry *e, *tmp;
	rlist_foreach_entry_safe(e, &limbo->queue, in_queue, tmp) {
		if (e->lsn > lsn)
			break;
		assert(e->txn->fiber == NULL);
		e->is_commit = true;
		txn_limbo_remove(limbo, e);
		txn_clear_flag(e->txn, TXN_WAIT_ACK);
		/*
		 * txn_complete_async must've been called already,
		 * since CONFIRM always follows the tx in question.
		 * So, finish this tx processing right away.
		 */
		txn_complete(e->txn);
	}
}

void
txn_limbo_ack(struct txn_limbo *limbo, uint32_t replica_id, int64_t lsn)
{
	if (rlist_empty(&limbo->queue))
		return;
	assert(limbo->instance_id != REPLICA_ID_NIL);
	int64_t prev_lsn = vclock_get(&limbo->vclock, replica_id);
	vclock_follow(&limbo->vclock, replica_id, lsn);
	struct txn_limbo_entry *e, *tmp;
	struct txn_limbo_entry *last_quorum = NULL;
	rlist_foreach_entry(e, &limbo->queue, in_queue) {
		if (e->lsn <= prev_lsn)
			continue;
		if (e->lsn > lsn)
			break;
		if (++e->ack_count >= replication_synchro_quorum) {
			e->is_commit = true;
			last_quorum = e;
		}
		assert(e->ack_count <= VCLOCK_MAX);
	}
	if (last_quorum != NULL) {
		if (txn_limbo_write_confirm(limbo, last_quorum) != 0) {
			// TODO: rollback.
			return;
		}
		/*
		 * Wakeup all the entries in direct order as soon
		 * as confirmation message is written to WAL.
		 */
		rlist_foreach_entry_safe(e, &limbo->queue, in_queue, tmp) {
			rlist_del_entry(e, in_queue);
			fiber_wakeup(e->txn->fiber);
			if (e == last_quorum)
				break;
		}
	}
}

double
txn_limbo_confirm_timeout(struct txn_limbo *limbo)
{
	(void)limbo;
	return replication_synchro_timeout;
}

void
txn_limbo_init(void)
{
	txn_limbo_create(&txn_limbo);
}
