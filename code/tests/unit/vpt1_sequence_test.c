// SPDX-License-Identifier: GPL-2.0-only
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "vpt1_sequence.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define sequence_test kunit
#define EXPECT(t, condition) KUNIT_EXPECT_TRUE_MSG(t, condition, "%s:%d", \
						  __func__, __LINE__)
#else
struct sequence_test {
	unsigned int assertions;
	unsigned int failures;
};

static void sequence_expect(struct sequence_test *t, bool condition,
			    const char *function, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_sequence_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) sequence_expect(t, condition, __func__, __LINE__)
#endif

static void tx_reservations_and_limits(struct sequence_test *t)
{
	struct vpt1_tx_sequence tx;
	u64 counter = 999;

	vpt1_tx_sequence_init(&tx);
	EXPECT(t, !vpt1_tx_reserve(&tx, 20, &counter));
	EXPECT(t, counter == 0 && tx.bytes == 20);
	/* Simulate failed encryption/output by abandoning the reserved counter. */
	EXPECT(t, !vpt1_tx_reserve(&tx, 20, &counter));
	EXPECT(t, counter == 1 && tx.bytes == 40);
	EXPECT(t, vpt1_tx_reserve(&tx, 3, &counter) == -EINVAL);
	EXPECT(t, vpt1_tx_reserve(&tx, 1433, &counter) == -EINVAL);
	EXPECT(t, vpt1_tx_reserve(&tx, U32_MAX, &counter) == -EINVAL);
	EXPECT(t, vpt1_tx_reserve(&tx, 4, NULL) == -EINVAL);
	EXPECT(t, counter == 1 && tx.bytes == 40 && atomic64_read(&tx.next) == 2);
	/* Test-only boundary state, before any concurrent publication. */
	atomic64_set(&tx.next, 1048575);
	EXPECT(t, !vpt1_tx_reserve(&tx, 4, &counter));
	EXPECT(t, counter == 1048575);
	EXPECT(t, vpt1_tx_reserve(&tx, 4, &counter) == -EDQUOT);
	EXPECT(t, atomic64_read(&tx.next) == 1048576 && counter == 1048575);
	vpt1_tx_sequence_init(&tx);
	tx.bytes = (1ULL << 30) - 4;
	EXPECT(t, !vpt1_tx_reserve(&tx, 4, &counter));
	EXPECT(t, tx.bytes == (1ULL << 30));
	EXPECT(t, vpt1_tx_reserve(&tx, 4, &counter) == -EDQUOT);
	EXPECT(t, counter == 0 && atomic64_read(&tx.next) == 1);
	vpt1_tx_disable(&tx);
	EXPECT(t, vpt1_tx_reserve(&tx, 4, &counter) == -EKEYREVOKED);
}

static void replay_edges_and_auth_boundary(struct sequence_test *t)
{
	static const u64 shifts[] = { 1, 1023, 1024, 4096 };
	struct vpt1_rx_sequence *rx, *other;
	unsigned int i;

	rx = kcalloc(2, sizeof(*rx), GFP_KERNEL);
	if (!rx) {
		EXPECT(t, false);
		return;
	}
	other = rx + 1;
	vpt1_rx_sequence_init(rx);
	EXPECT(t, !vpt1_rx_precheck(rx, 0));
	EXPECT(t, !vpt1_rx_precheck(rx, 1048575));
	/* Failed authentication means no accept call: precheck has no effect. */
	EXPECT(t, !rx->have_highest && !rx->records && !rx->bytes);
	EXPECT(t, !vpt1_rx_accept_authenticated(rx, 0, 4));
	EXPECT(t, rx->have_highest && rx->highest == 0 && rx->records == 1);
	EXPECT(t, vpt1_rx_precheck(rx, 0) == -EALREADY);
	EXPECT(t, vpt1_rx_accept_authenticated(rx, 0, 4) == -EALREADY);
	EXPECT(t, !vpt1_rx_accept_authenticated(rx, 2048, 4));
	EXPECT(t, !vpt1_rx_accept_authenticated(rx, 1025, 4));
	EXPECT(t, vpt1_rx_accept_authenticated(rx, 1024, 4) == -ESTALE);
	EXPECT(t, rx->records == 3 && rx->bytes == 12 && rx->highest == 2048);
	for (i = 0; i < ARRAY_SIZE(shifts); i++) {
		vpt1_rx_sequence_init(rx);
		EXPECT(t, !vpt1_rx_accept_authenticated(rx, 0, 4));
		EXPECT(t, !vpt1_rx_accept_authenticated(rx, shifts[i], 4));
		EXPECT(t, vpt1_rx_precheck(rx, 0) ==
		       (shifts[i] < 1024 ? -EALREADY : -ESTALE));
	}
	vpt1_rx_sequence_init(rx);
	vpt1_rx_sequence_init(other);
	EXPECT(t, !vpt1_rx_accept_authenticated(rx, 1048575, 4));
	EXPECT(t, vpt1_rx_precheck(rx, 1048576) == -ERANGE);
	EXPECT(t, vpt1_rx_precheck(rx, U64_MAX) == -ERANGE);
	EXPECT(t, !vpt1_rx_accept_authenticated(other, 1048575, 4));
	/* Successful AEAD with malformed semantics still claims its counter. */
	EXPECT(t, vpt1_rx_accept_authenticated(rx, 1048575, 4) == -EALREADY);
	vpt1_rx_disable(rx);
	EXPECT(t, vpt1_rx_precheck(rx, 1048574) == -EKEYREVOKED);
	EXPECT(t, vpt1_rx_accept_authenticated(rx, 1048574, 4) == -EKEYREVOKED);
	EXPECT(t, !vpt1_rx_precheck(other, 1048574));
	kfree(rx);
}

