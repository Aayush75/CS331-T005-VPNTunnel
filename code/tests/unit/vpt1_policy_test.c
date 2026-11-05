// SPDX-License-Identifier: GPL-2.0-only
/* HR5: real RCU/admission/resource boundaries, not a packet/conntrack adapter. */
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/in.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <net/tcp.h>
#include "vpt1_gateway.h"
#include "vpt1_generation.h"
#include "vpt1_flow_state.h"
#include "vpt1_zone_pool.h"

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>
#define policy_test kunit
#define EXPECT(t, c) KUNIT_EXPECT_TRUE_MSG(t, c, "%s:%d", __func__, __LINE__)
#else
struct policy_test { unsigned int assertions, failures; };

static void expect(struct policy_test *t, bool condition, unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_policy_test: line %u failed\n", line);
	}
}

#define EXPECT(t, c) expect(t, c, __LINE__)
#endif

#define IP4(a, b, c, d) cpu_to_be32(((u32)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))
static const u8 owner[VPT1_ID_LEN] = { 1, 2, 3, 4 };

struct fixture {
	struct vpt1_gw *gw;
	struct vpt1_snapshot *snapshot;
	struct vpt1_peer *peers[2];
	struct vpt1_generation *generations[2];
	struct vpt1_incarnation incarnations[2];
	u64 now;
};

static struct vpt1_acl_flow flow(unsigned int slot)
{
	return (struct vpt1_acl_flow){
		.source = IP4(10, 77, 1, 10), .destination = IP4(10, 77, slot + 1, 10),
		.source_port = cpu_to_be16(40000), .destination_port = cpu_to_be16(8080),
		.peer_slot = slot, .direction = VPT1_ACL_LOCAL_TO_PEER, .protocol = IPPROTO_TCP,
	};
}

static struct vpt1_generation *generation(u8 seed, u64 now)
{
	u8 tx[32], rx[32];
	unsigned int i;

	for (i = 0; i < sizeof(tx); i++) {
		tx[i] = seed + i;
		rx[i] = seed + 64 + i;
	}
	return vpt1_generation_create(tx, rx, now);
}

static int commit(struct fixture *f, u64 revision)
{
	bool disruptive;
	int err;

	vpt1_gw_lock(f->gw);
	err = vpt1_gw_validate(f->gw, revision, f->snapshot);
	if (!err)
		err = vpt1_gw_commit(f->gw, revision, f->now + 1, &disruptive);
	vpt1_gw_unlock(f->gw);
	return err;
}

static int setup(struct fixture *f)
{
	struct vpt1_snapshot *s;
	u64 epoch;
	unsigned int i, slot;
	int err;

	memset(f, 0, sizeof(*f));
	f->now = ktime_get_boottime_ns();
	f->gw = vpt1_gw_create(10, f->now);
	f->snapshot = kzalloc(sizeof(*f->snapshot), GFP_KERNEL);
	if (!f->gw || !f->snapshot)
		return -ENOMEM;
	s = f->snapshot;
	s->domain.lan = (struct vpt1_acl_prefix){ IP4(10, 77, 1, 0), 24 };
	s->domain.underlay = (struct vpt1_acl_prefix){ IP4(172, 31, 250, 0), 24 };
	s->domain.lan_address = IP4(10, 77, 1, 1);
	s->lan_ifindex = 2;
	s->underlay_ifindex = 3;
	s->underlay_address = IP4(172, 31, 250, 11);
	for (i = 0; i < 32; i++)
		s->local_key[i] = 16 + i;
	for (slot = 1; slot <= 2; slot++) {
		struct vpt1_cfg_peer *p = &s->peers[slot];

		p->present = true;
		p->slot = slot;
		p->pmtu = 1500;
		p->endpoint = IP4(172, 31, 250, slot + 11);
		p->port = cpu_to_be16(51888);
		p->owned.count = 1;
		p->owned.prefixes[0] = (struct vpt1_acl_prefix){ IP4(10, 77, slot + 1, 0), 24 };
		for (i = 0; i < 32; i++)
			p->key[i] = 32 * (slot + 1) + i;
		s->rules[slot - 1] = (struct vpt1_acl_rule){
			.id = slot, .peer_slot = slot, .direction = VPT1_ACL_LOCAL_TO_PEER,
			.protocol = IPPROTO_TCP, .port_first = 8080, .port_last = 8080,
			.source = { IP4(10, 77, 1, 10), 32 },
			.destination = { IP4(10, 77, slot + 1, 10), 32 },
		};
	}
	s->rule_count = 2;
	vpt1_gw_lock(f->gw);
	/* Extend only the test lease. Generation installation uses real boot time. */
	err = vpt1_gw_claim(f->gw, owner, 100, f->now + 120ULL * NSEC_PER_SEC, &epoch);
	vpt1_gw_unlock(f->gw);
	if (err)
		return err;
	err = commit(f, 1);
	if (err)
		return err;
	for (i = 0; i < 2; i++) {
		f->peers[i] = vpt1_gw_peer_get(f->gw, i + 1);
		if (!f->peers[i])
			return -ENOENT;
		vpt1_peer_incarnation(f->peers[i], &f->incarnations[i]);
		f->generations[i] = generation(i * 32, f->now);
		if (IS_ERR(f->generations[i])) {
			err = PTR_ERR(f->generations[i]);
			f->generations[i] = NULL;
			return err;
		}
		vpt1_generation_get(f->generations[i]); /* Transfer only this ref on success. */
		vpt1_gw_lock(f->gw);
		err = vpt1_gw_dbg_stage(f->gw, i + 1, &f->incarnations[i],
					       f->generations[i], 100 + i, 200 + i);
		vpt1_gw_unlock(f->gw);
		if (err) {
			vpt1_generation_put(f->generations[i]);
			return err;
		}
	}
	return 0;
}

