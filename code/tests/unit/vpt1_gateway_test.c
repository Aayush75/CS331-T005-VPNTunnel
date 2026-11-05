// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test-only: AY2 gateway core (lease, config publication, peer table, admission,
 * teardown) with an injected clock. No genl, device, socket or packet. Built only
 * with VPT1_DEBUG=1 because it drives the debug knobs.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/icmp.h>
#include <linux/in.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <net/ip.h>

#include "vpt1_error.h"
#include "vpt1_fragment.h"
#include "vpt1_gateway.h"
#include "vpt1_generation.h"
#include "vpt1_wire.h"

struct gw_test {
	unsigned int assertions;
	unsigned int failures;
};

static void gw_expect(struct gw_test *t, bool condition, const char *function,
		      unsigned int line)
{
	t->assertions++;
	if (!condition) {
		t->failures++;
		pr_err("vpt1_gateway_test: %s:%u failed\n", function, line);
	}
}

#define EXPECT(t, condition) gw_expect(t, condition, __func__, __LINE__)

#define SEC NSEC_PER_SEC
#define IPV4(a, b, c, d) cpu_to_be32(((u32)(a) << 24) | ((b) << 16) | \
				   ((c) << 8) | (d))

static const u8 ID_A[VPT1_ID_LEN] = { 0xa1, 1, 2, 3 };
static const u8 ID_B[VPT1_ID_LEN] = { 0xb2, 4, 5, 6 };

static void fill_key(u8 key[32], u8 seed)
{
	unsigned int i;

	for (i = 0; i < 32; i++)
		key[i] = seed + i;
}

static void snap_base(struct vpt1_snapshot *s)
{
	struct vpt1_cfg_peer *p;

	memset(s, 0, sizeof(*s));
	s->domain.lan = (struct vpt1_acl_prefix){ IPV4(10, 77, 1, 0), 24 };
	s->domain.underlay = (struct vpt1_acl_prefix){ IPV4(172, 31, 250, 0), 24 };
	s->domain.lan_address = IPV4(10, 77, 1, 1);
	s->lan_ifindex = 2;
	s->underlay_ifindex = 3;
	s->underlay_address = IPV4(172, 31, 250, 1);
	fill_key(s->local_key, 0x10);

	p = &s->peers[1];
	p->present = true;
	p->slot = 1;
	p->pmtu = 1500;
	p->endpoint = IPV4(172, 31, 250, 12);
	p->port = cpu_to_be16(51888);
	fill_key(p->key, 0x20);
	p->owned.count = 1;
	p->owned.prefixes[0] = (struct vpt1_acl_prefix){ IPV4(10, 77, 2, 0), 24 };

	p = &s->peers[2];
	p->present = true;
	p->slot = 2;
	p->pmtu = 1500;
	p->endpoint = IPV4(172, 31, 250, 13);
	p->port = cpu_to_be16(51888);
	fill_key(p->key, 0x30);
	p->owned.count = 1;
	p->owned.prefixes[0] = (struct vpt1_acl_prefix){ IPV4(10, 77, 3, 0), 24 };

	s->rule_count = 2;
	s->rules[0] = (struct vpt1_acl_rule){
		.id = 1, .peer_slot = 1, .direction = VPT1_ACL_LOCAL_TO_PEER,
		.protocol = IPPROTO_TCP, .port_first = 8080, .port_last = 8080,
		.source = { IPV4(10, 77, 1, 10), 32 },
		.destination = { IPV4(10, 77, 2, 10), 32 },
	};
	s->rules[1] = (struct vpt1_acl_rule){
		.id = 2, .peer_slot = 2, .direction = VPT1_ACL_LOCAL_TO_PEER,
		.protocol = IPPROTO_TCP, .port_first = 8080, .port_last = 8080,
		.source = { IPV4(10, 77, 1, 10), 32 },
		.destination = { IPV4(10, 77, 3, 10), 32 },
	};
}

static struct vpt1_acl_flow flow_for(u8 slot, u8 host)
{
	return (struct vpt1_acl_flow){
		.source = IPV4(10, 77, 1, 10),
		.destination = IPV4(10, 77, slot + 1, host),
		.source_port = cpu_to_be16(45000),
		.destination_port = cpu_to_be16(8080),
		.peer_slot = slot,
		.direction = VPT1_ACL_LOCAL_TO_PEER,
		.protocol = IPPROTO_TCP,
	};
}

static struct vpt1_gw *new_gw(struct gw_test *t, u64 now, u64 *epoch)
{
	struct vpt1_gw *gw = vpt1_gw_create(2, now);
	u64 e = 0;
	int err;

	EXPECT(t, gw);
	if (!gw)
		return NULL;
	vpt1_gw_lock(gw);
	err = vpt1_gw_claim(gw, ID_A, 100, now, &e);
	vpt1_gw_unlock(gw);
	EXPECT(t, !err && e);
	if (epoch)
		*epoch = e;
	return gw;
}

static int stage_commit(struct vpt1_gw *gw, u64 revision, const struct vpt1_snapshot *s,
			u64 now, bool *disruptive)
{
	bool dis = false;
	int err;

	/* The retired-config ceiling is real: let earlier reclamation finish. */
	rcu_barrier();
	vpt1_gw_lock(gw);
	err = vpt1_gw_validate(gw, revision, s);
	if (!err)
		err = vpt1_gw_commit(gw, revision, now, &dis);
	vpt1_gw_unlock(gw);
	if (disruptive)
		*disruptive = dis;
	return err;
}

static void get_status(struct vpt1_gw *gw, u64 now, struct vpt1_status *st)
{
	vpt1_gw_lock(gw);
	vpt1_gw_status(gw, now, st);
	vpt1_gw_unlock(gw);
}

static const struct vpt1_status_peer *status_peer(const struct vpt1_status *st,
						  unsigned int slot)
{
	unsigned int i;

	for (i = 0; i < st->peer_count; i++)
		if (st->peers[i].slot == slot)
			return &st->peers[i];
	return NULL;
}

static bool same_inc(const struct vpt1_status *a, const struct vpt1_status *b,
		     unsigned int slot)
{
	const struct vpt1_status_peer *x = status_peer(a, slot);
	const struct vpt1_status_peer *y = status_peer(b, slot);

	return x && y && !memcmp(x->incarnation.id, y->incarnation.id, VPT1_ID_LEN);
}

static void end_gw(struct gw_test *t, struct vpt1_gw *gw)
{
	vpt1_gw_destroy(gw, 0);
	/* Every cfg, peer and gateway is freed by RCU; a peer callback queues the
	 * gateway free, so wait twice.
	 */
	rcu_barrier();
	rcu_barrier();
}

/* Lease: claim, renew, expiry by clock comparison, owner release. --------------- */

static void lease_rules(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_gw *gw = new_gw(t, T, NULL);
	u64 epoch, deadline, e2;
	u64 counters[VPT1_E_COUNT];

	if (!gw)
		return;
	vpt1_gw_lock(gw);
	/* A live lease: another controller is refused; same ID on another portid is
	 * not continuity (a reused portid or pid never is).
	 */
	EXPECT(t, vpt1_gw_claim(gw, ID_B, 101, T + 1, &epoch) == -EBUSY);
	EXPECT(t, vpt1_gw_claim(gw, ID_A, 101, T + 1, &epoch) == -EBUSY);
	/* Same ID and portid while live is a renewal: same epoch. */
	EXPECT(t, !vpt1_gw_claim(gw, ID_A, 100, T + 1, &epoch) && epoch == 1);
	EXPECT(t, !vpt1_gw_check_owner(gw, ID_A, 100, T + 2));
	EXPECT(t, vpt1_gw_check_owner(gw, ID_B, 100, T + 2) == -EPERM);
	EXPECT(t, vpt1_gw_check_owner(gw, ID_A, 101, T + 2) == -EPERM);
	deadline = T + 1 + VPT1_LEASE_NS;
	EXPECT(t, !vpt1_gw_check_owner(gw, ID_A, 100, deadline - 1));
	EXPECT(t, vpt1_gw_check_owner(gw, ID_A, 100, deadline) == -ESTALE);

	/* Renewal: just before the deadline works; at it or after it never does. */
	EXPECT(t, vpt1_gw_renew(gw, ID_A, 100, 7, deadline - 1) == -ESTALE); /* revision */
	EXPECT(t, !vpt1_gw_renew(gw, ID_A, 100, 0, deadline - 1));
	deadline = deadline - 1 + VPT1_LEASE_NS;
	EXPECT(t, vpt1_gw_renew(gw, ID_A, 100, 0, deadline) == -ESTALE);
	EXPECT(t, vpt1_gw_renew(gw, ID_A, 100, 0, deadline + 1) == -ESTALE);
	EXPECT(t, vpt1_gw_renew(gw, ID_B, 100, 0, deadline - 1) == -EPERM);
	/* The cleanup work has not run, yet the clock alone already denies. */
	EXPECT(t, vpt1_gw_check_owner(gw, ID_A, 100, deadline) == -ESTALE);
	/* CLAIM after expiry is a new owner, even with the same ID and portid. */
	EXPECT(t, !vpt1_gw_claim(gw, ID_A, 100, deadline + 1, &e2) && e2 == 2);
	EXPECT(t, !vpt1_gw_check_owner(gw, ID_A, 100, deadline + 2));
	vpt1_gw_unlock(gw);

	vpt1_gw_counters(gw, counters);
	EXPECT(t, counters[VPT1_E_CLAIMS] == 2);
	EXPECT(t, counters[VPT1_E_LEASE_EXPIRED] == 1);

	/* Owner socket release closes the lease at once; other portids do not. */
	EXPECT(t, !vpt1_gw_owner_released(gw, 999));
	EXPECT(t, vpt1_gw_owner_released(gw, 100));
	EXPECT(t, vpt1_gw_check_owner(gw, ID_A, 100, deadline + 3) == -EPERM);
	EXPECT(t, !vpt1_gw_owner_released(gw, 100)); /* Already lost. */
	vpt1_gw_flush();
	end_gw(t, gw);
}

/* Configuration: validation, delta classification, ceiling. ----------------------- */

static void config_delta(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_status st0, st;
	struct vpt1_snapshot *s;
	struct vpt1_gw *gw;
	bool dis;
	long cfg0 = vpt1_cfg_live(), peer0 = vpt1_peer_live();

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	gw = new_gw(t, T, NULL);
	EXPECT(t, s && gw);
	if (!s || !gw)
		goto out;
	snap_base(s);

	/* Revision 1 is the first commit: not disruptive, two peers appear. */
	EXPECT(t, !stage_commit(gw, 1, s, T, &dis) && !dis);
	get_status(gw, T, &st0);
	EXPECT(t, st0.revision == 1 && st0.peer_count == 2 && !st0.has_staged);
	EXPECT(t, !same_inc(&st0, &st0, 3));
	EXPECT(t, !memcmp(status_peer(&st0, 1)->incarnation.id,
			  status_peer(&st0, 1)->incarnation.id, VPT1_ID_LEN));
	EXPECT(t, memcmp(status_peer(&st0, 1)->incarnation.id,
			 status_peer(&st0, 2)->incarnation.id, VPT1_ID_LEN));

	vpt1_gw_lock(gw);
	EXPECT(t, vpt1_gw_commit(gw, 1, T, &dis) == -ENOENT); /* Nothing staged. */
	EXPECT(t, vpt1_gw_validate(gw, 1, s) == -ESTALE); /* Not newer than current. */
	EXPECT(t, vpt1_gw_validate(gw, 0, s) == -ESTALE);
	vpt1_gw_unlock(gw);

	/* ACL-only change: the session is kept (policy-only). */
	s->rule_count = 1;
	EXPECT(t, !stage_commit(gw, 2, s, T, &dis) && !dis);
	get_status(gw, T, &st);
	EXPECT(t, same_inc(&st0, &st, 1) && same_inc(&st0, &st, 2));

	/* A key kept but the endpoint changed: disruptive for that slot only. */
	s->peers[1].endpoint = IPV4(172, 31, 250, 22);
	EXPECT(t, !stage_commit(gw, 3, s, T, &dis) && dis);
	get_status(gw, T, &st);
	EXPECT(t, !same_inc(&st0, &st, 1) && same_inc(&st0, &st, 2));
	st0 = st;

	/* Owned prefixes reassigned with the same key: disruptive. */
	s->peers[2].owned.prefixes[0] = (struct vpt1_acl_prefix){ IPV4(10, 77, 4, 0), 24 };
	EXPECT(t, !stage_commit(gw, 4, s, T, &dis) && dis);
	get_status(gw, T, &st);
	EXPECT(t, same_inc(&st0, &st, 1) && !same_inc(&st0, &st, 2));
	st0 = st;

	/* Pinned key change, and a PMTU-only change (disruptive until Raj decides). */
	fill_key(s->peers[1].key, 0x60);
	EXPECT(t, !stage_commit(gw, 5, s, T, &dis) && dis);
	get_status(gw, T, &st);
	EXPECT(t, !same_inc(&st0, &st, 1) && same_inc(&st0, &st, 2));
	st0 = st;
	s->peers[2].pmtu = 1400;
	EXPECT(t, !stage_commit(gw, 6, s, T, &dis) && dis);
	get_status(gw, T, &st);
	EXPECT(t, same_inc(&st0, &st, 1) && !same_inc(&st0, &st, 2));
	st0 = st;

	/* A local change (identity) replaces every peer. */
	fill_key(s->local_key, 0x70);
	EXPECT(t, !stage_commit(gw, 7, s, T, &dis) && dis);
	get_status(gw, T, &st);
	EXPECT(t, !same_inc(&st0, &st, 1) && !same_inc(&st0, &st, 2));
	st0 = st;

	/* Removing a peer closes it. */
	s->peers[2].present = false;
	memset(&s->peers[2], 0, sizeof(s->peers[2]));
	EXPECT(t, !stage_commit(gw, 8, s, T, &dis) && dis);
	get_status(gw, T, &st);
	EXPECT(t, st.peer_count == 1 && same_inc(&st0, &st, 1));

	/* Invalid snapshots fail with no effect on the published revision. */
	snap_base(s);
	vpt1_gw_lock(gw);
	memcpy(s->peers[2].key, s->peers[1].key, 32);
	EXPECT(t, vpt1_gw_validate(gw, 9, s) == -EEXIST);
	snap_base(s);
	s->peers[2].endpoint = s->peers[1].endpoint;
	EXPECT(t, vpt1_gw_validate(gw, 9, s) == -EEXIST);
	snap_base(s);
	s->peers[1].pmtu = 100;
	EXPECT(t, vpt1_gw_validate(gw, 9, s) == -EINVAL);
	snap_base(s);
	memcpy(s->peers[1].key, s->local_key, 32);
	EXPECT(t, vpt1_gw_validate(gw, 9, s) == -EINVAL);
	snap_base(s);
	s->rule_count = 0;
	EXPECT(t, vpt1_gw_validate(gw, 9, s) == -EINVAL);
	snap_base(s);
	s->peers[1].present = false;
	s->peers[2].present = false;
	EXPECT(t, vpt1_gw_validate(gw, 9, s) == -EINVAL);
	snap_base(s);
	/* The ceiling: two retired configs still awaiting reclamation. */
	vpt1_gw_dbg_add_retired(gw, VPT1_MAX_RETIRED);
	EXPECT(t, !vpt1_gw_validate(gw, 9, s));
	EXPECT(t, vpt1_gw_commit(gw, 9, T, &dis) == -EBUSY);
	vpt1_gw_dbg_add_retired(gw, -VPT1_MAX_RETIRED);
	EXPECT(t, !vpt1_gw_commit(gw, 9, T, &dis));
	vpt1_gw_unlock(gw);
	get_status(gw, T, &st);
	EXPECT(t, st.revision == 9);

	/* A snapshot staged by an old owner never commits after a new claim. */
	snap_base(s);
	vpt1_gw_lock(gw);
	EXPECT(t, !vpt1_gw_validate(gw, 10, s));
	vpt1_gw_unlock(gw);
	vpt1_gw_owner_released(gw, 100);
	vpt1_gw_lock(gw);
	{
		u64 e;

		EXPECT(t, !vpt1_gw_claim(gw, ID_B, 200, T + 1, &e) && e == 2);
		EXPECT(t, vpt1_gw_commit(gw, 10, T + 1, &dis) == -ENOENT);
	}
	vpt1_gw_unlock(gw);
	get_status(gw, T + 1, &st);
	/* The old owner's cfg and peers are gone as well. */
	EXPECT(t, st.revision == 0 && st.peer_count == 0);
out:
	if (gw)
		end_gw(t, gw);
	kfree(s);
	EXPECT(t, vpt1_cfg_live() == cfg0 && vpt1_peer_live() == peer0);
}

/* Admission ------------------------------------------------------------------ */

struct fixture {
	struct vpt1_gw *gw;
	struct vpt1_snapshot *snap;
	struct vpt1_peer *peer;
	struct vpt1_generation *gen;
	struct vpt1_incarnation inc;
	u64 T;
};
static u8 test_peer_role; /* The role fixture_make gives peer 1: 0 responder, 1 initiator. */