static void rx_usage_limits_and_rollback(struct sequence_test *t)
{
	struct vpt1_rx_sequence rx;

	vpt1_rx_sequence_init(&rx);
	rx.bytes = (1ULL << 30) - 4;
	EXPECT(t, !vpt1_rx_accept_authenticated(&rx, 0, 4));
	EXPECT(t, vpt1_rx_accept_authenticated(&rx, 1000, 4) == -EDQUOT);
	EXPECT(t, rx.highest == 0 && rx.records == 1 && rx.bytes == (1ULL << 30));
	EXPECT(t, !test_bit(1000, rx.seen));
	vpt1_rx_sequence_init(&rx);
	rx.records = 1048575;
	EXPECT(t, !vpt1_rx_accept_authenticated(&rx, 0, 4));
	EXPECT(t, vpt1_rx_accept_authenticated(&rx, 1, 4) == -EDQUOT);
	EXPECT(t, rx.highest == 0 && rx.records == 1048576 && rx.bytes == 4);
	vpt1_rx_sequence_init(&rx);
	EXPECT(t, vpt1_rx_accept_authenticated(&rx, 0, 0) == -EINVAL);
	EXPECT(t, vpt1_rx_accept_authenticated(&rx, 0, 1433) == -EINVAL);
	EXPECT(t, vpt1_rx_accept_authenticated(&rx, 0, U32_MAX) == -EINVAL);
	EXPECT(t, !rx.have_highest && !rx.records && !rx.bytes);
	EXPECT(t, bitmap_empty(rx.seen, 1024));
}

static void independent_history_model(struct sequence_test *t)
{
	struct vpt1_rx_sequence rx;
	u32 seed = 0x56315431, counter, highest = 0;
	unsigned int i, accepted = 0;
	bool have_highest = false;
	bool *history;
	int expected;

	history = kcalloc(4096, sizeof(*history), GFP_KERNEL);
	EXPECT(t, history);
	if (!history)
		return;
	vpt1_rx_sequence_init(&rx);
	/* Independent full-history set, no implementation bitmap/shift helpers. */
	for (i = 0; i < 8192; i++) {
		seed = seed * 1664525U + 1013904223U;
		counter = (seed >> 16) & 4095;
		if (have_highest && highest > counter && highest - counter >= 1024)
			expected = -ESTALE;
		else if (history[counter])
			expected = -EALREADY;
		else
			expected = 0;
		EXPECT(t, vpt1_rx_precheck(&rx, counter) == expected);
		EXPECT(t, vpt1_rx_accept_authenticated(&rx, counter, 4) == expected);
		if (!expected) {
			history[counter] = true;
			if (!have_highest || counter > highest)
				highest = counter;
			have_highest = true;
			accepted++;
		}
		EXPECT(t, rx.records == accepted && rx.bytes == accepted * 4);
	}
	kfree(history);
}