static void cleanup(struct policy_test *t, struct fixture *f)
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		if (f->generations[i])
			vpt1_generation_put(f->generations[i]);
		vpt1_peer_put(f->peers[i]);
	}
	if (f->gw)
		vpt1_gw_destroy(f->gw, 0);
	kfree(f->snapshot);
	rcu_barrier();
	rcu_barrier();
	vpt1_generation_drain();
	EXPECT(t, !vpt1_gw_live() && !vpt1_peer_live() && !vpt1_cfg_live());
	EXPECT(t, !vpt1_generation_live());
}

static void accounting(struct fixture *f, struct vpt1_cfg_accounting *out)
{
	vpt1_gw_lock(f->gw);
	vpt1_gw_debug_accounting(f->gw, out);
	vpt1_gw_unlock(f->gw);
}

/* A new initiation is validated under the SAME admission lock as its handoff.
 * This test-only stack witness models no CT table, zone template or packet.
 */
static int initiate(struct fixture *f, unsigned int i, struct vpt1_generation *gen, bool finish)
{
	struct vpt1_new_initiation init = { .protocol_validated = true, .flow = flow(i + 1) };
	struct vpt1_tcp_sample syn = { .sequence = 123, .flags = TCPHDR_SYN };
	struct vpt1_tcp_state state = {};
	struct vpt1_admit_ctx ctx;
	enum vpt1_reason reason;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peers[i], &f->incarnations[i], VPT1_DIR_TX, gen,
			       f->now + 2, &ctx, &reason);
	if (!err)
		err = vpt1_admit_new(&ctx, &init, &reason);
	if (!err) {
		err = vpt1_tcp_begin(&state, &syn, f->now + 2);
		if (err)
			vpt1_admit_abort(&ctx);
		else
			vpt1_admit_handoff(&ctx);
	}
	rcu_read_unlock();
	if (!err && finish)
		vpt1_admit_done(f->peers[i]);
	return err;
}

struct reader {
	struct fixture *f;
	struct vpt1_generation *replacement; /* Caller holds a reference until join. */
	struct completion ready;
	struct completion done;
	atomic_t release;
	bool expired;
	bool inspect_policy;
	bool changed;
	u64 before_revision;
	u64 after_revision;
	int before;
	int after;
	int fresh;
};

/* Never sleep in RCU. The writer stays on another CPU and releases this gate.
 * A bounded deadline makes an unmet schedule a failed case, never a silent skip.
 */
static void reader_wait(struct reader *r)
{
	unsigned long deadline = jiffies + 5 * HZ;

	while (!atomic_read_acquire(&r->release) && !kthread_should_stop()) {
		if (time_after_eq(jiffies, deadline)) {
			r->expired = true;
			break;
		}
		cpu_relax();
	}
}

static int rcu_reader(void *arg)
{
	struct reader *r = arg;
	struct fixture *f = r->f;
	struct vpt1_witness witness = { .rule_id = 1, .valid = true, .flow = flow(1) };
	struct vpt1_admit_ctx ctx;
	const struct vpt1_cfg *old = NULL;
	enum vpt1_reason why;

	rcu_read_lock();
	r->before = vpt1_admit_begin(f->peers[0], &f->incarnations[0], VPT1_DIR_TX,
				     f->generations[0], f->now + 1, &ctx, &why);
	if (!r->before) {
		old = ctx.cfg;
		r->before_revision = ctx.revision;
		r->before = vpt1_admit_existing(&ctx, &witness, &why);
		vpt1_admit_abort(&ctx);
	}
	complete(&r->ready);
	reader_wait(r);
	if (r->inspect_policy) {
		r->after = vpt1_admit_begin(f->peers[0], &f->incarnations[0], VPT1_DIR_TX,
					    f->generations[0], f->now + 2, &ctx, &why);
		if (!r->after) {
			r->after_revision = ctx.revision;
			r->changed = ctx.cfg != old;
			r->after = vpt1_admit_existing(&ctx, &witness, &why);
			vpt1_admit_abort(&ctx);
		}
	}
	rcu_read_unlock();
	complete(&r->done);
	return 0;
}