static int fixture_make_pmtu(struct gw_test *t, struct fixture *f, u64 claim_at,
			     u64 gen_at, u16 pmtu)
{
	u8 tx[32], rx[32];
	bool dis;
	int err;

	memset(f, 0, sizeof(*f));
	f->T = claim_at;
	f->snap = kzalloc(sizeof(*f->snap), GFP_KERNEL);
	f->gw = new_gw(t, claim_at, NULL);
	if (!f->snap || !f->gw)
		return -ENOMEM;
	snap_base(f->snap);
	f->snap->peers[1].role = test_peer_role;
	f->snap->peers[1].pmtu = pmtu;
	err = stage_commit(f->gw, 1, f->snap, claim_at, &dis);
	EXPECT(t, !err);
	f->peer = vpt1_gw_peer_get(f->gw, 1);
	EXPECT(t, f->peer);
	if (!f->peer)
		return -ENOENT;
	vpt1_peer_incarnation(f->peer, &f->inc);
	fill_key(tx, 0x01);
	fill_key(rx, 0x81);
	f->gen = vpt1_generation_create(tx, rx, gen_at);
	EXPECT(t, !IS_ERR(f->gen));
	if (IS_ERR(f->gen)) {
		f->gen = NULL;
		return -ENOKEY;
	}
	vpt1_generation_get(f->gen); /* One for the peer, one for the test. */
	vpt1_gw_lock(f->gw);
	err = vpt1_gw_dbg_stage(f->gw, 1, &f->inc, f->gen, 0x1111, 0x2222);
	vpt1_gw_unlock(f->gw);
	EXPECT(t, !err);
	return err;
}

static int fixture_make(struct gw_test *t, struct fixture *f, u64 claim_at, u64 gen_at)
{
	return fixture_make_pmtu(t, f, claim_at, gen_at, 1500);
}

static void fixture_free(struct gw_test *t, struct fixture *f)
{
	if (f->gen)
		vpt1_generation_put(f->gen);
	if (f->peer)
		vpt1_peer_put(f->peer);
	if (f->gw)
		end_gw(t, f->gw);
	kfree(f->snap);
}

static void size_admission(struct gw_test *t)
{
	struct vpt1_admit_ctx ctx = {};
	struct fixture f;
	enum vpt1_reason why;
	u64 now = ktime_get_boottime_ns();
	unsigned int pmtu, length;
	int err;

	EXPECT(t, vpt1_admit_size(NULL, 100, &why) == -EINVAL);
	EXPECT(t, vpt1_admit_size(&ctx, 100, &why) == -EINVAL);
	for (pmtu = 1280; pmtu <= 1500; pmtu += 220) {
		if (fixture_make_pmtu(t, &f, now, now, pmtu)) {
			fixture_free(t, &f);
			continue;
		}
		rcu_read_lock();
		EXPECT(t, vpt1_gw_underlay_local(f.gw, 3, IPV4(172, 31, 250, 1)));
		EXPECT(t, !vpt1_gw_underlay_local(f.gw, 2, IPV4(172, 31, 250, 1)));
		EXPECT(t, !vpt1_gw_underlay_local(f.gw, 3, IPV4(172, 31, 250, 2)));
		for (length = pmtu - 73; length <= pmtu - 71; length++) {
			err = vpt1_admit_begin(f.peer, &f.inc, VPT1_DIR_TX, f.gen,
					       now + 1, &ctx, &why);
			EXPECT(t, !err);
			if (err)
				continue;
			err = vpt1_admit_size(&ctx, length, &why);
			EXPECT(t, err == (length > pmtu - 72 ? -EMSGSIZE : 0));
			if (err)
				EXPECT(t, why == VPT1_R_SIZE);
			EXPECT(t, ctx.locked == (length <= pmtu - 72));
			/* Size success is a predicate, not authorization/handoff. */
			if (ctx.locked)
				vpt1_admit_abort(&ctx);
		}
		rcu_read_unlock();
		fixture_free(t, &f);
	}
}

/* A failed device transaction must preserve staging, peers and publication. */
static u16 device_mtu;
static bool fail_mtu;

static int test_set_mtu(struct vpt1_gw *gw, u16 mtu)
{
	if (fail_mtu)
		return -EIO;
	device_mtu = mtu;
	return 0;
}

static void mtu_transaction(struct gw_test *t)
{
	const struct vpt1_gw_ops ops = { .set_mtu = test_set_mtu };
	struct vpt1_snapshot *s;
	struct vpt1_status st;
	struct vpt1_gw *gw;
	u64 now = ktime_get_boottime_ns();
	bool dis;

	static_assert(VPT1_E_RX_ECN_ANOMALY == 35);
	static_assert(VPT1_R_SIZE == 49);
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	EXPECT(t, s);
	if (!s)
		return;
	snap_base(s);
	gw = new_gw(t, now, NULL);
	if (!gw)
		goto free;
	device_mtu = 1428;
	fail_mtu = true;
	vpt1_gw_set_ops(&ops);
	EXPECT(t, stage_commit(gw, 1, s, now, &dis) == -EIO);
	get_status(gw, now, &st);
	EXPECT(t, !st.revision && !st.peer_count && st.has_staged && device_mtu == 1428);
	fail_mtu = false;
	EXPECT(t, !stage_commit(gw, 1, s, now, &dis) && device_mtu == 1428);
	s->peers[2].pmtu = 1280;
	fail_mtu = true;
	EXPECT(t, stage_commit(gw, 2, s, now, &dis) == -EIO);
	get_status(gw, now, &st);
	EXPECT(t, st.revision == 1 && st.peer_count == 2 && st.has_staged &&
	       st.peers[1].pmtu == 1500 && device_mtu == 1428);
	fail_mtu = false;
	EXPECT(t, !stage_commit(gw, 2, s, now, &dis) && dis && device_mtu == 1208);
	s->peers[1].pmtu = 1400;
	EXPECT(t, !stage_commit(gw, 3, s, now, &dis) && device_mtu == 1208);
	s->peers[2].pmtu = 1500;
	EXPECT(t, !stage_commit(gw, 4, s, now, &dis) && device_mtu == 1328);
	s->peers[1].pmtu = 1500;
	EXPECT(t, !stage_commit(gw, 5, s, now, &dis) && device_mtu == 1428);
	vpt1_gw_set_ops(NULL);
	end_gw(t, gw);
free:
	kfree(s);
}

static bool fail_bind;
static unsigned int bind_calls, unbind_calls;
static __be32 bound_local;

static int test_bind(struct vpt1_gw *gw, __be32 local, u32 ifindex)
{
	bind_calls++;
	if (fail_bind)
		return -EADDRINUSE;
	bound_local = local;
	return 0;
}

static void test_unbind(struct vpt1_gw *gw)
{
	unbind_calls++;
	bound_local = 0;
}

static void global_binding_failure(struct gw_test *t)
{
	const struct vpt1_gw_ops ops = { .bind = test_bind, .unbind = test_unbind,
		.set_mtu = test_set_mtu };
	struct vpt1_snapshot *s;
	struct vpt1_status before, after;
	struct vpt1_gw *gw;
	u64 now = ktime_get_boottime_ns(), epoch;
	bool dis;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	EXPECT(t, s);
	if (!s)
		return;
	snap_base(s);
	gw = new_gw(t, now, NULL);
	if (!gw)
		goto free;
	fail_bind = false;
	fail_mtu = false;
	bind_calls = 0;
	unbind_calls = 0;
	vpt1_gw_set_ops(&ops);
	EXPECT(t, !stage_commit(gw, 1, s, now, &dis));
	get_status(gw, now, &before);
	EXPECT(t, bound_local == s->underlay_address && bind_calls == 1);
	s->underlay_address = IPV4(172, 31, 250, 2);
	fail_mtu = true;
	EXPECT(t, stage_commit(gw, 2, s, now, &dis) == -EIO);
	get_status(gw, now, &after);
	EXPECT(t, after.revision == 1 && after.has_staged && after.staged_revision == 2);
	EXPECT(t, after.peers[0].state == VPT1_PEER_INVALID &&
	       after.peers[1].state == VPT1_PEER_INVALID);
	EXPECT(t, !bound_local && bind_calls == 2 && unbind_calls == 2);
	fail_mtu = false;
	fail_bind = true;
	EXPECT(t, stage_commit(gw, 2, s, now, &dis) == -EADDRINUSE);
	EXPECT(t, !bound_local && bind_calls == 3);
	/* Reverting the config must replace closed peers even when its fields
	 * equal the last published cfg; old incarnations can never be revived.
	 */
	fail_bind = false;
	s->underlay_address = IPV4(172, 31, 250, 1);
	EXPECT(t, !stage_commit(gw, 3, s, now, &dis));
	get_status(gw, now, &after);
	EXPECT(t, after.revision == 3 && after.peers[0].state == VPT1_PEER_DOWN);
	EXPECT(t, memcmp(after.peers[0].incarnation.id, before.peers[0].incarnation.id, 16));
	EXPECT(t, bound_local == s->underlay_address && bind_calls == 4);
	vpt1_gw_lock(gw);
	EXPECT(t, !vpt1_gw_claim(gw, ID_B, 101, now + 6 * SEC, &epoch));
	vpt1_gw_unlock(gw);
	s->underlay_address = IPV4(172, 31, 250, 2);
	EXPECT(t, !stage_commit(gw, 1, s, now + 6 * SEC, &dis));
	EXPECT(t, bound_local == s->underlay_address && bind_calls == 5 && unbind_calls == 3);
	end_gw(t, gw);
	EXPECT(t, !bound_local && unbind_calls == 4);
	vpt1_gw_set_ops(NULL);
free:
	kfree(s);
}

/*
 * Whole-runtime memory budget (16 MiB/gateway, 128 MiB/host). The fragment reserve path and the
 * configuration charge both feed the aggregate; a single gateway cannot pass its own ceiling and
 * the gateways together cannot pass the host ceiling.
 */
static void runtime_budget(struct gw_test *t)
{
	const size_t GW_CAP = VPT1_GW_MEM_BUDGET, HOST_CAP = VPT1_HOST_MEM_BUDGET;
	const size_t chunk = 1024UL * 1024UL; /* 1 MiB reservations. */
	struct vpt1_cfg_accounting acct;
	struct vpt1_snapshot *s = NULL;
	struct vpt1_gw *gws[9] = {};
	u64 T = ktime_get_boottime_ns();
	unsigned int i, held;
	size_t host_before;

	/* A clean host accumulator for the arithmetic below. */
	EXPECT(t, vpt1_host_mem_used() == 0);
	host_before = vpt1_host_mem_used();

	gws[0] = new_gw(t, T, NULL);
	if (!gws[0])
		goto out;

	/* A single reservation larger than the per-gateway ceiling is refused outright. */
	EXPECT(t, !vpt1_gw_mem_reserve(gws[0], GW_CAP + 1));
	EXPECT(t, vpt1_gw_mem_used(gws[0]) == 0);

	/* Fill exactly one gateway's ceiling in 1 MiB frag reservations, then the next fails. */
	for (i = 0, held = 0; i < GW_CAP / chunk; i++)
		if (vpt1_gw_frag_reserve(gws[0], chunk))
			held++;
	/* The fragment sub-budget (2 MiB) caps the frag path first, below the 16 MiB aggregate. */
	EXPECT(t, vpt1_gw_frag_used(gws[0]) <= VPT1_FRAG_BUDGET);
	EXPECT(t, held == VPT1_FRAG_BUDGET / chunk);
	EXPECT(t, !vpt1_gw_frag_reserve(gws[0], chunk)); /* frag sub-budget full */
	/* Aggregate still has room, but the frag path is capped; release it all. */
	while (held--)
		vpt1_gw_frag_release(gws[0], chunk);
	EXPECT(t, vpt1_gw_frag_used(gws[0]) == 0 && vpt1_gw_mem_used(gws[0]) == 0);
	EXPECT(t, vpt1_host_mem_used() == host_before);

	/* The aggregate ceiling itself, exercised through the raw reserve API. */
	for (i = 0, held = 0; i < GW_CAP / chunk + 2; i++)
		if (vpt1_gw_mem_reserve(gws[0], chunk))
			held++;
	EXPECT(t, held == GW_CAP / chunk);
	EXPECT(t, vpt1_gw_mem_used(gws[0]) == GW_CAP);
	EXPECT(t, vpt1_host_mem_used() == host_before + GW_CAP);
	while (held--)
		vpt1_gw_mem_release(gws[0], chunk);
	EXPECT(t, vpt1_gw_mem_used(gws[0]) == 0);
	EXPECT(t, vpt1_host_mem_used() == host_before);

	/* The host ceiling across many gateways: 8 x 16 MiB = 128 MiB exactly, then nothing more. */
	for (i = 1; i < 8; i++) {
		gws[i] = new_gw(t, T, NULL);
		if (!gws[i])
			goto out;
	}
	for (i = 0; i < 8; i++) {
		unsigned int n = 0, k;

		for (k = 0; k < GW_CAP / chunk; k++)
			if (vpt1_gw_mem_reserve(gws[i], chunk))
				n++;
		EXPECT(t, n == GW_CAP / chunk); /* each gateway fills its own 16 MiB */
	}
	EXPECT(t, vpt1_host_mem_used() == HOST_CAP);
	/* A ninth gateway now gets nothing: the host is full even though it is empty itself. */
	gws[8] = new_gw(t, T, NULL);
	if (gws[8]) {
		EXPECT(t, !vpt1_gw_mem_reserve(gws[8], chunk));
		EXPECT(t, vpt1_gw_mem_used(gws[8]) == 0);
	}
	/* Release everything. */
	for (i = 0; i < 8; i++) {
		unsigned int k;

		for (k = 0; k < GW_CAP / chunk; k++)
			vpt1_gw_mem_release(gws[i], chunk);
		EXPECT(t, vpt1_gw_mem_used(gws[i]) == 0);
	}
	EXPECT(t, vpt1_host_mem_used() == host_before);

	/* A committed configuration shows up in the aggregate, and leaving it returns the bytes. */
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	EXPECT(t, s);
	if (s) {
		size_t after_commit, after_retire;

		snap_base(s);
		EXPECT(t, !stage_commit(gws[0], 1, s, T, NULL));
		vpt1_gw_lock(gws[0]);
		vpt1_gw_debug_accounting(gws[0], &acct);
		vpt1_gw_unlock(gws[0]);
		/* gw_mem is the config charge PLUS the per-peer error rings now charged at
		 * peer_alloc, so it is at least the charge, and the host total tracks it.
		 */
		EXPECT(t, acct.gw_mem >= acct.charged && acct.charged > 0);
		EXPECT(t, acct.host_mem >= acct.gw_mem);
		after_commit = acct.host_mem;

		/*
		 * VALIDATE fails closed when its own aggregate reserve cannot be met, and changes
		 * nothing. Shrink this gateway's ceiling to just under its current usage with a raw
		 * reserve of all remaining headroom, so the next config's charge cannot fit; the
		 * reserve inside vpt1_gw_validate fails and must leave gw_mem/host_mem untouched
		 * (catches a missing release on the validate failure path).
		 */
		{
			size_t held = 0, mem_before, host_val;

			/* Reserve all but a sliver of this gateway's ceiling in one go. */
			held = VPT1_GW_MEM_BUDGET - vpt1_gw_mem_used(gws[0]) - 64;
			EXPECT(t, vpt1_gw_mem_reserve(gws[0], held));
			mem_before = vpt1_gw_mem_used(gws[0]);
			host_val = vpt1_host_mem_used();
			s->rules[0].port_first = 8081;
			s->rules[0].port_last = 8081;
			vpt1_gw_lock(gws[0]);
			EXPECT(t, vpt1_gw_validate(gws[0], 2, s) == -ENOSPC);
			vpt1_gw_debug_accounting(gws[0], &acct);
			vpt1_gw_unlock(gws[0]);
			EXPECT(t, acct.gw_mem == mem_before && acct.host_mem == host_val &&
			       !acct.staged_charge);
			/* Drain the reserve; the same revision now commits. */
			vpt1_gw_mem_release(gws[0], held);
			EXPECT(t, !stage_commit(gws[0], 2, s, T, NULL));
		}

		/*
		 * A commit/retire cycle returns to one config's worth: revision 3 retires
		 * revision 2, and after the RCU grace the aggregate holds exactly one config
		 * plus the rings (catches a missing subtraction in cfg_destroy).
		 */
		s->rules[0].port_first = 8082;
		s->rules[0].port_last = 8082;
		EXPECT(t, !stage_commit(gws[0], 3, s, T, NULL));
		rcu_barrier();
		vpt1_gw_lock(gws[0]);
		vpt1_gw_debug_accounting(gws[0], &acct);
		vpt1_gw_unlock(gws[0]);
		after_retire = acct.host_mem;
		/* The retired revision's bytes came back: no retired config, and the host total
		 * is back to one-config-plus-rings (equal to the post-commit figure, since each
		 * commit here is the same size and the previous one was retired and reclaimed).
		 */
		EXPECT(t, acct.retired == 0 && after_retire == after_commit);
	}
out:
	for (i = 0; i < 9; i++)
		if (gws[i])
			end_gw(t, gws[i]);
	/* Every gateway is gone and its configs reclaimed: the host accumulator is clean again. */
	rcu_barrier();
	EXPECT(t, vpt1_host_mem_used() == 0);
	kfree(s);
}