#define RACE_THREADS 8
#define TX_PER_THREAD 128
struct sequence_race {
	struct vpt1_tx_sequence tx;
	struct vpt1_rx_sequence rx;
	struct completion ready;
	struct completion start;
	spinlock_t result_lock; /* Protects counters bitmap and result totals. */
	DECLARE_BITMAP(counters, RACE_THREADS * TX_PER_THREAD);
	unsigned int successes;
	unsigned int failures;
};

static int sequence_worker(void *arg)
{
	struct sequence_race *race = arg;
	unsigned int i;
	u64 counter;
	int result;

	/* All workers must pass preliminary replay check before any can commit. */
	result = vpt1_rx_precheck(&race->rx, 42);
	if (result) {
		spin_lock_bh(&race->result_lock);
		race->failures++;
		spin_unlock_bh(&race->result_lock);
	}
	complete(&race->ready);
	wait_for_completion(&race->start);
	result = vpt1_rx_accept_authenticated(&race->rx, 42, 4);
	spin_lock_bh(&race->result_lock);
	if (!result)
		race->successes++;
	else if (result != -EALREADY)
		race->failures++;
	spin_unlock_bh(&race->result_lock);
	for (i = 0; i < TX_PER_THREAD; i++) {
		result = vpt1_tx_reserve(&race->tx, 4, &counter);
		spin_lock_bh(&race->result_lock);
		if (result || counter >= RACE_THREADS * TX_PER_THREAD)
			race->failures++;
		else if (test_and_set_bit(counter, race->counters))
			race->failures++;
		spin_unlock_bh(&race->result_lock);
	}
	return 0;
}

static void concurrent_reservations_and_recheck(struct sequence_test *t)
{
	struct task_struct *tasks[RACE_THREADS];
	struct sequence_race *race;
	unsigned int i, started = 0;

	race = kzalloc(sizeof(*race), GFP_KERNEL);
	EXPECT(t, race);
	if (!race)
		return;
	vpt1_tx_sequence_init(&race->tx);
	vpt1_rx_sequence_init(&race->rx);
	init_completion(&race->ready);
	init_completion(&race->start);
	spin_lock_init(&race->result_lock);
	for (i = 0; i < RACE_THREADS; i++) {
		tasks[i] = kthread_run(sequence_worker, race, "vpt1-seq-%u", i);
		EXPECT(t, !IS_ERR(tasks[i]));
		if (IS_ERR(tasks[i]))
			break;
		started++;
	}
	for (i = 0; i < started; i++)
		EXPECT(t, wait_for_completion_timeout(&race->ready, 5 * HZ));
	complete_all(&race->start);
	for (i = 0; i < started; i++)
		kthread_stop(tasks[i]);
	EXPECT(t, started == RACE_THREADS);
	EXPECT(t, race->successes == 1 && !race->failures);
	EXPECT(t, bitmap_full(race->counters, RACE_THREADS * TX_PER_THREAD));
	EXPECT(t, atomic64_read(&race->tx.next) == RACE_THREADS * TX_PER_THREAD);
	EXPECT(t, race->tx.bytes == RACE_THREADS * TX_PER_THREAD * 4);
	EXPECT(t, race->rx.records == 1 && race->rx.bytes == 4);
	kfree(race);
}

#if IS_ENABLED(CONFIG_KUNIT)
static struct kunit_case vpt1_sequence_cases[] = {
	KUNIT_CASE(tx_reservations_and_limits),
	KUNIT_CASE(replay_edges_and_auth_boundary),
	KUNIT_CASE(rx_usage_limits_and_rollback),
	KUNIT_CASE(independent_history_model),
	KUNIT_CASE(concurrent_reservations_and_recheck),
	{}
};

static struct kunit_suite vpt1_sequence_suite = {
	.name = "vpt1-sequence",
	.test_cases = vpt1_sequence_cases,
};

kunit_test_suite(vpt1_sequence_suite);
#else
static int __init vpt1_sequence_test_init(void)
{
	struct sequence_test t = {};

	tx_reservations_and_limits(&t);
	replay_edges_and_auth_boundary(&t);
	rx_usage_limits_and_rollback(&t);
	independent_history_model(&t);
	concurrent_reservations_and_recheck(&t);
	pr_info("VPT1_SEQUENCE_TEST: assertions=%u failures=%u %s\n",
		t.assertions, t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_sequence_test_exit(void)
{
}
module_init(vpt1_sequence_test_init);
module_exit(vpt1_sequence_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 counter/replay accounting and concurrency tests");