/* Caller holds cpus_read_lock and migrate_disable through stop_reader. */
static struct task_struct *start_reader(struct policy_test *t, struct reader *r,
					int (*fn)(void *))
{
	struct task_struct *task;
	unsigned int cpu;

	init_completion(&r->ready);
	init_completion(&r->done);
	atomic_set(&r->release, 0);
	cpus_read_lock();
	migrate_disable();
	cpu = cpumask_any_but(cpu_online_mask, smp_processor_id());
	if (cpu >= nr_cpu_ids) {
		EXPECT(t, false);
		migrate_enable();
		cpus_read_unlock();
		return ERR_PTR(-ENODEV);
	}
	task = kthread_create(fn, r, "vpt1_hr5_reader");
	EXPECT(t, !IS_ERR(task));
	if (IS_ERR(task)) {
		migrate_enable();
		cpus_read_unlock();
		return task;
	}
	get_task_struct(task);
	kthread_bind(task, cpu);
	wake_up_process(task);
	EXPECT(t, wait_for_completion_timeout(&r->ready, HZ) > 0);
	return task;
}

static void stop_reader(struct policy_test *t, struct reader *r, struct task_struct *task)
{
	atomic_set_release(&r->release, 1);
	EXPECT(t, wait_for_completion_timeout(&r->done, 6 * HZ) > 0);
	kthread_stop(task);
	put_task_struct(task);
	EXPECT(t, !r->expired);
	migrate_enable();
	cpus_read_unlock();
}

static void unconfigured_teardown(struct policy_test *t)
{
	struct fixture f = {};

	f.gw = vpt1_gw_create(10, ktime_get_boottime_ns());
	EXPECT(t, f.gw);
	cleanup(t, &f);
}

static void staged_replacement_is_bounded(struct policy_test *t)
{
	struct fixture f;
	struct reader r = {};
	struct task_struct *task;
	struct vpt1_cfg_accounting before, state;
	struct vpt1_acl_rule rule;
	bool disruptive;
	unsigned int i;
	int err = setup(&f);

	EXPECT(t, !err);
	if (err)
		goto out;
	accounting(&f, &before);
	r.f = &f;
	task = start_reader(t, &r, rcu_reader);
	if (IS_ERR(task))
		goto out;
	for (i = 2; i <= 257; i++) {
		vpt1_gw_lock(f.gw);
		err = vpt1_gw_validate(f.gw, i, f.snapshot);
		vpt1_gw_unlock(f.gw);
		accounting(&f, &state);
		EXPECT(t, !err);
		EXPECT(t, !state.retired);
		EXPECT(t, state.current_charge == before.current_charge);
		EXPECT(t, state.staged_charge == before.current_charge);
		EXPECT(t, state.charged == 2 * before.current_charge);
		EXPECT(t, vpt1_cfg_live() == 2);
	}
	stop_reader(t, &r, task);
	EXPECT(t, !r.before);
	EXPECT(t, before.current_charge == before.cfg_bucket + vpt1_acl_charge_bound(2));
	pr_info("VPT1_POLICY_CHARGE: cfg_bucket=%zu current=%zu staged_peak=%zu\n",
		before.cfg_bucket, before.current_charge, state.charged);
	rule = f.snapshot->rules[0];
	for (i = 0; i < VPT1_ACL_MAX_RULES; i++) {
		f.snapshot->rules[i] = rule;
		f.snapshot->rules[i].id = i + 1;
		f.snapshot->rules[i].port_first = 8080 + i;
		f.snapshot->rules[i].port_last = 8080 + i;
	}
	f.snapshot->rule_count = VPT1_ACL_MAX_RULES;
	vpt1_gw_lock(f.gw);
	err = vpt1_gw_validate(f.gw, 258, f.snapshot);
	EXPECT(t, !err);
	vpt1_gw_debug_accounting(f.gw, &state);
	EXPECT(t, state.staged_charge == state.cfg_bucket + vpt1_acl_charge_bound(128));
	pr_info("VPT1_POLICY_MAX_RULES: count=128 charge=%zu budget=%lu\n",
		state.staged_charge, VPT1_CFG_BUDGET);
	f.snapshot->rule_count = VPT1_ACL_MAX_RULES + 1;
	EXPECT(t, vpt1_gw_validate(f.gw, 259, f.snapshot) == -EINVAL);
	err = vpt1_gw_commit(f.gw, 258, f.now + 3, &disruptive);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !err && !disruptive);
	EXPECT(t, !initiate(&f, 0, f.generations[0], true));
out:
	cleanup(t, &f);
}