static int try_existing(struct fixture *f, const struct vpt1_incarnation *inc,
			struct vpt1_generation *gen, u64 now,
			const struct vpt1_witness *w, enum vpt1_reason *why, bool done)
{
	struct vpt1_admit_ctx ctx;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peer, inc, VPT1_DIR_TX, gen, now, &ctx, why);
	if (!err)
		err = vpt1_admit_existing(&ctx, w, why);
	if (!err)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	if (!err && done)
		vpt1_admit_done(f->peer);
	return err;
}

static int try_new(struct fixture *f, u64 now, const struct vpt1_new_initiation *init,
		   enum vpt1_reason *why)
{
	struct vpt1_admit_ctx ctx;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peer, &f->inc, VPT1_DIR_TX, f->gen, now, &ctx, why);
	if (!err)
		err = vpt1_admit_new(&ctx, init, why);
	if (!err)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	if (!err)
		vpt1_admit_done(f->peer);
	return err;
}

static int try_error(struct fixture *f, u64 now, const struct vpt1_error_ctx *e,
		     enum vpt1_reason *why)
{
	struct vpt1_admit_ctx ctx;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peer, &f->inc, VPT1_DIR_TX, f->gen, now, &ctx, why);
	if (!err)
		err = vpt1_admit_error(&ctx, e, why);
	if (!err)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	if (!err)
		vpt1_admit_done(f->peer);
	return err;
}

static void associated_error_deadline(struct gw_test *t)
{
	u64 installed = ktime_get_boottime_ns();
	/* Inside the 15 s liveness window of the session installed at @installed: the retention deadline
	 * is under test here, not the closure of a peer that never answered a PING.
	 */
	u64 now = installed + 11 * SEC;
	struct vpt1_error_ctx e = { .created_ns = installed + SEC, .rule_id = 1,
		.flow = flow_for(1, 10), .held = true, .validated = true, .peer_slot = 1 };
	enum vpt1_reason why;
	struct fixture f;

	/* Keep the owner lease live while testing the independent error deadline.
	 * Keys are installed at real boot time, not in the future.
	 */
	if (fixture_make(t, &f, installed + 20 * SEC, installed))
		goto out;
	EXPECT(t, !try_error(&f, now - 1, &e, &why));
	EXPECT(t, try_error(&f, now, &e, &why) == -EINVAL && why == VPT1_R_BAD_CONTEXT);
	e.created_ns = now + 1;
	EXPECT(t, try_error(&f, now, &e, &why) == -EINVAL && why == VPT1_R_BAD_CONTEXT);
	e.created_ns = now;
	e.flow.peer_slot = 2;
	EXPECT(t, try_error(&f, now, &e, &why) == -EINVAL && why == VPT1_R_BAD_CONTEXT);
	e.flow.peer_slot = 1;
	e.rule_id = 2;
	EXPECT(t, try_error(&f, now, &e, &why) == -EACCES && why == VPT1_R_ACL_DENY);
	e.rule_id = 1;
	EXPECT(t, !try_error(&f, now, &e, &why));
out:
	fixture_free(t, &f);
}

static void admission_paths(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_witness w = { .rule_id = 1, .valid = true,
				  .flow = flow_for(1, 10) };
	struct vpt1_new_initiation init = { .protocol_validated = true,
					    .flow = flow_for(1, 10) };
	struct vpt1_error_ctx e = { .created_ns = T, .rule_id = 1, .flow = flow_for(1, 10),
		.held = true, .validated = true, .peer_slot = 1 };
	struct vpt1_incarnation other;
	enum vpt1_reason why;
	struct fixture f;
	unsigned int i, ok;

	if (fixture_make(t, &f, T, T))
		goto out;
	/* Existing flow, validated new initiation and associated error all pass. */
	EXPECT(t, !try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true));
	EXPECT(t, why == VPT1_R_OK);
	EXPECT(t, !try_new(&f, T + 1, &init, &why));
	EXPECT(t, !try_error(&f, T + 1, &e, &why));

	/* Each refusal reports its own reason and leaves no lock held. */
	other = f.inc;
	other.id[0] ^= 1;
	EXPECT(t, try_existing(&f, &other, f.gen, T + 1, &w, &why, true) == -ESTALE &&
		   why == VPT1_R_STALE_INCARNATION);
	{
		struct vpt1_generation *g2;
		u8 tx[32], rx[32];

		fill_key(tx, 0x11);
		fill_key(rx, 0x91);
		g2 = vpt1_generation_create(tx, rx, T);
		EXPECT(t, !IS_ERR(g2));
		if (!IS_ERR(g2)) {
			EXPECT(t, try_existing(&f, &f.inc, g2, T + 1, &w, &why, true) ==
				   -ENOKEY && why == VPT1_R_NO_GENERATION);
			vpt1_generation_put(g2);
		}
	}
	w.valid = false;
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true) == -ENOENT &&
		   why == VPT1_R_NO_WITNESS);
	w.valid = true;
	w.rule_id = 99;
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true) == -EACCES &&
		   why == VPT1_R_ACL_DENY);
	w.rule_id = 1;
	w.flow.destination_port = cpu_to_be16(8081);
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true) == -EACCES &&
		   why == VPT1_R_ACL_DENY);
	w.flow = flow_for(2, 10);
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true) == -EINVAL &&
		   why == VPT1_R_BAD_CONTEXT);
	w.flow = flow_for(1, 10);

	/* A new initiation must be protocol-validated and match a rule. */
	init.protocol_validated = false;
	EXPECT(t, try_new(&f, T + 1, &init, &why) == -EINVAL &&
		   why == VPT1_R_BAD_CONTEXT);
	init.protocol_validated = true;
	init.flow.destination_port = cpu_to_be16(9);
	EXPECT(t, try_new(&f, T + 1, &init, &why) == -EACCES && why == VPT1_R_ACL_DENY);
	init.flow = flow_for(1, 10);
	/* An error needs its own held, validated context. */
	e.validated = false;
	EXPECT(t, try_error(&f, T + 1, &e, &why) == -EINVAL &&
		   why == VPT1_R_BAD_CONTEXT);
	e.validated = true;

	/* The lease is a clock comparison: exact deadline denies, cleanup not run. */
	EXPECT(t, !try_existing(&f, &f.inc, f.gen, T + VPT1_LEASE_NS - 1, &w, &why,
				true));
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + VPT1_LEASE_NS, &w, &why, true) ==
		   -ESTALE && why == VPT1_R_LEASE_EXPIRED);

	/* Rates: a burst of 200 new flows per peer, then refusal, then refill. */
	ok = 0;
	for (i = 0; i < 400; i++)
		if (!try_new(&f, T + 2, &init, &why))
			ok++;
	EXPECT(t, ok >= 196 && ok <= 200);
	EXPECT(t, try_new(&f, T + 2, &init, &why) == -EBUSY && why == VPT1_R_RATE_LIMIT);
	EXPECT(t, !try_new(&f, T + 3 * SEC / 2, &init, &why));
	ok = 0;
	for (i = 0; i < 40; i++)
		if (!try_error(&f, T + 2, &e, &why))
			ok++;
	EXPECT(t, ok >= 1 && ok <= 20);

	/* In-flight cap: unfinished handoffs are bounded. */
	for (i = 0; i < VPT1_PEER_INFLIGHT_MAX; i++)
		if (try_existing(&f, &f.inc, f.gen, T + 3, &w, &why, false))
			break;
	EXPECT(t, i == VPT1_PEER_INFLIGHT_MAX);
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 3, &w, &why, true) == -EBUSY &&
		   why == VPT1_R_INFLIGHT_LIMIT);
	while (i--)
		vpt1_admit_done(f.peer);
	EXPECT(t, !try_existing(&f, &f.inc, f.gen, T + 3, &w, &why, true));
out:
	fixture_free(t, &f);
}

static void policy_tightening(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_witness w = { .rule_id = 1, .valid = true,
				  .flow = flow_for(1, 10) };
	struct vpt1_error_ctx e = { .created_ns = T, .rule_id = 1, .flow = flow_for(1, 10),
		.held = true, .validated = true, .peer_slot = 1 };
	struct vpt1_status st;
	enum vpt1_reason why;
	struct fixture f;
	bool dis;

	if (fixture_make(t, &f, T, T))
		goto out;
	EXPECT(t, !try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true));
	/* Rule 1 is removed by a policy-only commit: the saved ID is rechecked. */
	f.snap->rule_count = 1;
	f.snap->rules[0] = f.snap->rules[1];
	EXPECT(t, !stage_commit(f.gw, 2, f.snap, T + 1, &dis) && !dis);
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 2, &w, &why, true) == -EACCES &&
		   why == VPT1_R_ACL_DENY);
	get_status(f.gw, T + 2, &st);
	EXPECT(t, try_error(&f, T + 2, &e, &why) == -EACCES && why == VPT1_R_ACL_DENY);
	/* Same session, so admission still reaches the rule check (not stale). */
	EXPECT(t, status_peer(&st, 1)->state == VPT1_PEER_READY);
	/* A flow whose rule moved to another ID is not silently switched over. */
	w.rule_id = 2;
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 2, &w, &why, true) == -EACCES &&
		   why == VPT1_R_ACL_DENY);
out:
	fixture_free(t, &f);
}

static void generation_age(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	struct vpt1_witness w = { .rule_id = 1, .valid = true,
				  .flow = flow_for(1, 10) };
	enum vpt1_reason why;
	struct fixture f;

	/* The lease is claimed late and the generation installed at T0, so its
	 * hard age (180 s) has passed while the lease is still live.
	 */
	if (fixture_make(t, &f, T0 + 190 * SEC, T0))
		goto out;
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T0 + 191 * SEC, &w, &why, true) ==
		   -EKEYEXPIRED && why == VPT1_R_GENERATION_LIMIT);
out:
	fixture_free(t, &f);
}

/* Rekey (AY4): a new generation receives at once, sending stays on the current one until a
 * record authenticates under the new one or its timer runs out, and the old one receives for a
 * grace period but can never send again.
 */
static int admit_probe(struct fixture *f, enum vpt1_dir dir, struct vpt1_generation *gen, u64 now)
{
	struct vpt1_admit_ctx ctx;
	enum vpt1_reason why;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peer, &f->inc, dir, gen, now, &ctx, &why);
	if (!err)
		vpt1_admit_abort(&ctx);
	rcu_read_unlock();
	return err;
}

static struct vpt1_generation *new_gen(u8 seed, u64 at)
{
	u8 tx[32], rx[32];
	struct vpt1_generation *g;

	fill_key(tx, seed);
	fill_key(rx, seed + 0x80);
	g = vpt1_generation_create(tx, rx, at);
	return IS_ERR(g) ? NULL : g;
}

/* One reference stays with the test; the peer takes the other, or the test drops it on error. */
static int stage_gen(struct fixture *f, struct vpt1_generation *g, u64 local, u64 remote)
{
	int err;

	vpt1_generation_get(g);
	vpt1_gw_lock(f->gw);
	err = vpt1_gw_dbg_stage(f->gw, 1, &f->inc, g, local, remote);
	vpt1_gw_unlock(f->gw);
	if (err)
		vpt1_generation_put(g);
	return err;
}

static bool tx_is(struct fixture *f, struct vpt1_generation *want, u64 want_remote, u64 now)
{
	u64 remote = 0;
	struct vpt1_generation *got = vpt1_peer_tx_generation_get(f->peer, now, &remote);
	bool ok = got == want && (!want || remote == want_remote);

	if (got)
		vpt1_generation_put(got);
	return ok;
}

static bool rx_is(struct fixture *f, u64 locator, struct vpt1_generation *want, u64 now)
{
	struct vpt1_generation *got = vpt1_peer_rx_generation_get(f->peer, locator, now);
	bool ok = got == want;

	if (got)
		vpt1_generation_put(got);
	return ok;
}

static void rekey_overlap(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	struct vpt1_generation *g2, *g3, *g4, *g5, *g6;
	struct vpt1_peer *found;
	struct fixture f = {}, h = {};

	g2 = new_gen(0x11, T0);
	g3 = new_gen(0x21, T0);
	g4 = new_gen(0x31, T0);
	g5 = new_gen(0x41, T0);
	g6 = new_gen(0x51, T0);
	EXPECT(t, g2 && g3 && g4 && g5 && g6);
	if (!g2 || !g3 || !g4 || !g5 || !g6)
		goto out_gens;
	if (fixture_make(t, &f, T0 + 20 * SEC, T0))
		goto out_fixture;

	EXPECT(t, tx_is(&f, f.gen, 0x2222, T0 + SEC));
	/* Locators name exactly one generation of the peer: a current or next one is refused. */
	EXPECT(t, stage_gen(&f, g2, 0x1111, 0x9) == -EEXIST);
	EXPECT(t, !stage_gen(&f, g2, 0x3333, 0x4444));
	EXPECT(t, stage_gen(&f, g3, 0x3333, 0x9) == -EEXIST);

	/* A rekey: sending stays on the current generation; both receive. */
	EXPECT(t, tx_is(&f, f.gen, 0x2222, T0 + SEC));
	EXPECT(t, admit_probe(&f, VPT1_DIR_TX, g2, T0 + SEC) == -ENOKEY);
	EXPECT(t, !admit_probe(&f, VPT1_DIR_RX, g2, T0 + SEC));
	EXPECT(t, !admit_probe(&f, VPT1_DIR_RX, f.gen, T0 + SEC));
	EXPECT(t, !admit_probe(&f, VPT1_DIR_TX, f.gen, T0 + SEC));
	EXPECT(t, rx_is(&f, 0x1111, f.gen, T0 + SEC));
	EXPECT(t, rx_is(&f, 0x3333, g2, T0 + SEC));
	EXPECT(t, rx_is(&f, 0x7777, NULL, T0 + SEC) && rx_is(&f, 0, NULL, T0 + SEC));
	found = vpt1_gw_peer_by_locator(f.gw, 0x3333);
	EXPECT(t, found == f.peer);
	if (found)
		vpt1_peer_put(found);

	/* The candidate takes over sending when it is committed, and not before. */
	EXPECT(t, tx_is(&f, f.gen, 0x2222, T0 + 2 * SEC));
	vpt1_peer_commit(f.peer, T0 + 3 * SEC);
	EXPECT(t, tx_is(&f, g2, 0x4444, T0 + 3 * SEC));

	/* The old generation can no longer send, and receives only through its grace. */
	EXPECT(t, admit_probe(&f, VPT1_DIR_TX, f.gen, T0 + 4 * SEC) == -ENOKEY);
	EXPECT(t, !admit_probe(&f, VPT1_DIR_RX, f.gen, T0 + 12 * SEC));
	EXPECT(t, rx_is(&f, 0x1111, f.gen, T0 + 12 * SEC));
	EXPECT(t, admit_probe(&f, VPT1_DIR_RX, f.gen, T0 + 14 * SEC) == -ENOKEY);
	EXPECT(t, rx_is(&f, 0x1111, NULL, T0 + 14 * SEC));
	EXPECT(t, rx_is(&f, 0x3333, g2, T0 + 14 * SEC));

	/* No timer commits a candidate: it takes over only when the protocol (or the test) commits it. */
	EXPECT(t, !stage_gen(&f, g3, 0x5555, 0x6666));
	EXPECT(t, tx_is(&f, g2, 0x4444, T0 + SEC));
	EXPECT(t, tx_is(&f, g2, 0x4444, T0 + 15 * SEC));
	vpt1_peer_commit(f.peer, T0 + 15 * SEC);
	EXPECT(t, tx_is(&f, g3, 0x6666, T0 + 15 * SEC));
	EXPECT(t, rx_is(&f, 0x3333, g2, T0 + 15 * SEC)); /* Now prev. */

	/* A second rekey before the first one took over replaces the waiting generation. */
	EXPECT(t, !stage_gen(&f, g4, 0x7000, 0x7001));
	EXPECT(t, !stage_gen(&f, g5, 0x8000, 0x8001));
	EXPECT(t, rx_is(&f, 0x7000, NULL, T0 + SEC));
	EXPECT(t, rx_is(&f, 0x8000, g5, T0 + SEC));
	EXPECT(t, tx_is(&f, g3, 0x6666, T0 + SEC));

	/* A current generation past its hard age is replaced at once: nothing worth keeping. */
	if (!fixture_make(t, &h, T0 + 20 * SEC, T0 - 181 * SEC)) {
		EXPECT(t, !stage_gen(&h, g6, 0x9000, 0x9001));
		EXPECT(t, tx_is(&h, g6, 0x9001, T0 + SEC));
		EXPECT(t, rx_is(&h, 0x1111, NULL, T0 + SEC));
		EXPECT(t, rx_is(&h, 0x9000, g6, T0 + SEC));
	}
	fixture_free(t, &h);
out_fixture:
	fixture_free(t, &f);
out_gens:
	if (g2)
		vpt1_generation_put(g2);
	if (g3)
		vpt1_generation_put(g3);
	if (g4)
		vpt1_generation_put(g4);
	if (g5)
		vpt1_generation_put(g5);
	if (g6)
		vpt1_generation_put(g6);
}

/* Allocation failure: fail each allocation point of a gateway's life once. ------------------ */

/*
 * One pass: create a gateway, claim it, commit three configurations (the first adds two peers, the second
 * changes only policy, the third replaces one peer), lose the owner and clean up. Whichever allocation is
 * armed to fail must produce -ENOMEM (or a NULL create) and leave everything as it was, a retry of the same
 * step must then succeed, and nothing may be left behind (the caller checks the object counts).
 */