static void real_retirement_and_publication_cut(struct policy_test *t)
{
	struct fixture f;
	struct reader r = { .inspect_policy = true };
	struct task_struct *task;
	struct vpt1_cfg_accounting before, state;
	bool disruptive;
	int err = setup(&f);

	EXPECT(t, !err);
	if (err)
		goto out;
	accounting(&f, &before);
	r.f = &f;
	task = start_reader(t, &r, rcu_reader);
	if (IS_ERR(task))
		goto out;
	/* Same-sized replacement rule preserves the peer but withdraws saved ID 1. */
	f.snapshot->rules[0].id = 3;
	EXPECT(t, !commit(&f, 2));
	f.snapshot->rules[0].id = 4;
	EXPECT(t, !commit(&f, 3));
	accounting(&f, &state);
	EXPECT(t, state.retired == 2 && state.charged == 3 * before.current_charge);
	EXPECT(t, vpt1_cfg_live() == 3);
	vpt1_gw_lock(f.gw);
	err = vpt1_gw_validate(f.gw, 4, f.snapshot);
	EXPECT(t, !err);
	err = vpt1_gw_commit(f.gw, 4, f.now + 2, &disruptive);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, err == -EBUSY);
	accounting(&f, &state);
	EXPECT(t, state.retired == 2 && state.charged == 4 * before.current_charge);
	stop_reader(t, &r, task);
	EXPECT(t, !r.before && r.before_revision == 1);
	EXPECT(t, r.changed && r.after_revision == 3 && r.after == -EACCES);
	rcu_barrier();
	accounting(&f, &state);
	EXPECT(t, !state.retired && state.charged == 2 * before.current_charge);
	vpt1_gw_lock(f.gw);
	err = vpt1_gw_commit(f.gw, 4, f.now + 3, &disruptive);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !err && !disruptive);
	EXPECT(t, !initiate(&f, 0, f.generations[0], true));
	EXPECT(t, !initiate(&f, 1, f.generations[1], true));
out:
	cleanup(t, &f);
}

static void invalid_construction_rolls_back(struct policy_test *t)
{
	struct fixture f;
	struct vpt1_cfg_accounting before, after;
	struct vpt1_status status;
	struct vpt1_generation *candidate, *active_gen;
	u64 remote_rx;
	unsigned int n;
	int err = setup(&f);

	EXPECT(t, !err);
	if (err)
		goto out;
	vpt1_gw_lock(f.gw);
	err = vpt1_gw_validate(f.gw, 2, f.snapshot);
	EXPECT(t, !err);
	vpt1_gw_debug_accounting(f.gw, &before);
	/* Pass snapshot prechecks, fail ACL construction AFTER budget reservation. */
	f.snapshot->rules[0].source.address = IP4(10, 99, 1, 10);
	err = vpt1_gw_validate(f.gw, 3, f.snapshot);
	vpt1_gw_debug_accounting(f.gw, &after);
	vpt1_gw_status(f.gw, f.now + 2, &status);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, err == -EINVAL);
	EXPECT(t, before.charged == after.charged && !after.retired);
	EXPECT(t, after.staged_charge == before.staged_charge && vpt1_cfg_live() == 2);
	EXPECT(t, status.revision == 1 && status.staged_revision == 2);
	EXPECT(t, !initiate(&f, 0, f.generations[0], true));

	/* Actual ENOMEM after the cfg reservation, not just invalid ACL input. */
	f.snapshot->rules[0].source.address = IP4(10, 77, 1, 10);
	vpt1_gw_lock(f.gw);
	vpt1_acl_dbg_fail_nth(1);
	err = vpt1_gw_validate(f.gw, 3, f.snapshot);
	vpt1_gw_debug_accounting(f.gw, &after);
	vpt1_gw_status(f.gw, f.now + 2, &status);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, err == -ENOMEM && !vpt1_acl_dbg_fail_nth(0));
	EXPECT(t, before.charged == after.charged && !after.retired);
	EXPECT(t, after.staged_charge == before.staged_charge && vpt1_cfg_live() == 2);
	EXPECT(t, status.revision == 1 && status.staged_revision == 2);
	EXPECT(t, !initiate(&f, 0, f.generations[0], true));
	EXPECT(t, !initiate(&f, 1, f.generations[1], true));
	EXPECT(t, !commit(&f, 3)); /* The same valid input succeeds after recovery. */

	/* Object, TX transform, RX transform: every owned allocation boundary.
	 * In particular RX failure must free the already allocated TX transform.
	 */
	for (n = 1; n <= 3; n++) {
		vpt1_generation_dbg_fail_nth(n);
		candidate = generation(0x61, f.now);
		EXPECT(t, IS_ERR(candidate) && PTR_ERR(candidate) == -ENOMEM);
		EXPECT(t, !vpt1_generation_dbg_fail_nth(0));
		if (!IS_ERR(candidate))
			vpt1_generation_put(candidate);
		vpt1_generation_drain();
		EXPECT(t, vpt1_generation_live() == 2);
		active_gen = vpt1_peer_tx_generation_get(f.peers[0], f.now + 2, &remote_rx);
		EXPECT(t, active_gen == f.generations[0]);
		if (active_gen)
			vpt1_generation_put(active_gen);
		EXPECT(t, !initiate(&f, 0, f.generations[0], true));
		EXPECT(t, !initiate(&f, 1, f.generations[1], true));
	}
	candidate = generation(0x61, f.now);
	EXPECT(t, !IS_ERR(candidate));
	if (!IS_ERR(candidate))
		vpt1_generation_put(candidate);