static void alloc_pass(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_snapshot *s;
	struct vpt1_status st;
	struct vpt1_gw *gw;
	unsigned int rev;
	bool dis;
	u64 e;
	int err;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	EXPECT(t, s);
	if (!s)
		return;
	snap_base(s);
	gw = vpt1_gw_create(2, T);
	if (!gw)
		goto out; /* A failed create leaves nothing behind. */
	vpt1_gw_lock(gw);
	err = vpt1_gw_claim(gw, ID_A, 100, T, &e);
	vpt1_gw_unlock(gw);
	EXPECT(t, !err);

	for (rev = 1; rev <= 3; rev++) {
		if (rev == 2)
			s->rule_count = 1;
		if (rev == 3)
			s->peers[1].endpoint = IPV4(172, 31, 250, 22);
		err = stage_commit(gw, rev, s, T, &dis);
		if (err) {
			EXPECT(t, err == -ENOMEM);
			get_status(gw, T, &st);
			EXPECT(t, st.revision == rev - 1);
			EXPECT(t, st.peer_count == (rev == 1 ? 0 : 2));
			EXPECT(t, !stage_commit(gw, rev, s, T, &dis));
		}
		get_status(gw, T, &st);
		EXPECT(t, st.revision == rev && st.peer_count == 2);
	}

	/* The owner goes away; if the loss record cannot be allocated, expiry does the same cleanup. */
	vpt1_gw_owner_released(gw, 100);
	vpt1_gw_flush();
	vpt1_gw_expire_check(gw, T + 6 * SEC);
	vpt1_gw_flush();
	get_status(gw, T + 6 * SEC, &st);
	EXPECT(t, st.revision == 0 && st.peer_count == 0);
	end_gw(t, gw);
out:
	kfree(s);
}

static void alloc_failures(struct gw_test *t)
{
	long gw0 = vpt1_gw_live(), peer0 = vpt1_peer_live(), cfg0 = vpt1_cfg_live();
	unsigned int n, points = 0;

	for (n = 1; n <= 64; n++) {
		atomic_set(&vpt1_dbg_fail_hit, 0);
		atomic_set(&vpt1_dbg_fail_nth, n);
		alloc_pass(t);
		atomic_set(&vpt1_dbg_fail_nth, 0);
		rcu_barrier();
		rcu_barrier();
		EXPECT(t, vpt1_gw_live() == gw0 && vpt1_peer_live() == peer0 && vpt1_cfg_live() == cfg0);
		if (!atomic_read(&vpt1_dbg_fail_hit))
			break; /* The pass made fewer than n allocations: every point has failed once. */
		points++;
	}
	EXPECT(t, n <= 64);
	EXPECT(t, points >= 8);
	pr_info("VPT1_ALLOC_FAIL: points=%u\n", points);
}

/* HR3 runtime adapter: continuity, shared fragment budget, bounded worker, live cache. ---------- */

static u64 cont_id_of(struct vpt1_peer *peer)
{
	struct vpt1_continuity *c = vpt1_peer_continuity_get(peer);
	u64 id = 0;

	if (c) {
		id = vpt1_continuity_id(c);
		vpt1_continuity_put(c);
	}
	return id;
}

static void continuity_lifecycle(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	struct vpt1_generation *g2 = new_gen(0x11, T0), *g3 = new_gen(0x21, T0);
	struct vpt1_generation *g4 = new_gen(0x31, T0);
	struct vpt1_continuity *c1 = NULL, *c2 = NULL;
	struct fixture f = {}, h = {};
	struct vpt1_peer *np;
	long live = vpt1_continuity_live();
	u64 id1, hid;
	int err;

	EXPECT(t, g2 && g3 && g4);
	if (!g2 || !g3 || !g4 || fixture_make(t, &f, T0 + 20 * SEC, T0))
		goto out;
	c1 = vpt1_peer_continuity_get(f.peer);
	EXPECT(t, c1 && !vpt1_continuity_dead(c1));
	if (!c1)
		goto out;
	id1 = vpt1_continuity_id(c1);
	EXPECT(t, id1);

	/* An ordinary rekey, and the promotion that follows it, keep the same continuity. */
	EXPECT(t, !stage_gen(&f, g2, 0x3333, 0x4444));
	EXPECT(t, cont_id_of(f.peer) == id1);
	vpt1_peer_commit(f.peer, T0 + SEC);
	EXPECT(t, tx_is(&f, g2, 0x4444, T0 + SEC));
	EXPECT(t, cont_id_of(f.peer) == id1 && !vpt1_continuity_dead(c1));

	/* Disable kills it: no new reference, though the holder's own stays valid memory. */
	vpt1_gw_lock(f.gw);
	err = vpt1_gw_disable_peer(f.gw, 1, &f.inc);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !err);
	EXPECT(t, vpt1_continuity_dead(c1) && !vpt1_continuity_try_get(c1));
	EXPECT(t, !vpt1_peer_continuity_get(f.peer));
	EXPECT(t, vpt1_continuity_id(c1) == id1);

	/* A fresh establishment on the same peer is a new continuity. */
	EXPECT(t, !stage_gen(&f, g3, 0x5555, 0x6666));
	c2 = vpt1_peer_continuity_get(f.peer);
	EXPECT(t, c2 && c2 != c1 && vpt1_continuity_id(c2) != id1 && !vpt1_continuity_dead(c2));

	/* Revoke replaces the peer: the old continuity dies and the new peer has none yet. */
	vpt1_gw_lock(f.gw);
	err = vpt1_gw_revoke_peer(f.gw, 1, &f.inc);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !err);
	EXPECT(t, c2 && vpt1_continuity_dead(c2));
	np = vpt1_gw_peer_get(f.gw, 1);
	EXPECT(t, np && np != f.peer && !cont_id_of(np));
	if (np)
		vpt1_peer_put(np);

	/* A current generation past its hard age is replaced, which is a new session too. */
	if (!fixture_make(t, &h, T0 + 20 * SEC, T0 - 181 * SEC)) {
		hid = cont_id_of(h.peer);
		EXPECT(t, hid);
		EXPECT(t, !stage_gen(&h, g4, 0x9000, 0x9001));
		EXPECT(t, cont_id_of(h.peer) && cont_id_of(h.peer) != hid);
	}
	fixture_free(t, &h);
out:
	vpt1_continuity_put(c1);
	vpt1_continuity_put(c2);
	fixture_free(t, &f);
	if (g2)
		vpt1_generation_put(g2);
	if (g3)
		vpt1_generation_put(g3);
	if (g4)
		vpt1_generation_put(g4);
	EXPECT(t, vpt1_continuity_live() == live);
}

static void fragment_budget(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_gw *gw = new_gw(t, T, NULL);

	if (!gw)
		return;
	EXPECT(t, vpt1_gw_frag_used(gw) == 0);
	EXPECT(t, !vpt1_gw_frag_reserve(gw, VPT1_FRAG_BUDGET + 1));
	EXPECT(t, vpt1_gw_frag_used(gw) == 0);
	EXPECT(t, vpt1_gw_frag_reserve(gw, VPT1_FRAG_BUDGET - 10));
	EXPECT(t, vpt1_gw_frag_reserve(gw, 10));
	EXPECT(t, !vpt1_gw_frag_reserve(gw, 1)); /* Exactly full. */
	EXPECT(t, vpt1_gw_frag_used(gw) == VPT1_FRAG_BUDGET);
	vpt1_gw_frag_release(gw, 10);
	EXPECT(t, vpt1_gw_frag_reserve(gw, 10) && !vpt1_gw_frag_reserve(gw, 1));
	vpt1_gw_frag_release(gw, 10);
	vpt1_gw_frag_release(gw, VPT1_FRAG_BUDGET - 10);
	EXPECT(t, vpt1_gw_frag_used(gw) == 0);
	end_gw(t, gw);
}

struct tjob {
	struct vpt1_job job;
	struct completion started;
	struct completion go;
	bool block;
	atomic_t ran;
	atomic_t cancelled;
};

static void tjob_run(struct vpt1_job *job, bool cancelled)
{
	struct tjob *j = container_of(job, struct tjob, job);

	if (cancelled) {
		atomic_inc(&j->cancelled);
		return;
	}
	atomic_inc(&j->ran);
	if (j->block) {
		complete(&j->started);
		wait_for_completion(&j->go);
	}
}

static void tjob_init(struct tjob *j, bool block)
{
	memset(j, 0, sizeof(*j));
	j->job.run = tjob_run;
	j->block = block;
	init_completion(&j->started);
	init_completion(&j->go);
}

static bool tjob_done(struct tjob *j, int ran, int cancelled)
{
	return atomic_read(&j->ran) == ran && atomic_read(&j->cancelled) == cancelled;
}

struct revoker {
	struct vpt1_gw *gw;
	struct vpt1_incarnation inc;
	struct completion done;
	int err;
};

static int revoker_fn(void *arg)
{
	struct revoker *r = arg;

	vpt1_gw_lock(r->gw);
	r->err = vpt1_gw_revoke_peer(r->gw, 1, &r->inc);
	vpt1_gw_unlock(r->gw);
	complete(&r->done);
	return 0;
}

static int releaser_fn(void *arg)
{
	struct tjob *j = arg;

	msleep(100);
	complete(&j->go);
	return 0;
}

static void job_queue(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	struct tjob *jobs = kcalloc(VPT1_JOB_QUEUE_MAX + 8, sizeof(*jobs), GFP_KERNEL);
	struct tjob blocker, other, late;
	struct revoker r;
	struct fixture f = {};
	struct vpt1_peer *p2 = NULL;
	struct task_struct *th;
	unsigned int i;
	int err;

	EXPECT(t, jobs);
	if (!jobs || fixture_make(t, &f, T0 + 20 * SEC, T0))
		goto out;
	p2 = vpt1_gw_peer_get(f.gw, 2);
	EXPECT(t, p2);
	if (!p2)
		goto out;

	/* A job runs once on the worker, not cancelled. */
	tjob_init(&other, false);
	EXPECT(t, !vpt1_job_queue(f.peer, &other.job));
	vpt1_gw_flush();
	EXPECT(t, tjob_done(&other, 1, 0) && !vpt1_job_pending(f.gw));

	/* The queue is bounded: one job running, VPT1_JOB_QUEUE_MAX waiting, the next is refused. */
	tjob_init(&blocker, true);
	EXPECT(t, !vpt1_job_queue(p2, &blocker.job));
	wait_for_completion(&blocker.started);
	for (i = 0; i < VPT1_JOB_QUEUE_MAX; i++) {
		tjob_init(&jobs[i], false);
		err = vpt1_job_queue(i % 2 ? p2 : f.peer, &jobs[i].job);
		EXPECT(t, !err);
	}
	tjob_init(&late, false);
	EXPECT(t, vpt1_job_queue(f.peer, &late.job) == -EBUSY);
	EXPECT(t, vpt1_job_pending(f.gw) == VPT1_JOB_QUEUE_MAX);

	/* Revoking peer 1 cancels its queued jobs, leaves peer 2's, and refuses new ones for peer 1. */
	memset(&r, 0, sizeof(r));
	r.gw = f.gw;
	r.inc = f.inc;
	init_completion(&r.done);
	th = kthread_run(revoker_fn, &r, "vpt1_revoker");
	EXPECT(t, !IS_ERR(th));
	if (!IS_ERR(th))
		wait_for_completion(&r.done);
	EXPECT(t, !r.err);
	for (i = 0; i < VPT1_JOB_QUEUE_MAX; i++)
		EXPECT(t, i % 2 ? tjob_done(&jobs[i], 0, 0) : tjob_done(&jobs[i], 0, 1));
	EXPECT(t, vpt1_job_pending(f.gw) == VPT1_JOB_QUEUE_MAX / 2);
	EXPECT(t, vpt1_job_queue(f.peer, &late.job) == -ESTALE);

	/* Let the worker go: peer 2's jobs run, exactly once. */
	complete(&blocker.go);
	vpt1_gw_flush();
	for (i = 0; i < VPT1_JOB_QUEUE_MAX; i++)
		EXPECT(t, i % 2 ? tjob_done(&jobs[i], 1, 0) : tjob_done(&jobs[i], 0, 1));
	EXPECT(t, tjob_done(&blocker, 1, 0) && !vpt1_job_pending(f.gw));

	/* Revoke waits for a job of that peer that is already running. */
	tjob_init(&blocker, true);
	vpt1_peer_put(f.peer);
	f.peer = vpt1_gw_peer_get(f.gw, 1);
	EXPECT(t, f.peer);
	if (!f.peer)
		goto out;
	vpt1_peer_incarnation(f.peer, &f.inc);
	EXPECT(t, !vpt1_job_queue(f.peer, &blocker.job));
	wait_for_completion(&blocker.started);
	memset(&r, 0, sizeof(r));
	r.gw = f.gw;
	r.inc = f.inc;
	init_completion(&r.done);
	th = kthread_run(revoker_fn, &r, "vpt1_revoker");
	EXPECT(t, !IS_ERR(th));
	EXPECT(t, !wait_for_completion_timeout(&r.done, msecs_to_jiffies(150)));
	complete(&blocker.go);
	EXPECT(t, wait_for_completion_timeout(&r.done, msecs_to_jiffies(5000)));
	EXPECT(t, !r.err && tjob_done(&blocker, 1, 0));
out:
	if (p2)
		vpt1_peer_put(p2);
	fixture_free(t, &f);
	kfree(jobs);
}

/* Teardown refuses new work, cancels what is queued and waits for the running job. */
static void job_teardown(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_gw *gw = new_gw(t, T, NULL);
	struct vpt1_snapshot *s = kzalloc(sizeof(*s), GFP_KERNEL);
	struct tjob blocker, queued, after;
	struct vpt1_peer *p = NULL;
	struct task_struct *th;
	bool dis;
	int err;

	EXPECT(t, gw && s);
	if (!gw || !s)
		goto out;
	snap_base(s);
	EXPECT(t, !stage_commit(gw, 1, s, T, &dis));
	p = vpt1_gw_peer_get(gw, 1);
	EXPECT(t, p);
	if (!p)
		goto out;
	tjob_init(&blocker, true);
	tjob_init(&queued, false);
	tjob_init(&after, false);
	EXPECT(t, !vpt1_job_queue(p, &blocker.job));
	wait_for_completion(&blocker.started);
	EXPECT(t, !vpt1_job_queue(p, &queued.job));
	th = kthread_run(releaser_fn, &blocker, "vpt1_releaser");
	EXPECT(t, !IS_ERR(th));
	vpt1_gw_destroy(gw, 0); /* Waits for the blocker, which the thread releases. */
	EXPECT(t, tjob_done(&blocker, 1, 0) && tjob_done(&queued, 0, 1));
	err = vpt1_job_queue(p, &after.job);
	EXPECT(t, err == -ESHUTDOWN || err == -ESTALE);
	EXPECT(t, tjob_done(&after, 0, 0));
	vpt1_peer_put(p);
	p = NULL;
	rcu_barrier();
	rcu_barrier();
	gw = NULL;
out:
	if (p)
		vpt1_peer_put(p);
	if (gw)
		end_gw(t, gw);
	kfree(s);
}

/* The real fragment cache on the real continuity, budget and clock. */
static u8 *test_fragment(u16 id, u32 offset, u32 length, bool more)
{
	u8 *data = kzalloc(20 + length, GFP_KERNEL);
	__sum16 sum;

	if (!data)
		return NULL;
	data[0] = 0x45;
	put_unaligned_be16(20 + length, data + 2);
	put_unaligned_be16(id, data + 4);
	put_unaligned_be16(offset / 8 | (more ? IP_MF : 0), data + 6);
	data[8] = 61;
	data[9] = IPPROTO_UDP;
	data[12] = 10;
	data[13] = 77;
	data[14] = 1;
	data[15] = 10;
	data[16] = 10;
	data[17] = 77;
	data[18] = 2;
	data[19] = 10;
	memset(data + 20, 0x5a, length);
	sum = ip_fast_csum(data, 5);
	memcpy(data + 10, &sum, sizeof(sum));
	return data;
}

static int frag_send(struct vpt1_frag_cache *cache, void *context, u16 id, u32 offset,
		     u32 length, bool more, u64 now, struct vpt1_frag_result **result)
{
	struct vpt1_frag_scope scope = { .context = context, .ingress = VPT1_FRAG_TUNNEL };
	u8 *data = test_fragment(id, offset, length, more);
	int err;

	if (!data)
		return -ENOMEM;
	err = vpt1_frag_submit(cache, &scope, data, 20 + length, now, result);
	kfree(data);
	return err;
}

static const struct vpt1_frag_ops frag_ops = {
	.get = vpt1_continuity_try_get,
	.put = vpt1_continuity_put,
	.reserve = vpt1_gw_frag_reserve,
	.release = vpt1_gw_frag_release,
};

static void fragment_integration(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	struct vpt1_generation *g2 = new_gen(0x11, T0);
	struct vpt1_frag_cache *cache, *big;
	struct vpt1_frag_result *res = NULL;
	struct vpt1_continuity *c1 = NULL, *c2 = NULL;
	struct vpt1_frag_usage u;
	struct fixture f = {};
	size_t base;
	int err;

	EXPECT(t, g2);
	if (!g2 || fixture_make(t, &f, T0 + 20 * SEC, T0))
		goto out;
	c1 = vpt1_peer_continuity_get(f.peer);
	EXPECT(t, c1);
	if (!c1)
		goto out;
	cache = vpt1_frag_create(&frag_ops, f.gw);
	EXPECT(t, !IS_ERR(cache));
	if (IS_ERR(cache))
		goto out;
	base = vpt1_gw_frag_used(f.gw);
	EXPECT(t, base == vpt1_frag_cache_charge());

	/* Two fragments complete a packet; the result holds charge until it is freed. */
	EXPECT(t, frag_send(cache, c1, 7, 0, 16, true, T0, &res) == 0 && !res);
	EXPECT(t, vpt1_gw_frag_used(f.gw) > base);
	EXPECT(t, frag_send(cache, c1, 7, 16, 8, false, T0 + 1, &res) == 1 && res);
	vpt1_frag_result_free(res);
	res = NULL;
	vpt1_frag_usage(cache, &u);
	EXPECT(t, !u.sets && !u.results && vpt1_gw_frag_used(f.gw) == base);

	/* An ordinary rekey does not disturb a set in progress. */
	EXPECT(t, frag_send(cache, c1, 8, 0, 16, true, T0, &res) == 0);
	EXPECT(t, !stage_gen(&f, g2, 0x3333, 0x4444));
	vpt1_peer_commit(f.peer, T0 + SEC);
	EXPECT(t, cont_id_of(f.peer) == vpt1_continuity_id(c1));
	EXPECT(t, frag_send(cache, c1, 8, 16, 8, false, T0 + 2, &res) == 1 && res);
	vpt1_frag_result_free(res);
	res = NULL;

	/* A set is dropped by expiry, and its charge returns. */
	EXPECT(t, frag_send(cache, c1, 9, 0, 16, true, T0, &res) == 0);
	vpt1_frag_usage(cache, &u);
	EXPECT(t, u.sets == 1 && vpt1_gw_frag_used(f.gw) > base);
	EXPECT(t, vpt1_frag_expire(cache, T0 + 6 * SEC) == 1);
	vpt1_frag_usage(cache, &u);
	EXPECT(t, !u.sets && vpt1_gw_frag_used(f.gw) == base);

	/* Disabling the peer kills the continuity: a pending set is flushed, a late fragment refused. */
	EXPECT(t, frag_send(cache, c1, 10, 0, 16, true, T0 + 10 * SEC, &res) == 0);
	vpt1_gw_lock(f.gw);
	err = vpt1_gw_disable_peer(f.gw, 1, &f.inc);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !err && vpt1_continuity_dead(c1));
	vpt1_frag_flush_context(cache, c1);
	vpt1_frag_usage(cache, &u);
	EXPECT(t, !u.sets && vpt1_gw_frag_used(f.gw) == base);
	err = frag_send(cache, c1, 10, 16, 8, false, T0 + 11 * SEC, &res);
	EXPECT(t, err < 0 && !res);
	vpt1_frag_usage(cache, &u);
	EXPECT(t, !u.sets && vpt1_gw_frag_used(f.gw) == base);

	/* A new session has a new continuity, so the old set cannot finish under it. */
	EXPECT(t, !stage_gen(&f, g2, 0x5555, 0x6666));
	c2 = vpt1_peer_continuity_get(f.peer);
	EXPECT(t, c2 && c2 != c1);
	EXPECT(t, c2 && frag_send(cache, c2, 10, 16, 8, false, T0 + 12 * SEC, &res) == 0);
	vpt1_frag_flush_context(cache, c2);

	/* The shared budget bounds the cache: with it used up, a second cache cannot even be made. */
	EXPECT(t, vpt1_gw_frag_reserve(f.gw, VPT1_FRAG_BUDGET - vpt1_gw_frag_used(f.gw)));
	big = vpt1_frag_create(&frag_ops, f.gw);
	EXPECT(t, IS_ERR(big));
	if (!IS_ERR(big))
		vpt1_frag_destroy(big);
	EXPECT(t, c2 && frag_send(cache, c2, 11, 0, 16, true, T0 + 13 * SEC, &res) < 0);
	vpt1_gw_frag_release(f.gw, VPT1_FRAG_BUDGET - base);

	/* Close and destroy return every byte. */
	vpt1_frag_close(cache);
	EXPECT(t, !vpt1_frag_destroy(cache));
	EXPECT(t, vpt1_gw_frag_used(f.gw) == 0);
out:
	vpt1_continuity_put(c1);
	vpt1_continuity_put(c2);
	fixture_free(t, &f);
	if (g2)
		vpt1_generation_put(g2);
}

/*
 * HR3/HR4 live adapter: ownership helpers, per-peer cache lifecycle, recent-error table. ----------
 */

static u16 csum16(const u8 *p, unsigned int len)
{
	u32 sum = 0;

	while (len > 1) {
		sum += ((u32)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (u32)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

/*
 * A 40-byte IPv4/TCP packet; only the IPv4 header checksum is repaired (the save never checks the
 * rest).
 */
static unsigned int tcp_packet(u8 *p, __be32 src, __be32 dst, u16 sport, u16 dport)
{
	memset(p, 0, 40);
	p[0] = 0x45;
	put_unaligned_be16(40, p + 2);
	put_unaligned_be16(0x1234, p + 4);
	p[8] = 63;
	p[9] = IPPROTO_TCP;
	memcpy(p + 12, &src, 4);
	memcpy(p + 16, &dst, 4);
	put_unaligned_be16(sport, p + 20);
	put_unaligned_be16(dport, p + 22);
	p[32] = 0x50;
	p[33] = 2;
	put_unaligned_be16(csum16(p, 20), p + 10);
	return 40;
}

/*
 * An ICMP error (64 bytes: 28 + a 36 byte quotation) with valid checksums, from @src toward the
 * quoted source.
 */
static unsigned int icmp_error(u8 *e, __be32 src, const u8 *quote, u8 type, u8 code)
{
	memset(e, 0, 64);
	e[0] = 0x45;
	put_unaligned_be16(64, e + 2);
	e[8] = 64;
	e[9] = IPPROTO_ICMP;
	memcpy(e + 12, &src, 4);
	memcpy(e + 16, quote + 12, 4); /* Back to the quoted packet's source. */
	e[20] = type;
	e[21] = code;
	memcpy(e + 28, quote, 36);
	put_unaligned_be16(csum16(e + 20, 44), e + 22);
	put_unaligned_be16(csum16(e, 20), e + 10);
	return 64;
}

static void error_fix_csum(u8 *e)
{
	e[22] = 0;
	e[23] = 0;
	put_unaligned_be16(csum16(e + 20, 44), e + 22);
}

static void adapter_helpers(struct gw_test *t)
{
	struct fixture f = {};
	u64 T = ktime_get_boottime_ns();

	if (fixture_make(t, &f, T + 20 * SEC, T))
		goto out;
	rcu_read_lock();
	EXPECT(t, vpt1_gw_lan_has(f.gw, IPV4(10, 77, 1, 10), false));
	/* The gateway's own address. */
	EXPECT(t, !vpt1_gw_lan_has(f.gw, IPV4(10, 77, 1, 1), false));
	EXPECT(t, vpt1_gw_lan_has(f.gw, IPV4(10, 77, 1, 1), true));
	/* Network and broadcast addresses. */
	EXPECT(t, !vpt1_gw_lan_has(f.gw, IPV4(10, 77, 1, 0), true));
	EXPECT(t, !vpt1_gw_lan_has(f.gw, IPV4(10, 77, 1, 255), true));
	EXPECT(t, !vpt1_gw_lan_has(f.gw, IPV4(10, 77, 2, 10), false)); /* The peer's side. */
	EXPECT(t, !vpt1_gw_lan_has(f.gw, IPV4(224, 0, 0, 1), true));
	EXPECT(t, !vpt1_gw_lan_has(f.gw, IPV4(127, 0, 0, 1), true));
	EXPECT(t, vpt1_peer_owns(f.peer, IPV4(10, 77, 2, 10)) &&
	       vpt1_peer_owns(f.peer, IPV4(10, 77, 2, 1)));
	EXPECT(t, !vpt1_peer_owns(f.peer, IPV4(10, 77, 3, 10)) &&
	       !vpt1_peer_owns(f.peer, IPV4(10, 77, 1, 10)));
	EXPECT(t, !vpt1_peer_owns(f.peer, IPV4(255, 255, 255, 255)) &&
	       !vpt1_peer_owns(f.peer, IPV4(0, 0, 0, 0)));
	EXPECT(t, vpt1_peer_cap(f.peer) == 1428);
	rcu_read_unlock();
	EXPECT(t, vpt1_peer_eligible(f.peer, T + SEC));
	EXPECT(t, !vpt1_peer_eligible(f.peer, T + 26 * SEC)); /* The lease has lapsed. */
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_gw_disable_peer(f.gw, 1, &f.inc));
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !vpt1_peer_eligible(f.peer, T + SEC));
out:
	fixture_free(t, &f);
}

static void adapter_frag_cache(struct gw_test *t)
{
	struct vpt1_generation *g3 = new_gen(0x21, ktime_get_boottime_ns());
	struct vpt1_generation *g4 = new_gen(0x31, ktime_get_boottime_ns());
	u64 T = ktime_get_boottime_ns();
	struct vpt1_continuity *c1 = NULL, *c2 = NULL;
	struct vpt1_frag_cache *cache, *cache2;
	struct vpt1_frag_result *res = NULL;
	struct fixture f = {}, h = {}, k = {};
	struct vpt1_frag_usage u;
	size_t base;
	long live = vpt1_continuity_live();

	EXPECT(t, g3 && g4);
	if (!g3 || !g4 || fixture_make(t, &f, T + 20 * SEC, T))
		goto out;
	c1 = vpt1_peer_continuity_get(f.peer);
	cache = vpt1_peer_frag_cache(f.peer);
	EXPECT(t, c1 && !IS_ERR(cache) && vpt1_peer_frag_cache(f.peer) == cache);
	if (!c1 || IS_ERR(cache))
		goto out;
	base = vpt1_gw_frag_used(f.gw);
	EXPECT(t, base == vpt1_frag_cache_charge());

	/*
	 * Disable kills the continuity and flushes its open sets without any call from the cache's
	 * user.
	 */
	EXPECT(t, frag_send(cache, c1, 7, 0, 16, true, T, &res) == 0 && !res);
	vpt1_frag_usage(cache, &u);
	EXPECT(t, u.sets == 1 && vpt1_gw_frag_used(f.gw) > base);
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_gw_disable_peer(f.gw, 1, &f.inc));
	vpt1_gw_unlock(f.gw);
	vpt1_frag_usage(cache, &u);
	EXPECT(t, !u.sets && vpt1_gw_frag_used(f.gw) == base);

	/*
	 * A new session has a new continuity; revoke then destroys the cache and returns every
	 * byte.
	 */
	EXPECT(t, !stage_gen(&f, g3, 0x5555, 0x6666));
	c2 = vpt1_peer_continuity_get(f.peer);
	EXPECT(t, c2 && c2 != c1);
	EXPECT(t, c2 && frag_send(cache, c2, 8, 0, 16, true, T, &res) == 0);
	vpt1_frag_usage(cache, &u);
	EXPECT(t, u.sets == 1);
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_gw_revoke_peer(f.gw, 1, &f.inc));
	vpt1_gw_unlock(f.gw);
	EXPECT(t, vpt1_gw_frag_used(f.gw) == 0);
	EXPECT(t, IS_ERR(vpt1_peer_frag_cache(f.peer)) &&
	       PTR_ERR(vpt1_peer_frag_cache(f.peer)) == -ESTALE);

	/*
	 * A fresh establishment over an aged-out generation kills the old continuity: its open set
	 * can no longer complete and is left to age out.
	 */
	if (!fixture_make(t, &h, T + 20 * SEC, T - 181 * SEC)) {
		struct vpt1_continuity *old = vpt1_peer_continuity_get(h.peer);

		cache2 = vpt1_peer_frag_cache(h.peer);
		EXPECT(t, old && !IS_ERR(cache2));
		if (old && !IS_ERR(cache2)) {
			EXPECT(t, frag_send(cache2, old, 9, 0, 16, true, T, &res) == 0);
			EXPECT(t, !stage_gen(&h, g4, 0x9000, 0x9001));
			EXPECT(t, vpt1_continuity_dead(old));
			/* The cache completes the set; the adapter drops it (dead context). */
			EXPECT(t, frag_send(cache2, old, 9, 16, 8, false, T + SEC, &res) == 1);
			EXPECT(t, res);
			if (res) {
				struct vpt1_frag_view view;

				vpt1_frag_view(res, &view);
				EXPECT(t, vpt1_continuity_dead(view.context));
				vpt1_frag_result_free(res);
				res = NULL;
			}
			vpt1_frag_usage(cache2, &u);
			EXPECT(t, !u.sets && vpt1_gw_frag_used(h.gw) == vpt1_frag_cache_charge());
		}
		vpt1_continuity_put(old);
	}
	/*
	 * The periodic expiry: a set that is already stale is dropped with no packet to drive it.
	 * Run on a fresh cache, whose clock watermark has not yet moved past the stale sample. /
	 */
	if (!fixture_make(t, &k, T + 20 * SEC, T)) {
		struct vpt1_continuity *cur = vpt1_peer_continuity_get(k.peer);

		cache2 = vpt1_peer_frag_cache(k.peer);
		EXPECT(t, cur && !IS_ERR(cache2));
		if (cur && !IS_ERR(cache2)) {
			EXPECT(t, frag_send(cache2, cur, 10, 0, 16, true,
					    ktime_get_boottime_ns() - 6 * SEC, &res) == 0);
			vpt1_gw_frag_work_arm(k.gw);
			msleep(1600);
			vpt1_frag_usage(cache2, &u);
			EXPECT(t, !u.sets && vpt1_gw_frag_used(k.gw) == vpt1_frag_cache_charge());
			/* A pending work item and an open set when the gateway goes away. */
			EXPECT(t,
			       frag_send(cache2, cur, 11, 0, 16, true, ktime_get_boottime_ns(),
					 &res) == 0);
			vpt1_gw_frag_work_arm(k.gw);
		}
		vpt1_continuity_put(cur);
	}
out:
	fixture_free(t, &k);
	fixture_free(t, &h);
	vpt1_continuity_put(c1);
	vpt1_continuity_put(c2);
	fixture_free(t, &f);
	if (g3)
		vpt1_generation_put(g3);
	if (g4)
		vpt1_generation_put(g4);
	EXPECT(t, vpt1_continuity_live() == live);
}

static int recheck_err(struct fixture *f, const struct vpt1_error_ctx *e, unsigned int length,
		       u64 now, enum vpt1_reason *why)
{
	int err;

	rcu_read_lock(); /* As in the data path: admission reads the current configuration. */
	err = vpt1_admit_tx_recheck_error(f->peer, &f->inc, f->gen, e, length, now, why);
	rcu_read_unlock();
	return err;
}