out:
	vpt1_acl_dbg_fail_nth(0);
	vpt1_generation_dbg_fail_nth(0);
	cleanup(t, &f);
}

static struct vpt1_pkt_info packet_info(unsigned int slot)
{
	struct vpt1_acl_flow original = flow(slot);

	return (struct vpt1_pkt_info){ .src = original.source, .dst = original.destination,
		.sport = original.source_port, .dport = original.destination_port,
		.proto = original.protocol };
}

static int processing_reserve(struct fixture *f, unsigned int index)
{
	struct vpt1_pkt_info pi = packet_info(index + 1);
	struct vpt1_admit_ctx ctx;
	enum vpt1_reason why;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peers[index], &f->incarnations[index], VPT1_DIR_TX,
			       f->generations[index], f->now + 2, &ctx, &why);
	if (!err)
		err = vpt1_admit_packet(&ctx, &pi, &why);
	if (!err)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	return err;
}

static int final_recheck(struct fixture *f, unsigned int index, enum vpt1_reason *why)
{
	struct vpt1_pkt_info pi = packet_info(index + 1);
	int err;

	rcu_read_lock();
	err = vpt1_admit_tx_recheck(f->peers[index], &f->incarnations[index],
				    f->generations[index], &pi, 40, f->now + 2, why);
	rcu_read_unlock();
	return err;
}

static void post_crypto_final_cut(struct policy_test *t)
{
	struct fixture f;
	enum vpt1_reason why;
	unsigned int held = 0, i;
	u64 counter;
	int err = setup(&f);

	EXPECT(t, !err);
	if (err)
		goto out;
	EXPECT(t, final_recheck(&f, 0, &why) == -EINVAL && why == VPT1_R_BAD_CONTEXT);
	for (i = 0; i < VPT1_PEER_INFLIGHT_MAX; i++) {
		err = processing_reserve(&f, 0);
		EXPECT(t, !err);
		if (!err)
			held++;
	}
	EXPECT(t, held == VPT1_PEER_INFLIGHT_MAX);
	EXPECT(t, processing_reserve(&f, 0) == -EBUSY);
	/* Existing reservations can finalize even at the exact in-flight ceiling. */
	EXPECT(t, !vpt1_generation_tx_reserve(f.generations[0], 44, f.now, &counter) &&
	       counter == 0);
	EXPECT(t, !final_recheck(&f, 0, &why));
	f.snapshot->rules[0].port_first = 8081;
	f.snapshot->rules[0].port_last = 8081;
	EXPECT(t, !commit(&f, 2));
	EXPECT(t, final_recheck(&f, 0, &why) == -EACCES && why == VPT1_R_ACL_DENY);
	/* The unrelated peer stays eligible; a failed output never refunds a counter. */
	err = processing_reserve(&f, 1);
	EXPECT(t, !err);
	if (!err) {
		EXPECT(t, !final_recheck(&f, 1, &why));
		vpt1_admit_done(f.peers[1]);
	}
	EXPECT(t, !vpt1_generation_tx_reserve(f.generations[0], 44, f.now, &counter) &&
	       counter == 1);
	while (held) {
		vpt1_admit_done(f.peers[0]);
		held--;
	}
out:
	cleanup(t, &f);
}

static int generation_reader(void *arg)
{
	struct reader *r = arg;
	struct fixture *f = r->f;

	r->before = initiate(f, 0, f->generations[0], false);
	complete(&r->ready);
	reader_wait(r);
	r->after = initiate(f, 0, f->generations[0], true);
	r->fresh = initiate(f, 0, r->replacement, true);
	if (!r->before)
		vpt1_admit_done(f->peers[0]);
	complete(&r->done);
	return 0;
}

static int generation_probe(struct fixture *f, enum vpt1_dir direction,
			    struct vpt1_generation *gen)
{
	struct vpt1_admit_ctx ctx;
	enum vpt1_reason why;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peers[0], &f->incarnations[0], direction, gen,
			       f->now + 2, &ctx, &why);
	if (!err)
		vpt1_admit_abort(&ctx);
	rcu_read_unlock();
	return err;
}