static void adapter_errors(struct gw_test *t)
{
	u8 pkt[64], err[64], reply[64];
	struct vpt1_error_ctx ectx = {};
	struct vpt1_admit_ctx ctx;
	struct vpt1_snapshot *tight;
	struct fixture f = {};
	enum vpt1_reason why;
	u64 T = ktime_get_boottime_ns(), now = T + SEC;
	unsigned int n, i;
	bool dis;
	int ret;

	if (fixture_make(t, &f, T + 20 * SEC, T))
		goto out;
	/*
	 * Local 10.77.1.10 sent TCP to the peer's 10.77.2.10:8080 under rule 1, as an initiation.
	 */
	tcp_packet(pkt, IPV4(10, 77, 1, 10), IPV4(10, 77, 2, 10), 45000, 8080);
	vpt1_peer_err_save(f.peer, VPT1_DIR_TX, 1, false, pkt, 40, now);
	n = icmp_error(err, IPV4(10, 77, 2, 1), pkt, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH);
	EXPECT(t, n == 64 && !vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now, &ectx));
	EXPECT(t, ectx.rule_id == 1 && ectx.held && ectx.validated && ectx.peer_slot == 1);
	EXPECT(t, ectx.created_ns == now && ectx.flow.protocol == IPPROTO_TCP &&
	       ectx.flow.peer_slot == 1);
	EXPECT(t, ectx.flow.source == IPV4(10, 77, 1, 10) &&
	       ectx.flow.destination == IPV4(10, 77, 2, 10));
	EXPECT(t, ectx.flow.source_port == cpu_to_be16(45000) &&
	       ectx.flow.destination_port == cpu_to_be16(8080));
	EXPECT(t, ectx.flow.direction == VPT1_ACL_LOCAL_TO_PEER);
	/*
	 * The saved rule, against the CURRENT policy, admits it (an error from the peer is
	 * received).
	 */
	rcu_read_lock();
	ret = vpt1_admit_begin(f.peer, &f.inc, VPT1_DIR_RX, f.gen, now, &ctx, &why);
	if (!ret)
		ret = vpt1_admit_error(&ctx, &ectx, &why);
	if (!ret)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	EXPECT(t, !ret);
	if (!ret)
		vpt1_admit_done(f.peer);
	/* Direction, age, quotation, destination and type all matter. */
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_RX, err, n, now, &ectx) == -ENOENT);
	EXPECT(t,
	       !vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now + VPT1_ERROR_RETENTION_NS - 1,
				   &ectx));
	EXPECT(t,
	       vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now + VPT1_ERROR_RETENTION_NS,
				  &ectx) == -ENOENT);
	memcpy(reply, err, n);
	reply[28 + 22] ^= 1; /* A quoted destination port that was never sent. */
	error_fix_csum(reply);
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, reply, n, now, &ectx) == -ENOENT);
	memcpy(reply, err, n);
	reply[28 + 12] ^= 1; /* A quoted source that was never ours. */
	error_fix_csum(reply);
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, reply, n, now, &ectx) == -ENOENT);
	n = icmp_error(reply, IPV4(10, 77, 2, 1), pkt, ICMP_REDIRECT, 0);
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, reply, n, now, &ectx) == -ENOENT);
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, 55, now, &ectx) == -EINVAL);
	/*
	 * A retransmission is the same rule: unique. A second rule for the same bytes is ambiguous.
	 */
	n = 64;
	vpt1_peer_err_save(f.peer, VPT1_DIR_TX, 1, false, pkt, 40, now + 1);
	EXPECT(t, !vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now + 1, &ectx));
	vpt1_peer_err_save(f.peer, VPT1_DIR_TX, 9, false, pkt, 40, now + 2);
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now + 2, &ectx) == -ENOTUNIQ);

	/*
	 * A reply that arrived (RX, reply=true): the error back to the peer carries the reversed
	 * tuple.
	 */
	tcp_packet(pkt, IPV4(10, 77, 2, 10), IPV4(10, 77, 1, 10), 8080, 45000);
	vpt1_peer_err_save(f.peer, VPT1_DIR_RX, 1, true, pkt, 40, now);
	n = icmp_error(err, IPV4(10, 77, 1, 1), pkt, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL);
	EXPECT(t, !vpt1_peer_err_find(f.peer, VPT1_DIR_RX, err, n, now, &ectx));
	EXPECT(t, ectx.flow.source == IPV4(10, 77, 1, 10) &&
	       ectx.flow.destination == IPV4(10, 77, 2, 10));
	EXPECT(t, ectx.flow.source_port == cpu_to_be16(45000) &&
	       ectx.flow.destination_port == cpu_to_be16(8080));
	EXPECT(t, ectx.flow.direction == VPT1_ACL_LOCAL_TO_PEER);

	/*
	 * The TX final cut for an error: current rule, age and size, without a second reservation.
	 */
	rcu_read_lock();
	ret = vpt1_admit_begin(f.peer, &f.inc, VPT1_DIR_TX, f.gen, now, &ctx, &why);
	if (!ret)
		ret = vpt1_admit_error(&ctx, &ectx, &why);
	if (!ret)
		vpt1_admit_handoff(&ctx);
	rcu_read_unlock();
	EXPECT(t, !ret);
	if (!ret) {
		EXPECT(t, !recheck_err(&f, &ectx, n, now + 1, &why));
		EXPECT(t, recheck_err(&f, &ectx, 1429, now + 1, &why) == -EMSGSIZE);
		EXPECT(t,
		       recheck_err(&f, &ectx, n, now + VPT1_ERROR_RETENTION_NS, &why) == -EINVAL);
		ectx.validated = false;
		EXPECT(t, recheck_err(&f, &ectx, n, now + 1, &why) == -EINVAL);
		ectx.validated = true;
		EXPECT(t, recheck_err(&f, NULL, n, now + 1, &why) == -EINVAL);
		/*
		 * Tightening removes the saved rule: the error cannot leave, though it was
		 * admitted.
		 */
		tight = kzalloc(sizeof(*tight), GFP_KERNEL);
		EXPECT(t, tight);
		if (tight) {
			snap_base(tight);
			tight->rule_count = 1;
			tight->rules[0] = tight->rules[1]; /* Only peer 2's rule remains. */
			EXPECT(t, !stage_commit(f.gw, 2, tight, T + 2, &dis) && !dis);
			EXPECT(t, recheck_err(&f, &ectx, n, now + 1, &why) == -EACCES);
			EXPECT(t, why == VPT1_R_ACL_DENY);
			kfree(tight);
		}
		vpt1_admit_done(f.peer);
	}

	/*
	 * The ring holds 256 packets a direction: an older one is overwritten, a newer one found.
	 */
	tcp_packet(pkt, IPV4(10, 77, 1, 10), IPV4(10, 77, 2, 10), 1000, 8080);
	vpt1_peer_err_save(f.peer, VPT1_DIR_TX, 1, false, pkt, 40, now);
	n = icmp_error(err, IPV4(10, 77, 2, 1), pkt, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH);
	EXPECT(t, !vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now, &ectx));
	for (i = 1; i <= VPT1_ERROR_CONTEXTS; i++) {
		tcp_packet(pkt, IPV4(10, 77, 1, 10), IPV4(10, 77, 2, 10), 1000 + i, 8080);
		vpt1_peer_err_save(f.peer, VPT1_DIR_TX, 1, false, pkt, 40, now);
	}
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now, &ectx) == -ENOENT);
	n = icmp_error(err, IPV4(10, 77, 2, 1), pkt, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH);
	EXPECT(t, !vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now, &ectx));
	/* A session that ends takes its contexts with it. */
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_gw_disable_peer(f.gw, 1, &f.inc));
	vpt1_gw_unlock(f.gw);
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now, &ectx) == -ENOENT);
	/* Too short to hold the header and a transport prefix: never saved. */
	vpt1_peer_err_save(f.peer, VPT1_DIR_TX, 1, false, pkt, 27, now);
	vpt1_peer_err_save(f.peer, VPT1_DIR_TX, 0, false, pkt, 40, now);
	EXPECT(t, vpt1_peer_err_find(f.peer, VPT1_DIR_TX, err, n, now, &ectx) == -ENOENT);
out:
	fixture_free(t, &f);
}

static void adapter_counters(struct gw_test *t)
{
	/*
	 * COUNTERS members are enum index + 1: the live adapter's seven follow SIZE (49).
	 */
	static_assert(VPT1_E_FRAG_QUEUED == 50);
	static_assert(VPT1_E_FRAG_COMPLETE == 51);
	static_assert(VPT1_E_FRAG_DROPPED == 52);
	static_assert(VPT1_E_FRAG_BUSY == 53);
	static_assert(VPT1_E_ERROR_ADMITTED == 54);
	static_assert(VPT1_E_ERROR_DENIED == 55);
	static_assert(VPT1_E_TX_GSO_LIMIT == 56);
	static_assert(VPT1_E_COUNT == 57);
	EXPECT(t, VPT1_E_LIVENESS_LOST == 41 && VPT1_R_SIZE == 49);
}

/*
 * A TCP packet takes the reply path only with ACK or RST: a bare SYN from the service port is not a
 * reply.
 */
static int admit_pi(struct fixture *f, enum vpt1_dir dir, const struct vpt1_pkt_info *pi, u64 now,
		    enum vpt1_reason *why)
{
	struct vpt1_admit_ctx ctx;
	int err;

	rcu_read_lock();
	err = vpt1_admit_begin(f->peer, &f->inc, dir, f->gen, now, &ctx, why);
	if (!err)
		err = vpt1_admit_packet(&ctx, pi, why);
	if (!err) {
		vpt1_admit_handoff(&ctx);
		vpt1_admit_done(f->peer);
	}
	rcu_read_unlock();
	return err;
}

static void reply_syn_refused(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_pkt_info fwd = { .src = IPV4(10, 77, 1, 10), .dst = IPV4(10, 77, 2, 10),
				     .sport = cpu_to_be16(45000), .dport = cpu_to_be16(8080),
				     .proto = IPPROTO_TCP };
	struct vpt1_pkt_info back = { .src = IPV4(10, 77, 2, 10), .dst = IPV4(10, 77, 1, 10),
				      .sport = cpu_to_be16(8080), .dport = cpu_to_be16(45000),
				      .proto = IPPROTO_TCP };
	/* SYN-ACK, ACK, PSH-ACK, FIN-ACK, RST, RST-ACK */
	static const u8 allowed[] = { 0x12, 0x10, 0x18, 0x11, 0x04, 0x14 };
	static const u8 refused[] = { 0x02, 0x00, 0x01, 0x08, 0x20 }; /* SYN, none, FIN, PSH, URG */
	struct fixture f = {};
	enum vpt1_reason why;
	unsigned int i;

	if (fixture_make(t, &f, T + 20 * SEC, T))
		goto out;
	/*
	 * The initiation direction is stateless: a SYN, and a midstream segment, are both admitted.
	 */
	fwd.tcp_flags = 0x02;
	EXPECT(t, !admit_pi(&f, VPT1_DIR_TX, &fwd, T + SEC, &why));
	fwd.tcp_flags = 0x10;
	EXPECT(t, !admit_pi(&f, VPT1_DIR_TX, &fwd, T + SEC, &why));
	for (i = 0; i < ARRAY_SIZE(allowed); i++) {
		back.tcp_flags = allowed[i];
		EXPECT(t, !admit_pi(&f, VPT1_DIR_RX, &back, T + SEC, &why));
	}
	for (i = 0; i < ARRAY_SIZE(refused); i++) {
		back.tcp_flags = refused[i];
		EXPECT(t, admit_pi(&f, VPT1_DIR_RX, &back, T + SEC, &why) == -EACCES &&
		       why == VPT1_R_ACL_DENY);
	}
	/* The rule is not the issue: the same flags on a datagram protocol are unaffected. */
	back.tcp_flags = 0x02;
	back.proto = IPPROTO_UDP;
	/* No UDP rule at all. */
	EXPECT(t, admit_pi(&f, VPT1_DIR_RX, &back, T + SEC, &why) == -EACCES);
out:
	fixture_free(t, &f);
}

/*
 * VALIDATE reserves budget before it allocates the ACL snapshot. If that allocation fails the reservation must
 * be given back, nothing may be staged, the gateway must be as before, and the same call must then succeed.
 */
static void acl_allocation_failure(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_cfg_accounting acc;
	struct vpt1_snapshot *s = kzalloc(sizeof(*s), GFP_KERNEL);
	struct vpt1_gw *gw = new_gw(t, T, NULL);
	struct vpt1_status st;
	bool dis;
	int left, err;

	EXPECT(t, s && gw);
	if (!s || !gw)
		goto out;
	snap_base(s);
	vpt1_acl_dbg_fail_nth(1);
	err = stage_commit(gw, 1, s, T, &dis);
	left = vpt1_acl_dbg_fail_nth(0);
	EXPECT(t, left == 0 && err == -ENOMEM);
	get_status(gw, T, &st);
	EXPECT(t, st.revision == 0 && st.peer_count == 0 && !st.has_staged);
	vpt1_gw_lock(gw);
	vpt1_gw_debug_accounting(gw, &acc);
	vpt1_gw_unlock(gw);
	EXPECT(t, acc.charged == 0 && acc.staged_charge == 0);
	EXPECT(t, !stage_commit(gw, 1, s, T, &dis));
	get_status(gw, T, &st);
	EXPECT(t, st.revision == 1 && st.peer_count == 2);
out:
	if (gw)
		end_gw(t, gw);
	kfree(s);
}

/* Kernel-issued attempts: the kernel picks the attempt ID and the receive locator. ------------------ */

static int att_begin(struct fixture *f, unsigned int slot, const struct vpt1_incarnation *inc,
		     const u8 *given, u64 now, u8 id[VPT1_ATTEMPT_ID_LEN], u64 *rx)
{
	int err;

	vpt1_gw_lock(f->gw);
	err = vpt1_gw_attempt_begin(f->gw, slot, inc, given, now, id, rx);
	vpt1_gw_unlock(f->gw);
	return err;
}

static int att_check(struct fixture *f, unsigned int slot, const struct vpt1_incarnation *inc,
		     const u8 *id, u64 rx, u64 now)
{
	int err;

	vpt1_gw_lock(f->gw);
	err = vpt1_gw_attempt_check(f->gw, slot, inc, id, rx, now, true);
	vpt1_gw_unlock(f->gw);
	return err;
}

static void kernel_attempts(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	static const u8 zero[VPT1_ATTEMPT_ID_LEN];
	u8 id1[VPT1_ATTEMPT_ID_LEN], id2[VPT1_ATTEMPT_ID_LEN], idp2[VPT1_ATTEMPT_ID_LEN];
	u8 init[VPT1_ATTEMPT_ID_LEN] = { 0xee, 1, 2, 3 }, other[VPT1_ATTEMPT_ID_LEN] = { 0xdd, 9 };
	struct vpt1_incarnation inc2, stale = { .id = { 0x77 } };
	u64 rx1, rx2, rxp2;
	struct vpt1_peer *p2 = NULL;
	struct fixture f = {};
	unsigned int i;

	if (fixture_make(t, &f, T0 + 20 * SEC, T0))
		goto out;
	p2 = vpt1_gw_peer_get(f.gw, 2);
	EXPECT(t, p2);
	if (!p2)
		goto out;
	vpt1_peer_incarnation(p2, &inc2);

	/* An initiator's attempt: the kernel chooses a nonzero ID and a nonzero, unretained locator. */
	EXPECT(t, !att_begin(&f, 1, &f.inc, NULL, T0, id1, &rx1));
	EXPECT(t, memcmp(id1, zero, sizeof(id1)) && rx1 && rx1 != 0x1111);
	EXPECT(t, !att_check(&f, 1, &f.inc, id1, rx1, T0 + SEC));
	EXPECT(t, !att_check(&f, 1, NULL, id1, 0, T0 + SEC)); /* ID only. */
	EXPECT(t, att_check(&f, 1, &f.inc, other, rx1, T0 + SEC) == -ENOENT);
	EXPECT(t, att_check(&f, 1, &f.inc, id1, rx1 + 1, T0 + SEC) == -EINVAL);
	EXPECT(t, att_check(&f, 1, &stale, id1, rx1, T0 + SEC) == -ESTALE);
	EXPECT(t, att_check(&f, 2, &inc2, id1, rx1, T0 + SEC) == -ENOENT); /* Another peer's attempt. */
	EXPECT(t, att_check(&f, 1, &f.inc, id1, rx1, T0 + VPT1_ATTEMPT_NS - 1) == 0);
	EXPECT(t, att_check(&f, 1, &f.inc, id1, rx1, T0 + VPT1_ATTEMPT_NS) == -ETIMEDOUT);

	/* A begin for the wrong incarnation or an absent slot is refused and issues nothing. */
	EXPECT(t, att_begin(&f, 1, &stale, NULL, T0, id2, &rx2) == -ESTALE);
	EXPECT(t, att_begin(&f, 3, &f.inc, NULL, T0, id2, &rx2) == -ENOENT);
	EXPECT(t, !att_check(&f, 1, &f.inc, id1, rx1, T0 + SEC));

	/* One pending attempt per peer: a later begin replaces it. */
	EXPECT(t, !att_begin(&f, 1, &f.inc, NULL, T0 + 2 * SEC, id2, &rx2));
	EXPECT(t, memcmp(id1, id2, sizeof(id1)) && rx1 != rx2);
	EXPECT(t, att_check(&f, 1, &f.inc, id1, rx1, T0 + 3 * SEC) == -ENOENT);
	EXPECT(t, !att_check(&f, 1, &f.inc, id2, rx2, T0 + 3 * SEC));

	/* Locators are distinct across a gateway's peers, over many draws. */
	EXPECT(t, !att_begin(&f, 2, &inc2, NULL, T0, idp2, &rxp2));
	EXPECT(t, rxp2 != rx2 && rxp2 != 0x1111 && !att_check(&f, 1, &f.inc, id2, rx2, T0 + 3 * SEC));
	for (i = 0; i < 200; i++) {
		EXPECT(t, !att_begin(&f, 2, &inc2, NULL, T0, idp2, &rxp2));
		EXPECT(t, rxp2 != rx2 && rxp2 != 0x1111);
	}

	/* done consumes it, and only for the matching ID. */
	vpt1_gw_lock(f.gw);
	vpt1_gw_attempt_done(f.gw, 1, other, T0 + 3 * SEC);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !att_check(&f, 1, &f.inc, id2, rx2, T0 + 3 * SEC));
	vpt1_gw_lock(f.gw);
	vpt1_gw_attempt_done(f.gw, 1, id2, T0 + 3 * SEC);
	/* Keys are installed once, but the attempt can still send (a responder answers duplicates). */
	EXPECT(t, vpt1_gw_attempt_check(f.gw, 1, &f.inc, id2, rx2, T0 + 4 * SEC, true) == -ENOENT);
	EXPECT(t, !vpt1_gw_attempt_check(f.gw, 1, NULL, id2, 0, T0 + 4 * SEC, false));
	EXPECT(t, !vpt1_gw_attempt_check(f.gw, 1, NULL, id2, 0, T0 + 3 * SEC + VPT1_ATTEMPT_DONE_NS - 1,
					 false));
	EXPECT(t, vpt1_gw_attempt_check(f.gw, 1, NULL, id2, 0, T0 + 3 * SEC + VPT1_ATTEMPT_DONE_NS,
					false) == -ETIMEDOUT);
	vpt1_gw_unlock(f.gw);

	/* A responder may only begin the attempt of an INIT the kernel delivered, still fresh. */
	EXPECT(t, att_begin(&f, 1, &f.inc, init, T0, id1, &rx1) == -ENOENT);
	vpt1_peer_note_init(f.peer, init, T0);
	EXPECT(t, att_begin(&f, 1, &f.inc, other, T0 + SEC, id1, &rx1) == -ENOENT);
	EXPECT(t, !att_begin(&f, 1, &f.inc, init, T0 + SEC, id1, &rx1));
	EXPECT(t, !memcmp(id1, init, sizeof(id1)) && rx1);
	EXPECT(t, !att_check(&f, 1, &f.inc, init, rx1, T0 + 2 * SEC));
	EXPECT(t, att_begin(&f, 1, &f.inc, init, T0 + VPT1_ATTEMPT_NS, id1, &rx1) == -ENOENT);
	/* The delivered INIT belongs to its own peer. */
	EXPECT(t, att_begin(&f, 2, &inc2, init, T0 + SEC, id1, &rx1) == -ENOENT);

	/* A revoked peer is a new incarnation: the old one cannot begin or check. */
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_gw_revoke_peer(f.gw, 1, &f.inc));
	vpt1_gw_unlock(f.gw);
	EXPECT(t, att_begin(&f, 1, &f.inc, NULL, T0, id1, &rx1) == -ESTALE);
out:
	if (p2)
		vpt1_peer_put(p2);
	fixture_free(t, &f);
}

/* Liveness: PING challenges, PONG matching, the 15 s closure and recovery. ----------------------- */

/*
 * The challenge, pong and closure functions take the time as an argument, so this part runs on a
 * made-up clock measured from base (a new session is installed at about base, the lease is long).
 */
static void liveness(struct gw_test *t)
{
	u64 base = ktime_get_boottime_ns();
	u8 c1[VPT1_CHALLENGE_LEN], c2[VPT1_CHALLENGE_LEN], c3[VPT1_CHALLENGE_LEN], c4[VPT1_CHALLENGE_LEN];
	static const u8 wrong[VPT1_CHALLENGE_LEN] = { 9 };
	struct fixture f = {};

	if (fixture_make(t, &f, base + 30 * SEC, base))
		goto out;

	/* The first PING is not due at once, then one every VPT1_PING_NS. */
	EXPECT(t, !vpt1_peer_live_challenge(f.peer, base + SEC / 2, c1));
	EXPECT(t, vpt1_peer_live_challenge(f.peer, base + 2 * SEC, c1));
	EXPECT(t, !vpt1_peer_live_challenge(f.peer, base + 3 * SEC, c2)); /* Not due again yet. */
	EXPECT(t, vpt1_peer_live_challenge(f.peer, base + 8 * SEC, c2));
	EXPECT(t, vpt1_peer_live_challenge(f.peer, base + 14 * SEC, c3));
	EXPECT(t, memcmp(c1, c2, sizeof(c1)) && memcmp(c2, c3, sizeof(c2)) && memcmp(c1, c3, sizeof(c1)));
	EXPECT(t, !vpt1_peer_live_challenge(f.peer, base + 15 * SEC, c4)); /* Three live, and not due. */

	/* Only an echo of a live challenge counts, and only once. */
	EXPECT(t, !vpt1_peer_live_pong(f.peer, wrong, base + 9 * SEC));
	EXPECT(t, vpt1_peer_live_pong(f.peer, c2, base + 9 * SEC));
	EXPECT(t, !vpt1_peer_live_pong(f.peer, c2, base + 10 * SEC)); /* Consumed. */
	/* c1 was issued at 2 s and is answerable until 17 s, not at 17 s. */
	EXPECT(t, !vpt1_peer_live_pong(f.peer, c1, base + 17 * SEC));

	/* The round trip at 9 s moved the deadline to 24 s: open at 20 s, closed at 25 s. */
	EXPECT(t, !vpt1_peer_live_closed(f.peer, base + 20 * SEC));
	EXPECT(t, vpt1_peer_live_closed(f.peer, base + 25 * SEC));
	EXPECT(t, !admit_probe(&f, VPT1_DIR_TX, f.gen, base + 20 * SEC));
	EXPECT(t, admit_probe(&f, VPT1_DIR_TX, f.gen, base + 25 * SEC) == -EACCES);
	EXPECT(t, admit_probe(&f, VPT1_DIR_RX, f.gen, base + 25 * SEC) == -EACCES);
	/* A late answer does not reopen a closed peer, and a closed peer sends no new PING. */
	EXPECT(t, !vpt1_peer_live_pong(f.peer, c3, base + 26 * SEC));
	EXPECT(t, !vpt1_peer_live_closed(f.peer, base + 20 * SEC)); /* The earlier time is still open. */
	EXPECT(t, !vpt1_peer_live_challenge(f.peer, base + 30 * SEC, c4));
out:
	fixture_free(t, &f);
}

/* On the real clock: the worker notices the closure once, and only a new session opens the peer again. */
static void liveness_loss(struct gw_test *t)
{
	u64 base = ktime_get_boottime_ns();
	struct vpt1_generation *g2 = new_gen(0x41, base);
	struct vpt1_continuity *cont_before = NULL, *cont_after = NULL;
	u64 before[VPT1_E_COUNT], after[VPT1_E_COUNT];
	struct fixture f = {};
	u64 id_before = 0;

	EXPECT(t, g2);
	if (!g2 || fixture_make(t, &f, base + 40 * SEC, base))
		goto out;
	cont_before = vpt1_peer_continuity_get(f.peer);
	EXPECT(t, cont_before);
	if (cont_before)
		id_before = vpt1_continuity_id(cont_before);
	vpt1_gw_lock(f.gw);
	vpt1_gw_counters(f.gw, before);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, !vpt1_peer_live_closed(f.peer, ktime_get_boottime_ns()));

	ssleep(17);
	vpt1_gw_lock(f.gw);
	vpt1_gw_counters(f.gw, after);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, after[VPT1_E_LIVENESS_LOST] == before[VPT1_E_LIVENESS_LOST] + 1);
	EXPECT(t, cont_before && vpt1_continuity_dead(cont_before));
	EXPECT(t, vpt1_peer_live_closed(f.peer, ktime_get_boottime_ns()));
	EXPECT(t, admit_probe(&f, VPT1_DIR_TX, f.gen, ktime_get_boottime_ns()) == -EACCES);
	msleep(1500); /* Counted once per closure, not once per tick. */
	vpt1_gw_lock(f.gw);
	vpt1_gw_counters(f.gw, after);
	vpt1_gw_unlock(f.gw);
	EXPECT(t, after[VPT1_E_LIVENESS_LOST] == before[VPT1_E_LIVENESS_LOST] + 1);

	/*
	 * A new generation on a closed peer is a new session even though its keys have not aged out: the
	 * old keys are dropped, there is a new continuity, and the peer is open again.
	 */
	EXPECT(t, !stage_gen(&f, g2, 0x5151, 0x5252));
	cont_after = vpt1_peer_continuity_get(f.peer);
	EXPECT(t, cont_after && vpt1_continuity_id(cont_after) != id_before &&
		   !vpt1_continuity_dead(cont_after));
	EXPECT(t, !vpt1_peer_live_closed(f.peer, ktime_get_boottime_ns()));
	EXPECT(t, tx_is(&f, g2, 0x5252, ktime_get_boottime_ns()));
	EXPECT(t, rx_is(&f, 0x1111, NULL, ktime_get_boottime_ns())); /* The closed session's keys are gone. */
out:
	vpt1_continuity_put(cont_before);
	vpt1_continuity_put(cont_after);
	fixture_free(t, &f);
	if (g2)
		vpt1_generation_put(g2);
}

/* Concurrency: a revoke racing a handoff, and teardown with a held reference. ----- */

struct revoke_job {
	struct vpt1_gw *gw;
	struct vpt1_incarnation inc;
	struct completion done;
	int err;
};

static int revoke_thread(void *arg)
{
	struct revoke_job *job = arg;

	vpt1_gw_lock(job->gw);
	job->err = vpt1_gw_revoke_peer(job->gw, 1, &job->inc);
	vpt1_gw_unlock(job->gw);
	complete(&job->done);
	return 0;
}

static void revoke_races_handoff(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_witness w = { .rule_id = 1, .valid = true,
				  .flow = flow_for(1, 10) };
	struct vpt1_admit_ctx ctx;
	struct revoke_job job = {};
	struct vpt1_peer *fresh;
	struct vpt1_incarnation inc2;
	enum vpt1_reason why;
	struct fixture f;
	long peer0 = vpt1_peer_live();

	if (fixture_make(t, &f, T, T))
		goto out;
	job.gw = f.gw;
	job.inc = f.inc;
	init_completion(&job.done);

	rcu_read_lock();
	EXPECT(t, !vpt1_admit_begin(f.peer, &f.inc, VPT1_DIR_TX, f.gen, T + 1, &ctx,
				    &why));
	EXPECT(t, !vpt1_admit_existing(&ctx, &w, &why));
	vpt1_admit_handoff(&ctx); /* Authorized and now in flight. */
	rcu_read_unlock();

	/* The revoke runs concurrently; it must not finish while a packet it
	 * authorized is inside the handoff, and no admission may succeed after it
	 * has swapped the peer.
	 */
	kthread_run(revoke_thread, &job, "vpt1_revoke_test");
	msleep(150);
	EXPECT(t, !completion_done(&job.done));
	vpt1_admit_done(f.peer);
	EXPECT(t, wait_for_completion_timeout(&job.done, 5 * HZ) > 0 && !job.err);

	/* The old peer object is closed; a fresh one carries a new incarnation. */
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 2, &w, &why, true) == -EACCES &&
		   why == VPT1_R_PEER_INELIGIBLE);
	fresh = vpt1_gw_peer_get(f.gw, 1);
	EXPECT(t, fresh);
	if (fresh) {
		vpt1_peer_incarnation(fresh, &inc2);
		EXPECT(t, memcmp(inc2.id, f.inc.id, VPT1_ID_LEN));
		vpt1_peer_put(fresh);
	}
out:
	fixture_free(t, &f);
	rcu_barrier();
	EXPECT(t, vpt1_peer_live() == peer0);
}

struct destroy_job {
	struct vpt1_gw *gw;
	struct completion done;
};

static int destroy_thread(void *arg)
{
	struct destroy_job *job = arg;

	vpt1_gw_destroy(job->gw, 0);
	complete(&job->done);
	return 0;
}

static void teardown_waits_for_handlers(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_witness w = { .rule_id = 1, .valid = true,
				  .flow = flow_for(1, 10) };
	struct destroy_job job = {};
	enum vpt1_reason why;
	struct fixture f;
	long gw0 = vpt1_gw_live(), cfg0 = vpt1_cfg_live(), peer0 = vpt1_peer_live();

	if (fixture_make(t, &f, T, T))
		goto out;
	job.gw = f.gw;
	init_completion(&job.done);
	/* A handler reference is held: destroy closes admission, then waits. */
	EXPECT(t, vpt1_gw_get(f.gw));
	kthread_run(destroy_thread, &job, "vpt1_destroy_test");
	msleep(150);
	EXPECT(t, !completion_done(&job.done));
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true) == -ESHUTDOWN &&
		   why == VPT1_R_STOPPING);
	EXPECT(t, !vpt1_gw_get(f.gw)); /* New handlers are refused while stopping. */
	vpt1_gw_put(f.gw);
	EXPECT(t, wait_for_completion_timeout(&job.done, 5 * HZ) > 0);
	f.gw = NULL; /* Destroyed; memory lives until the last peer reference. */
out:
	fixture_free(t, &f);
	rcu_barrier();
	rcu_barrier();
	EXPECT(t, vpt1_gw_live() == gw0 && vpt1_cfg_live() == cfg0 &&
		   vpt1_peer_live() == peer0);
}

static void close_all_stops_admission(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_witness w = { .rule_id = 1, .valid = true,
				  .flow = flow_for(1, 10) };
	struct fixture a, b;
	enum vpt1_reason why;

	if (fixture_make(t, &a, T, T) || fixture_make(t, &b, T, T))
		goto out;
	EXPECT(t, !try_existing(&a, &a.inc, a.gen, T + 1, &w, &why, true));
	vpt1_gw_close_all();
	EXPECT(t, try_existing(&a, &a.inc, a.gen, T + 1, &w, &why, true) == -ESHUTDOWN);
	EXPECT(t, try_existing(&b, &b.inc, b.gen, T + 1, &w, &why, true) == -ESHUTDOWN);
out:
	fixture_free(t, &a);
	fixture_free(t, &b);
}

/* Owner loss with delayed cleanup and a reused portid. -------------------------- */

static void loss_with_reused_portid(struct gw_test *t)
{
	u64 T = ktime_get_boottime_ns();
	struct vpt1_witness w = { .rule_id = 1, .valid = true,
				  .flow = flow_for(1, 10) };
	struct vpt1_status st;
	enum vpt1_reason why;
	struct fixture f;
	bool dis;
	u64 e;

	if (fixture_make(t, &f, T, T))
		goto out;
	EXPECT(t, !try_existing(&f, &f.inc, f.gen, T + 1, &w, &why, true));
	atomic_set(&vpt1_dbg_stall_expiry, 1); /* Hold the cleanup work back. */
	EXPECT(t, vpt1_gw_owner_released(f.gw, 100));
	/* Admission is closed immediately, even though no cleanup has run. */
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 2, &w, &why, true) == -EPERM &&
		   why == VPT1_R_NO_OWNER);
	/* A replacement owner reuses the same portid before the stalled work runs. */
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_gw_claim(f.gw, ID_B, 100, T + 3, &e) && e == 2);
	EXPECT(t, !vpt1_gw_validate(f.gw, 1, f.snap));
	EXPECT(t, !vpt1_gw_commit(f.gw, 1, T + 3, &dis));
	vpt1_gw_unlock(f.gw);
	atomic_set(&vpt1_dbg_stall_expiry, 0);
	vpt1_gw_flush();
	/* The old-owner work did not close the new owner's config or peers. */
	get_status(f.gw, T + 4, &st);
	EXPECT(t, st.owned && st.owner_epoch == 2 && st.revision == 1 &&
		   st.peer_count == 2);
	/* The old peer reference stays dead. */
	EXPECT(t, try_existing(&f, &f.inc, f.gen, T + 4, &w, &why, true) == -EPERM);
out:
	atomic_set(&vpt1_dbg_stall_expiry, 0);
	fixture_free(t, &f);
}

static void control_final_cut(struct gw_test *t)
{
	u64 now = ktime_get_boottime_ns(), counter = U64_MAX;
	struct vpt1_generation *next = NULL;
	struct vpt1_admit_ctx ctx;
	struct fixture f = {};
	enum vpt1_reason why;
	bool held = false;
	int err;

	if (fixture_make(t, &f, now + 200 * SEC, now))
		goto out;
	rcu_read_lock();
	err = vpt1_admit_control_recheck(f.peer, &f.inc, f.gen, now, &why);
	EXPECT(t, err == -EINVAL && why == VPT1_R_BAD_CONTEXT);
	err = vpt1_admit_control_begin(f.peer, &f.inc, f.gen, now, &ctx, &why);
	EXPECT(t, !err);
	if (!err) {
		vpt1_admit_handoff(&ctx);
		held = true;
		EXPECT(t, !vpt1_generation_tx_reserve(f.gen, 20, now, &counter) && !counter);
		EXPECT(t, !vpt1_admit_control_recheck(f.peer, &f.inc, f.gen, now, &why));
		EXPECT(t, !vpt1_admit_control_recheck(f.peer, &f.inc, f.gen, now, &why));
		vpt1_admit_done(f.peer);
		held = false;
		/* A final cut never minted another processing reservation. */
		EXPECT(t, vpt1_admit_control_recheck(f.peer, &f.inc, f.gen, now, &why) == -EINVAL);
	}
	err = vpt1_admit_control_begin(f.peer, &f.inc, f.gen, now, &ctx, &why);
	EXPECT(t, !err);
	if (!err) {
		vpt1_admit_handoff(&ctx);
		held = true;
		EXPECT(t, !vpt1_generation_tx_reserve(f.gen, 20, now, &counter) && counter == 1);
		/* The session was installed a moment after @now, so closure needs a margin. */
		err = vpt1_admit_control_recheck(f.peer, &f.inc, f.gen,
						 now + VPT1_LIVE_NS + SEC, &why);
		EXPECT(t, err == -EACCES);
		err = vpt1_admit_control_recheck(f.peer, &f.inc, f.gen,
						 now + VPT1_HARD_AGE_NS, &why);
		EXPECT(t, err == -EKEYEXPIRED);
		err = vpt1_admit_control_recheck(f.peer, &f.inc, f.gen, now + 205 * SEC, &why);
		EXPECT(t, err == -ESTALE);
	}
	rcu_read_unlock();
	if (!held)
		goto out;
	next = new_gen(0x71, now);
	EXPECT(t, next);
	if (next) {
		EXPECT(t, !stage_gen(&f, next, 0x9871, 0x9872));
		vpt1_peer_commit(f.peer, now + 1);
		rcu_read_lock();
		err = vpt1_admit_control_recheck(f.peer, &f.inc, f.gen, now + 1, &why);
		EXPECT(t, err == -ENOKEY);
		rcu_read_unlock();
	}
	vpt1_gw_lock(f.gw);
	EXPECT(t, !vpt1_gw_disable_peer(f.gw, 1, &f.inc));
	vpt1_gw_unlock(f.gw);
	rcu_read_lock();
	EXPECT(t, vpt1_admit_control_recheck(f.peer, &f.inc, f.gen, now + 1, &why) == -EACCES);
	rcu_read_unlock();
out:
	if (held)
		vpt1_admit_done(f.peer);
	if (next)
		vpt1_generation_put(next);
	fixture_free(t, &f);
}