static void generation_replacement_cut(struct policy_test *t)
{
	struct fixture f;
	struct reader r = {};
	struct task_struct *task;
	int err = setup(&f);

	EXPECT(t, !err);
	if (err)
		goto out;
	r.f = &f;
	r.replacement = generation(128, f.now);
	EXPECT(t, !IS_ERR(r.replacement));
	if (IS_ERR(r.replacement))
		goto out;
	task = start_reader(t, &r, generation_reader);
	if (!IS_ERR(task)) {
		vpt1_generation_get(r.replacement);
		vpt1_gw_lock(f.gw);
		err = vpt1_gw_dbg_stage(f.gw, 1, &f.incarnations[0],
					       r.replacement, 300, 400);
		vpt1_gw_unlock(f.gw);
		if (err)
			vpt1_generation_put(r.replacement);
		EXPECT(t, !err);
		/* Main's AY4 API stages next for RX without changing TX yet. */
		EXPECT(t, !generation_probe(&f, VPT1_DIR_TX, f.generations[0]));
		EXPECT(t, generation_probe(&f, VPT1_DIR_TX, r.replacement) == -ENOKEY);
		EXPECT(t, !generation_probe(&f, VPT1_DIR_RX, r.replacement));
		/* Model the callback after an accepted authenticated record on next. */
		vpt1_peer_commit(f.peers[0], ktime_get_boottime_ns());
		stop_reader(t, &r, task);
		EXPECT(t, !r.before && r.after == -ENOKEY && !r.fresh);
		EXPECT(t, !generation_probe(&f, VPT1_DIR_RX, f.generations[0]));
		EXPECT(t, !initiate(&f, 1, f.generations[1], true));
	}
	vpt1_generation_put(r.replacement);
out:
	cleanup(t, &f);
}

struct revoke {
	struct fixture *f;
	struct vpt1_zone_pool *pool;
	struct vpt1_zone_token zone;
	struct completion closed;
	struct completion done;
	int disable_err;
	int retire_err;
	int reserve_err;
	int revoke_err;
	int drain_err;
};

static int revoke_worker(void *arg)
{
	struct revoke *r = arg;
	struct vpt1_zone_token token;

	vpt1_gw_lock(r->f->gw);
	r->disable_err = vpt1_gw_disable_peer(r->f->gw, 1, &r->f->incarnations[0]);
	r->retire_err = vpt1_zone_retire(r->pool, &r->zone);
	r->reserve_err = vpt1_zone_reserve(r->pool, &token);
	complete(&r->closed);
	r->revoke_err = vpt1_gw_revoke_peer(r->f->gw, 1, &r->f->incarnations[0]);
	r->drain_err = vpt1_zone_drained(r->pool, &r->zone);
	vpt1_gw_unlock(r->f->gw);
	complete(&r->done);
	return 0;
}

static void revoke_drain_and_zone_pressure(struct policy_test *t)
{
	struct fixture f;
	struct revoke r = {};
	struct task_struct *task;
	struct vpt1_zone_token b = {}, old, reused, untouched = { .serial = 77, .zone = 77 };
	struct vpt1_peer *fresh;
	struct vpt1_incarnation incarnation;
	int err = setup(&f);

	EXPECT(t, !err);
	if (err)
		goto out;
	r.f = &f;
	r.pool = vpt1_zone_create(100, 101);
	EXPECT(t, !IS_ERR(r.pool));
	if (IS_ERR(r.pool))
		goto out;
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_zone_reserve(r.pool, &r.zone));
	EXPECT(t, !vpt1_zone_confirm_clean(r.pool, &r.zone));
	EXPECT(t, !vpt1_zone_reserve(r.pool, &b));
	EXPECT(t, !vpt1_zone_confirm_clean(r.pool, &b));
	vpt1_gw_unlock(f.gw);
	err = initiate(&f, 0, f.generations[0], false);
	EXPECT(t, !err);
	if (err)
		goto pool_out;
	init_completion(&r.closed);
	init_completion(&r.done);
	task = kthread_create(revoke_worker, &r, "vpt1_hr5_revoke");
	EXPECT(t, !IS_ERR(task));
	if (IS_ERR(task)) {
		vpt1_admit_done(f.peers[0]);
		goto pool_out;
	}
	get_task_struct(task);
	wake_up_process(task);
	EXPECT(t, wait_for_completion_timeout(&r.closed, HZ) > 0);
	/* Writer has CLOSED eligibility, but cannot finish until the old handoff drains. */
	EXPECT(t, !wait_for_completion_timeout(&r.done, msecs_to_jiffies(100)));
	EXPECT(t, initiate(&f, 0, f.generations[0], true) == -EACCES);
	EXPECT(t, !initiate(&f, 1, f.generations[1], true));
	vpt1_admit_done(f.peers[0]);
	EXPECT(t, wait_for_completion_timeout(&r.done, 5 * HZ) > 0);
	kthread_stop(task);
	put_task_struct(task);
	EXPECT(t, !r.disable_err && !r.retire_err && !r.revoke_err && !r.drain_err);
	EXPECT(t, r.reserve_err == -ENOSPC);
	vpt1_gw_lock(f.gw);
	EXPECT(t, vpt1_zone_phase(r.pool, &r.zone) == VPT1_ZONE_PURGE_PENDING);
	EXPECT(t, vpt1_zone_reserve(r.pool, &untouched) == -ENOSPC);
	EXPECT(t, untouched.serial == 77 && untouched.zone == 77);
	EXPECT(t, vpt1_zone_phase(r.pool, &b) == VPT1_ZONE_ACTIVE);
	/* This models a confirmed purge completion, NOT a real conntrack purge. */
	old = r.zone;
	EXPECT(t, !vpt1_zone_purged(r.pool, &r.zone));
	EXPECT(t, !vpt1_zone_reserve(r.pool, &reused));
	EXPECT(t, reused.zone == old.zone && reused.serial != old.serial);
	EXPECT(t, vpt1_zone_purged(r.pool, &old) == -ESTALE);
	r.zone = reused;
	fresh = vpt1_gw_peer_get(f.gw, 1);
	EXPECT(t, fresh);
	if (fresh) {
		vpt1_peer_incarnation(fresh, &incarnation);
		EXPECT(t, memcmp(incarnation.id, f.incarnations[0].id, VPT1_ID_LEN));
		EXPECT(t, vpt1_gw_dbg_stage(f.gw, 1, &f.incarnations[0],
						   f.generations[0], 500, 600) == -ESTALE);
		vpt1_peer_put(fresh);
	}
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !initiate(&f, 1, f.generations[1], true));
pool_out:
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_zone_retire(r.pool, &r.zone));
	EXPECT(t, !vpt1_zone_drained(r.pool, &r.zone));
	EXPECT(t, !vpt1_zone_purged(r.pool, &r.zone));
	EXPECT(t, !vpt1_zone_retire(r.pool, &b));
	EXPECT(t, !vpt1_zone_drained(r.pool, &b));
	EXPECT(t, !vpt1_zone_purged(r.pool, &b));
	EXPECT(t, !vpt1_zone_destroy(r.pool));
	vpt1_gw_unlock(f.gw);
out:
	cleanup(t, &f);
}

struct stress {
	struct fixture *f;
	struct completion started;
	wait_queue_head_t progress;
	atomic_t seen;
	unsigned int allowed, denied, new_flows, rate_limited, invalid;
};

static int stress_reader(void *arg)
{
	struct stress *s = arg;
	struct fixture *f = s->f;
	struct vpt1_witness witness = { .rule_id = 1, .valid = true, .flow = flow(1) };
	struct vpt1_new_initiation init = { .protocol_validated = true, .flow = flow(1) };
	struct vpt1_tcp_sample syn = { .sequence = 456, .flags = TCPHDR_SYN };
	unsigned int iteration = 0;

	complete(&s->started);
	while (!kthread_should_stop()) {
		struct vpt1_tcp_state state = {};
		struct vpt1_admit_ctx ctx;
		struct vpt1_generation *gen;
		u64 remote_rx;
		enum vpt1_reason reason;
		u64 revision = 0;
		bool create = iteration++ & 1;
		int err;

		gen = vpt1_peer_tx_generation_get(f->peers[0], f->now + 2, &remote_rx);
		if (!gen) {
			s->invalid++;
			break;
		}
		rcu_read_lock();
		err = vpt1_admit_begin(f->peers[0], &f->incarnations[0], VPT1_DIR_TX,
				       gen, f->now + 2, &ctx, &reason);
		if (!err) {
			revision = ctx.revision;
			err = create ? vpt1_admit_new(&ctx, &init, &reason) :
				       vpt1_admit_existing(&ctx, &witness, &reason);
			if (!(revision & 1)) {
				if (err != -EACCES)
					s->invalid++;
				else
					s->denied++;
			} else if (!err) {
				s->allowed++;
				if (create) {
					if (vpt1_tcp_begin(&state, &syn, f->now + 2))
						s->invalid++;
					s->new_flows++;
				}
			} else if (create && err == -EBUSY && reason == VPT1_R_RATE_LIMIT) {
				s->rate_limited++;
			} else {
				s->invalid++;
			}
			if (!err)
				vpt1_admit_handoff(&ctx);
			else
				vpt1_admit_abort(&ctx);
		} else if (err != -ENOKEY) {
			s->invalid++;
		}
		rcu_read_unlock();
		if (!err)
			vpt1_admit_done(f->peers[0]);
		vpt1_generation_put(gen);
		if (revision) {
			atomic_set_release(&s->seen, revision);
			wake_up(&s->progress);
		}
		cond_resched();
	}
	return 0;
}