/* Activation and granted rekey ---------------------------------------------------------------- */


static int stage_raw(struct fixture *f, struct vpt1_generation *g, u64 local, u64 remote, const u8 *hash,
		     u8 mode, const u8 *att, const u8 *parent)
{
	int err;

	vpt1_generation_get(g);
	vpt1_gw_lock(f->gw);
	err = vpt1_gw_stage_generation(f->gw, 1, &f->inc, g, local, remote, hash, mode, att, parent);
	vpt1_gw_unlock(f->gw);
	if (err)
		vpt1_generation_put(g);
	return err;
}

/* Feed one authenticated control record to the peer as if it arrived under @g. */
static int ctl(struct fixture *f, struct vpt1_generation *g, u8 subtype, const u8 *payload,
	       unsigned int len, struct vpt1_ctl_out *out)
{
	return vpt1_peer_control_rx(f->peer, g, subtype, payload, len, ktime_get_boottime_ns(), out);
}

static void act_responder(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	u8 H[VPT1_HASH_LEN], H2[VPT1_HASH_LEN], A1[VPT1_ATTEMPT_ID_LEN], A2[VPT1_ATTEMPT_ID_LEN];
	u8 rk[VPT1_REKEY_PAYLOAD_LEN], rk2[VPT1_REKEY_PAYLOAD_LEN], bad[VPT1_REKEY_PAYLOAD_LEN];
	struct vpt1_generation *g2 = new_gen(0x31, T0), *g3 = new_gen(0x41, T0);
	struct vpt1_continuity *c0 = NULL;
	struct vpt1_ctl_out out;
	struct fixture f = {};
	u64 id0 = 0;

	memset(H, 0x11, sizeof(H));
	memset(H2, 0x22, sizeof(H2));
	memset(A1, 0xa1, sizeof(A1));
	memset(A2, 0xa2, sizeof(A2));
	EXPECT(t, g2 && g3);
	test_peer_role = 0;
	if (!g2 || !g3 || fixture_make(t, &f, T0 + 60 * SEC, T0))
		goto out;
	c0 = vpt1_peer_continuity_get(f.peer);
	id0 = c0 ? vpt1_continuity_id(c0) : 0;
	EXPECT(t, id0);

	/* A fresh candidate carries nothing until committed; the old session still sends. */
	EXPECT(t, !stage_raw(&f, g2, 0x3333, 0x4444, H, 0, A1, NULL));
	EXPECT(t, tx_is(&f, f.gen, 0x2222, T0 + SEC));
	EXPECT(t, vpt1_peer_gen_is_candidate(f.peer, g2) && !vpt1_peer_gen_is_candidate(f.peer, f.gen));
	EXPECT(t, rx_is(&f, 0x3333, g2, T0 + SEC));
	/* Identical re-installation is recognised; a different one for the same attempt is not. */
	vpt1_gw_lock(f.gw);
	EXPECT(t, vpt1_gw_stage_duplicate(f.gw, 1, &f.inc, A1, 0x3333, 0x4444, H) == 1);
	EXPECT(t, vpt1_gw_stage_duplicate(f.gw, 1, &f.inc, A1, 0x3333, 0x4445, H) == -EEXIST);
	EXPECT(t, vpt1_gw_stage_duplicate(f.gw, 1, &f.inc, A1, 0x3333, 0x4444, H2) == -EEXIST);
	EXPECT(t, vpt1_gw_stage_duplicate(f.gw, 1, &f.inc, A2, 0x3333, 0x4444, H) == 0);
	vpt1_gw_unlock(f.gw);

	/* CONFIRM answers READY under the candidate, for the right hash only. */
	EXPECT(t, ctl(&f, g2, VPT1_CONFIRM, H2, VPT1_HASH_LEN, &out) == -EPROTO);
	EXPECT(t, ctl(&f, g2, VPT1_CONFIRM, H, VPT1_HASH_LEN - 1, &out) == -EPROTO);
	EXPECT(t, ctl(&f, f.gen, VPT1_CONFIRM, H, VPT1_HASH_LEN, &out) == -EPROTO); /* Wrong generation. */
	EXPECT(t, !ctl(&f, g2, VPT1_CONFIRM, H, VPT1_HASH_LEN, &out));
	EXPECT(t, out.subtype == VPT1_READY && out.which == VPT1_SEND_CANDIDATE &&
		  !memcmp(out.payload, H, VPT1_HASH_LEN));
	EXPECT(t, tx_is(&f, f.gen, 0x2222, T0 + SEC)); /* CONFIRM alone commits nothing. */
	/* READY and COMMITTED belong to the initiator; a responder ignores them. */
	EXPECT(t, ctl(&f, g2, VPT1_READY, H, VPT1_HASH_LEN, &out) == -EPROTO);
	EXPECT(t, ctl(&f, g2, VPT1_COMMITTED, H, VPT1_HASH_LEN, &out) == -EPROTO);

	/* COMMIT commits: the candidate is current, the old keys and continuity are gone. */
	EXPECT(t, ctl(&f, g2, VPT1_COMMIT, H2, VPT1_HASH_LEN, &out) == -EPROTO);
	EXPECT(t, !ctl(&f, g2, VPT1_COMMIT, H, VPT1_HASH_LEN, &out));
	EXPECT(t, out.subtype == VPT1_COMMITTED && out.which == VPT1_SEND_CURRENT);
	EXPECT(t, tx_is(&f, g2, 0x4444, T0 + SEC));
	EXPECT(t, !vpt1_peer_gen_is_candidate(f.peer, g2));
	EXPECT(t, rx_is(&f, 0x1111, NULL, T0 + SEC));
	EXPECT(t, c0 && vpt1_continuity_dead(c0) && cont_id_of(f.peer) != id0);
	/* Repeats are answered from the now current generation and change nothing. */
	EXPECT(t, !ctl(&f, g2, VPT1_COMMIT, H, VPT1_HASH_LEN, &out) && out.subtype == VPT1_COMMITTED);
	EXPECT(t, !ctl(&f, g2, VPT1_CONFIRM, H, VPT1_HASH_LEN, &out) && out.subtype == VPT1_READY &&
		  out.which == VPT1_SEND_CURRENT);
	EXPECT(t, tx_is(&f, g2, 0x4444, T0 + SEC));

	/* A continuity grant: bound to the current hash, one at a time, never extended by a repeat. */
	memcpy(rk, A1, VPT1_ATTEMPT_ID_LEN);
	memcpy(rk + VPT1_ATTEMPT_ID_LEN, H, VPT1_HASH_LEN);
	memcpy(rk2, A2, VPT1_ATTEMPT_ID_LEN);
	memcpy(rk2 + VPT1_ATTEMPT_ID_LEN, H, VPT1_HASH_LEN);
	memcpy(bad, A2, VPT1_ATTEMPT_ID_LEN);
	memcpy(bad + VPT1_ATTEMPT_ID_LEN, H2, VPT1_HASH_LEN);
	EXPECT(t, ctl(&f, g2, VPT1_REKEY_START, bad, sizeof(bad), &out) == -EPROTO); /* Wrong parent. */
	EXPECT(t, ctl(&f, g2, VPT1_REKEY_START, rk, sizeof(rk) - 1, &out) == -EPROTO);
	EXPECT(t, !ctl(&f, g2, VPT1_REKEY_START, rk, sizeof(rk), &out));
	EXPECT(t, out.subtype == VPT1_REKEY_OK && out.which == VPT1_SEND_CURRENT &&
		  !memcmp(out.payload, rk, sizeof(rk)));
	EXPECT(t, !ctl(&f, g2, VPT1_REKEY_START, rk, sizeof(rk), &out) && out.subtype == VPT1_REKEY_OK);
	EXPECT(t, ctl(&f, g2, VPT1_REKEY_START, rk2, sizeof(rk2), &out) == -EPROTO); /* One live grant. */

	/* Mode 1 needs exactly that grant: the attempt, and the parent. */
	EXPECT(t, stage_raw(&f, g3, 0x5555, 0x6666, H2, 1, A2, H) == -EPERM);
	EXPECT(t, stage_raw(&f, g3, 0x5555, 0x6666, H2, 1, A1, H2) == -EPERM);
	EXPECT(t, stage_raw(&f, g3, 0x5555, 0x6666, H2, 1, A1, NULL) == -EPERM);
	EXPECT(t, !stage_raw(&f, g3, 0x5555, 0x6666, H2, 1, A1, H));
	/* The grant is consumed: it cannot make a second candidate. */
	EXPECT(t, stage_raw(&f, g3, 0x7777, 0x8888, H2, 1, A1, H) == -EPERM);
	/* A rekey keeps the continuity and retires the old current into prev. */
	{
		u64 id1 = cont_id_of(f.peer);

		EXPECT(t, tx_is(&f, g2, 0x4444, T0 + SEC));
		EXPECT(t, !ctl(&f, g3, VPT1_COMMIT, H2, VPT1_HASH_LEN, &out));
		EXPECT(t, tx_is(&f, g3, 0x6666, T0 + SEC));
		EXPECT(t, cont_id_of(f.peer) == id1);
		EXPECT(t, rx_is(&f, 0x3333, g2, T0 + SEC)); /* prev */
	}
out:
	vpt1_continuity_put(c0);
	fixture_free(t, &f);
	if (g2)
		vpt1_generation_put(g2);
	if (g3)
		vpt1_generation_put(g3);
}

static void act_initiator(struct gw_test *t)
{
	u64 T0 = ktime_get_boottime_ns();
	u8 H[VPT1_HASH_LEN], H2[VPT1_HASH_LEN], att[VPT1_ATTEMPT_ID_LEN], other[VPT1_ATTEMPT_ID_LEN];
	u8 ok[VPT1_REKEY_PAYLOAD_LEN], wrong[VPT1_REKEY_PAYLOAD_LEN], mism[VPT1_REKEY_PAYLOAD_LEN];
	struct vpt1_generation *g2 = new_gen(0x31, T0), *g3 = new_gen(0x41, T0);
	struct vpt1_incarnation inc;
	struct vpt1_ctl_out out;
	struct fixture f = {};
	u64 rx = 0;

	memset(H, 0x11, sizeof(H));
	memset(H2, 0x22, sizeof(H2));
	memset(other, 0xee, sizeof(other));
	EXPECT(t, g2 && g3);
	test_peer_role = 1;
	if (!g2 || !g3 || fixture_make(t, &f, T0 + 60 * SEC, T0))
		goto out;
	inc = f.inc;

	/* The initiator commits on READY, then waits to be told COMMITTED. */
	EXPECT(t, !stage_raw(&f, g2, 0x3333, 0x4444, H, 0, other, NULL));
	EXPECT(t, ctl(&f, g2, VPT1_READY, H2, VPT1_HASH_LEN, &out) == -EPROTO);
	EXPECT(t, ctl(&f, g2, VPT1_CONFIRM, H, VPT1_HASH_LEN, &out) == -EPROTO); /* A responder's. */
	EXPECT(t, ctl(&f, g2, VPT1_COMMIT, H, VPT1_HASH_LEN, &out) == -EPROTO);
	EXPECT(t, tx_is(&f, f.gen, 0x2222, T0 + SEC));
	EXPECT(t, !ctl(&f, g2, VPT1_READY, H, VPT1_HASH_LEN, &out));
	EXPECT(t, out.subtype == VPT1_COMMIT && out.which == VPT1_SEND_CURRENT);
	EXPECT(t, tx_is(&f, g2, 0x4444, T0 + SEC));
	EXPECT(t, !ctl(&f, g2, VPT1_READY, H, VPT1_HASH_LEN, &out) && out.subtype == VPT1_COMMIT); /* Repeat. */
	EXPECT(t, ctl(&f, g2, VPT1_COMMITTED, H2, VPT1_HASH_LEN, &out) == -EPROTO);
	EXPECT(t, !ctl(&f, g2, VPT1_COMMITTED, H, VPT1_HASH_LEN, &out) && !out.subtype);
	EXPECT(t, !ctl(&f, g2, VPT1_COMMITTED, H, VPT1_HASH_LEN, &out)); /* Idempotent. */

	/* A re-key: the request needs this peer's own pending attempt. */
	vpt1_gw_lock(f.gw);
	EXPECT(t, vpt1_gw_rekey_request(f.gw, 1, &inc, other, T0) == -ENOENT);
	EXPECT(t, !vpt1_gw_attempt_begin(f.gw, 1, &inc, NULL, T0, att, &rx));
	EXPECT(t, !vpt1_gw_rekey_request(f.gw, 1, &inc, att, T0));
	vpt1_gw_unlock(f.gw);
	memcpy(ok, att, VPT1_ATTEMPT_ID_LEN);
	memcpy(ok + VPT1_ATTEMPT_ID_LEN, H, VPT1_HASH_LEN);
	memcpy(wrong, other, VPT1_ATTEMPT_ID_LEN);
	memcpy(wrong + VPT1_ATTEMPT_ID_LEN, H, VPT1_HASH_LEN);
	memcpy(mism, att, VPT1_ATTEMPT_ID_LEN);
	memcpy(mism + VPT1_ATTEMPT_ID_LEN, H2, VPT1_HASH_LEN);
	EXPECT(t, ctl(&f, g2, VPT1_REKEY_OK, wrong, sizeof(wrong), &out) == -EPROTO);
	EXPECT(t, ctl(&f, g2, VPT1_REKEY_OK, mism, sizeof(mism), &out) == -EPROTO);
	EXPECT(t, stage_raw(&f, g3, rx, 0x6666, H2, 1, att, H) == -EPERM); /* Not granted yet. */
	EXPECT(t, !ctl(&f, g2, VPT1_REKEY_OK, ok, sizeof(ok), &out));
	EXPECT(t, out.event == VPT1_EVT_REKEY_GRANTED && !memcmp(out.attempt, att, sizeof(att)));
	EXPECT(t, !ctl(&f, g2, VPT1_REKEY_OK, ok, sizeof(ok), &out) && out.event == VPT1_EVT_NONE);
	EXPECT(t, !stage_raw(&f, g3, rx, 0x6666, H2, 1, att, H));
	/* A responder asks for a re-key; the first is passed on, a repeat inside 5 s is not. */
	EXPECT(t, vpt1_peer_commit(f.peer, T0 + SEC));
	EXPECT(t, ctl(&f, g3, VPT1_REKEY_NEEDED, H, VPT1_HASH_LEN, &out) == -EPROTO); /* Not its parent. */
	EXPECT(t, !ctl(&f, g3, VPT1_REKEY_NEEDED, H2, VPT1_HASH_LEN, &out));
	EXPECT(t, out.event == VPT1_EVT_REKEY_NEEDED);
	EXPECT(t, !ctl(&f, g3, VPT1_REKEY_NEEDED, H2, VPT1_HASH_LEN, &out) && out.event == VPT1_EVT_NONE);
out:
	test_peer_role = 0;
	fixture_free(t, &f);
	if (g2)
		vpt1_generation_put(g2);
	if (g3)
		vpt1_generation_put(g3);
}

static int __init vpt1_gateway_test_init(void)
{
	struct gw_test t = {};
	long gw0, peer0, cfg0;
	int err;

	err = vpt1_generation_init();
	if (err)
		return err;
	err = vpt1_gw_init();
	if (err) {
		vpt1_generation_exit();
		return err;
	}
	gw0 = vpt1_gw_live();
	peer0 = vpt1_peer_live();
	cfg0 = vpt1_cfg_live();

	lease_rules(&t);
	config_delta(&t);
	admission_paths(&t);
	associated_error_deadline(&t);
	size_admission(&t);
	runtime_budget(&t);
	mtu_transaction(&t);
	global_binding_failure(&t);
	policy_tightening(&t);
	generation_age(&t);
	rekey_overlap(&t);
	alloc_failures(&t);
	acl_allocation_failure(&t);
	continuity_lifecycle(&t);
	kernel_attempts(&t);
	liveness(&t);
	liveness_loss(&t);
	control_final_cut(&t);
	act_responder(&t);
	act_initiator(&t);
	fragment_budget(&t);
	job_queue(&t);
	job_teardown(&t);
	fragment_integration(&t);
	adapter_helpers(&t);
	adapter_frag_cache(&t);
	adapter_errors(&t);
	adapter_counters(&t);
	reply_syn_refused(&t);
	revoke_races_handoff(&t);
	teardown_waits_for_handlers(&t);
	close_all_stops_admission(&t);
	loss_with_reused_portid(&t);

	vpt1_gw_exit();
	vpt1_generation_drain();
	EXPECT(&t, vpt1_gw_live() == gw0 && vpt1_peer_live() == peer0 &&
		   vpt1_cfg_live() == cfg0);
	EXPECT(&t, vpt1_generation_live() == 0);
	vpt1_generation_exit();
	pr_info("VPT1_GATEWAY_TEST: assertions=%u failures=%u %s\n", t.assertions,
		t.failures, t.failures ? "FAIL" : "PASS");
	return t.failures ? -EINVAL : 0;
}

static void __exit vpt1_gateway_test_exit(void)
{
}

module_init(vpt1_gateway_test_init);
module_exit(vpt1_gateway_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VPT1 AY2 gateway lease, configuration, admission and teardown tests");