static void active_creation_publication_and_generation_race(struct policy_test *t)
{
	struct fixture f;
	struct stress s = {};
	struct task_struct *task;
	unsigned int revision;
	int err = setup(&f);

	EXPECT(t, !err);
	if (err)
		goto out;
	s.f = &f;
	init_completion(&s.started);
	init_waitqueue_head(&s.progress);
	atomic_set(&s.seen, 0);
	task = kthread_create(stress_reader, &s, "vpt1_hr5_active");
	EXPECT(t, !IS_ERR(task));
	if (IS_ERR(task))
		goto out;
	get_task_struct(task);
	wake_up_process(task);
	EXPECT(t, wait_for_completion_timeout(&s.started, HZ) > 0);
	EXPECT(t, wait_event_timeout(s.progress, atomic_read_acquire(&s.seen) == 1, HZ) > 0);
	for (revision = 2; revision <= 17; revision++) {
		struct vpt1_generation *gen = generation(128 + revision, f.now);

		EXPECT(t, !IS_ERR(gen));
		if (IS_ERR(gen))
			break;
		f.snapshot->rules[0].port_first = revision & 1 ? 8080 : 8081;
		f.snapshot->rules[0].port_last = f.snapshot->rules[0].port_first;
		EXPECT(t, !commit(&f, revision));
		vpt1_generation_get(gen); /* Peer reference on success; caller keeps one. */
		vpt1_gw_lock(f.gw);
		err = vpt1_gw_dbg_stage(f.gw, 1, &f.incarnations[0], gen,
					       1000 + revision, 2000 + revision);
		vpt1_gw_unlock(f.gw);
		EXPECT(t, !err);
		if (err)
			vpt1_generation_put(gen);
		else
			vpt1_peer_commit(f.peers[0], ktime_get_boottime_ns());
		vpt1_generation_put(gen);
		EXPECT(t, wait_event_timeout(s.progress,
					     atomic_read_acquire(&s.seen) >= revision, HZ) > 0);
		rcu_barrier();
	}
	kthread_stop(task);
	put_task_struct(task);
	EXPECT(t, !s.invalid && s.allowed && s.denied && s.new_flows);
	EXPECT(t, atomic_read(&s.seen) == 17);
	EXPECT(t, !initiate(&f, 1, f.generations[1], true));
	pr_info("VPT1_POLICY_STRESS: allowed=%u denied=%u new=%u rate=%u invalid=%u revisions=%d\n",
		s.allowed, s.denied, s.new_flows, s.rate_limited, s.invalid, atomic_read(&s.seen));
out:
	cleanup(t, &f);
}

static int initialize(void)
{
	int err;

	if (num_online_cpus() < 2)
		return -ENODEV;
	err = vpt1_generation_init();
	if (err)
		return err;
	err = vpt1_gw_init();
	if (err)
		vpt1_generation_exit();
	return err;
}

static void finish(void)
{
	vpt1_gw_exit();
	vpt1_generation_drain();
	vpt1_generation_exit();
}

#if IS_ENABLED(CONFIG_KUNIT)
static int suite_initialize(struct kunit_suite *suite)
{
	return initialize();
}

static void suite_finish(struct kunit_suite *suite)
{
	finish();
}

static struct kunit_case cases[] = {
	KUNIT_CASE(unconfigured_teardown),
	KUNIT_CASE(staged_replacement_is_bounded), KUNIT_CASE(real_retirement_and_publication_cut),
	KUNIT_CASE(invalid_construction_rolls_back), KUNIT_CASE(generation_replacement_cut),
	KUNIT_CASE(post_crypto_final_cut),
	KUNIT_CASE(revoke_drain_and_zone_pressure),
	KUNIT_CASE(active_creation_publication_and_generation_race), {}
};

static struct kunit_suite suite = {
	.name = "vpt1-policy", .suite_init = suite_initialize, .suite_exit = suite_finish,
	.test_cases = cases,
};

kunit_test_suite(suite);
#else
static int __init policy_test_init(void)
{
	struct policy_test t = {};
	int err = initialize();

	if (err)
		return err;
	unconfigured_teardown(&t);
	staged_replacement_is_bounded(&t);
	real_retirement_and_publication_cut(&t);
	invalid_construction_rolls_back(&t);
	generation_replacement_cut(&t);
	post_crypto_final_cut(&t);
	revoke_drain_and_zone_pressure(&t);
	active_creation_publication_and_generation_race(&t);
	finish();
	pr_info("VPT1_POLICY_TEST: assertions=%u failures=%u %s\n", t.assertions,
		t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit policy_test_exit(void)
{
}

module_init(policy_test_init);
module_exit(policy_test_exit);
#endif
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 HR5 policy resource and deterministic concurrency qualification");
