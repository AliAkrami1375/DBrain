#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "dbrain/node.h"
#include "dbrain/frame.h"
#include "dbrain/bundle.h"
#include "dbrain/crc32.h"
#include "dbrain/version.h"

/* ---- small helpers ---------------------------------------------------- */

static uint32_t now_ms_of(uint64_t us)
{
    return (uint32_t)(us / 1000u);
}

static uint64_t now_us(node_t *n)
{
    return n->pf.now_us(n->pf.ctx);
}

static void nlog(node_t *n, int level, const char *fmt, ...)
{
    char buf[96];
    va_list ap;

    if (!n->pf.log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    n->pf.log(n->pf.ctx, level, buf);
}

static void emit(node_t *n, uint8_t type, uint16_t node_id, uint16_t shard_id, uint32_t tick,
                 int32_t value)
{
    node_event_t ev;

    if (!n->pf.event) return;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.node_id = node_id;
    ev.shard_id = shard_id;
    ev.tick = tick;
    ev.value = value;
    n->pf.event(n->pf.ctx, &ev);
}

static uint32_t rnd(node_t *n, uint32_t span)
{
    return span ? n->pf.random(n->pf.ctx) % span : 0;
}

static void send_frame(node_t *n, const uint8_t *mac, const uint8_t *frame, size_t len, uint8_t tag)
{
    n->pf.send(n->pf.ctx, mac, frame, len, tag);
}

const char *node_state_name(uint8_t state)
{
    switch (state) {
    case NODE_STATE_BOOT:        return "boot";
    case NODE_STATE_DISCOVER:    return "discover";
    case NODE_STATE_JOINING:     return "joining";
    case NODE_STATE_MEMBER:      return "member";
    case NODE_STATE_COORDINATOR: return "coordinator";
    default:                     return "?";
    }
}

static void set_state(node_t *n, uint8_t s)
{
    if (n->state == s) return;
    nlog(n, NODE_LOG_INFO, "state %s -> %s", node_state_name(n->state), node_state_name(s));
    n->state = s;
    emit(n, NODE_EV_STATE, n->node_id, 0, 0, s);
}

static uint32_t own_score(const node_t *n)
{
    return cap_score(&n->cap);
}

static bool eligible(const node_t *n)
{
    return (n->cap.flags & CAP_FLAG_ELIGIBLE) != 0;
}

/* ---- configuration ---------------------------------------------------- */

void node_config_default(node_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->cluster_id = 1;
    c->mode = NODE_MODE_RECIPE;
    c->phy_rate = PHY_RATE_1M;
    c->slot_events = SLOT_EVENTS_DEFAULT;
    c->min_period_us = TICK_PERIOD_MIN_US;
    c->tx_latency_us = 0;
    c->discover_ms = NODE_DISCOVER_MS_DEFAULT;
    recipe_default(&c->recipe);
}

/* ---- control lane glue ------------------------------------------------ */

static int ctrl_send_cb(void *ctx, const uint8_t mac[6], const uint8_t *frame, size_t len)
{
    node_t *n = (node_t *)ctx;
    send_frame(n, mac, frame, len, FRAME_CONTROL);
    return 0;
}

static void ctrl_fail_cb(void *ctx, const uint8_t mac[6], uint8_t type, uint8_t seq)
{
    node_t *n = (node_t *)ctx;
    (void)seq;
    n->tm.control_failures++;
    nlog(n, NODE_LOG_WARN, "control %u to %02x%02x gave up", type, mac[4], mac[5]);
    if (type == CTRL_JOIN_REQ && n->state == NODE_STATE_JOINING) {
        n->have_coord = false;
        set_state(n, NODE_STATE_DISCOVER);
        n->discover_until_ms = now_ms_of(now_us(n)) + n->cfg.discover_ms;
    }
}

static void ctrl_acked_cb(void *ctx, const uint8_t mac[6], uint8_t type, uint8_t seq)
{
    (void)ctx; (void)mac; (void)type; (void)seq;
}

static uint16_t recipe_neurons_capped(const node_t *n);

/* ---- heartbeat -------------------------------------------------------- */

static void fill_heartbeat(node_t *n, heartbeat_t *h)
{
    memset(h, 0, sizeof(*h));
    h->cluster_id = n->cfg.cluster_id;
    h->node_id = n->node_id;
    memcpy(h->mac, n->mac, 6);
    h->epoch = n->epoch;
    h->coord_id = n->have_coord ? n->coord_id : NODE_ID_NONE;
    h->shard_id = n->shard_valid ? n->shard.art.shard_id : SHARD_NONE;
    h->shard_version = n->shard_version;
    h->n_neurons = n->shard_valid ? n->shard.art.n_neurons : n->recipe_neurons;
    h->cap = n->cap;
    h->state = n->state;
    h->flags = 0;
    if (n->state == NODE_STATE_COORDINATOR) h->flags |= HB_FLAG_COORDINATOR;
    if (n->shard_valid) h->flags |= HB_FLAG_HAS_SHARD;
    if (n->cfg.mode == NODE_MODE_RECIPE && n->recipe_valid) h->flags |= HB_FLAG_RECIPE;
    h->hb_seq = n->hb_seq;
    h->tick = n->shard_valid ? n->shard.tick : 0;
    h->drops = (uint16_t)(n->tm.seq_gaps > 65535u ? 65535u : n->tm.seq_gaps);
    h->overruns = (uint16_t)(n->tm.tick_overruns > 65535u ? 65535u : n->tm.tick_overruns);
    h->sync_residual_us = (int16_t)(n->tm.sync_residual_us > 32767 ? 32767 :
                                    n->tm.sync_residual_us < -32768 ? -32768 : n->tm.sync_residual_us);
}

static void update_self_entry(node_t *n, uint32_t ms)
{
    heartbeat_t h;
    fill_heartbeat(n, &h);
    membership_update(&n->members, &h, ms);
}

static void send_heartbeat(node_t *n, uint32_t ms)
{
    heartbeat_t h;
    size_t len;

    n->pf.get_cap(n->pf.ctx, &n->cap);
    n->hb_seq++;
    fill_heartbeat(n, &h);
    len = heartbeat_encode(n->txbuf, sizeof n->txbuf, &h);
    send_frame(n, NULL, n->txbuf, len, FRAME_HEARTBEAT);
    update_self_entry(n, ms);
    n->next_hb_ms = ms + HB_INTERVAL_MS - HB_JITTER_MS + rnd(n, 2u * HB_JITTER_MS + 1u);
}

/* ---- coordinator: plan and beacon ------------------------------------- */

/* Period and slots from the roster; preserves the running tick index. */
static void rebuild_plan(node_t *n)
{
    slot_plan_t plan;
    unsigned i;
    uint16_t guard = SLOT_GUARD_US;
    uint8_t rate = n->cfg.phy_rate;
    uint16_t members = membership_active(&n->members);
    uint64_t local = now_us(n);
    uint32_t tick_now = 0;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        const member_t *m = &n->members.m[i];
        if (m->liveness != MEMBER_ALIVE && m->liveness != MEMBER_SUSPECT) continue;
        if (!(m->cap.flags & CAP_FLAG_HW_RX_TS)) guard = SLOT_GUARD_SOFT_US;
        if (m->cap.chip == DBRAIN_CHIP_ESP8266) rate = PHY_RATE_1M;
    }
    if (!(n->cap.flags & CAP_FLAG_HW_RX_TS)) guard = SLOT_GUARD_SOFT_US;
    slot_plan_build(&plan, members, rate, n->cfg.slot_events, guard, n->cfg.min_period_us);

    if (n->cluster_valid) tick_now = tick_of(local, n->tick_origin_us, n->tick_period_us);
    if (!n->cluster_valid || plan.tick_period_us != n->tick_period_us) {
        /* the current tick keeps its number and starts afresh now */
        n->tick_origin_us = local - (uint64_t)tick_now * plan.tick_period_us;
        n->tick_period_us = plan.tick_period_us;
    }
    n->slot_len_us = plan.slot_len_us;
    n->slot_events = plan.slot_events;
    n->phy_rate = rate;
    n->cluster_members = members;
    n->slot_version++;
    n->cluster_valid = true;
    n->roster_version_planned = membership_plan_signature(&n->members);
    nlog(n, NODE_LOG_INFO, "plan: %u members, tick %lu us, slot %u us",
         (unsigned)members, (unsigned long)n->tick_period_us, (unsigned)n->slot_len_us);
}

static void send_beacon(node_t *n, uint32_t ms)
{
    beacon_t b;
    size_t len;
    uint64_t local = now_us(n);

    if (n->beacon_inflight) return;   /* one beacon on the air at a time; the timer stays due */
    memset(&b, 0, sizeof b);
    b.cluster_id = n->cfg.cluster_id;
    b.epoch = n->epoch;
    b.coord_id = n->node_id;
    memcpy(b.coord_mac, n->mac, 6);
    b.seq = ++n->beacon_seq;
    b.tx_latency_us = n->cfg.tx_latency_us;
    b.member_count = n->cluster_members;
    b.prev_tx_time_us = n->beacon_tx_us;
    b.tick_origin_us = n->tick_origin_us;
    b.tick_period_us = n->tick_period_us;
    b.tick_now = tick_of(local, n->tick_origin_us, n->tick_period_us);
    b.slot_version = n->slot_version;
    b.slot_len_us = n->slot_len_us;
    b.slot_events = n->slot_events;
    b.phy_rate = n->phy_rate;
    b.flags = (uint8_t)(n->cfg.mode == NODE_MODE_RECIPE ? BEACON_FLAG_RECIPE : 0);
    b.prev_tx_seq = n->beacon_tx_seq;
    b.coord_score = own_score(n);
    len = beacon_encode(n->txbuf, sizeof n->txbuf, &b);
    n->beacon_inflight = true;
    send_frame(n, NULL, n->txbuf, len, FRAME_BEACON);
    n->next_beacon_ms = ms + BEACON_INTERVAL_MS;
}

/* ---- election --------------------------------------------------------- */

static void follow(node_t *n, const uint8_t mac[6], uint16_t node_id, uint16_t epoch, uint32_t score,
                   uint32_t ms)
{
    bool changed = !n->have_coord || memcmp(n->coord_mac, mac, 6) != 0;

    n->have_coord = true;
    memcpy(n->coord_mac, mac, 6);
    n->coord_id = node_id;
    n->coord_score = score;
    n->coord_last_ms = ms;
    if (epoch > n->epoch) n->epoch = epoch;
    if (changed) {
        timesync_init(&n->sync);
        n->have_prev_beacon = false;
        n->cluster_valid = false;
        emit(n, NODE_EV_COORDINATOR, node_id, 0, 0, epoch);
        nlog(n, NODE_LOG_INFO, "following coordinator %u epoch %u", (unsigned)node_id, (unsigned)epoch);
    }
}

static void start_join(node_t *n, uint32_t ms)
{
    uint8_t payload[28];

    n->promote_scheduled = false;
    cap_encode(payload, &n->cap);
    wr_mac(payload + 16, n->mac);
    wr16(payload + 22, n->node_id);
    wr16(payload + 24, n->shard_valid ? n->shard.art.shard_id : SHARD_NONE);
    wr16(payload + 26, n->shard_valid ? n->shard.art.n_neurons : n->recipe_neurons);
    set_state(n, NODE_STATE_JOINING);
    if (ctrl_send(&n->ctrl, n->coord_mac, n->node_id, n->coord_id, CTRL_JOIN_REQ, payload,
                  sizeof payload, true, ms) < 0)
        nlog(n, NODE_LOG_WARN, "join request could not be queued");
    n->join_next_ms = ms + 5000u;
}

static void become_coordinator(node_t *n, uint32_t ms)
{
    uint16_t e = membership_max_epoch(&n->members);
    uint32_t tick_now = 0;
    uint64_t local = now_us(n);
    bool had_tick = n->cluster_valid && n->sync.applied_valid;

    if (n->epoch > e) e = n->epoch;
    if (had_tick) {
        uint64_t ct;
        node_cluster_time(n, local, &ct);
        tick_now = tick_of(ct, n->tick_origin_us, n->tick_period_us);
    } else if (n->have_tick_carry) {
        /* the old coordinator is gone; continue its count, advanced by the silence */
        tick_now = n->tick_carry + (ms - n->tick_carry_ms) * 1000u / (n->tick_period_us ? n->tick_period_us : 1000u);
        had_tick = true;
    }
    n->have_tick_carry = false;
    n->epoch = (uint16_t)(e + 1u);
    if (n->node_id == NODE_ID_NONE) {
        n->node_id = membership_free_id(&n->members);
        if (n->node_id == NODE_ID_NONE) n->node_id = 1;
    }
    if (n->cfg.mode == NODE_MODE_RECIPE && !n->recipe_valid) {
        n->recipe = n->cfg.recipe;
        n->recipe_valid = true;
        n->recipe_neurons = recipe_neurons_capped(n);
    }
    timesync_init(&n->sync);
    n->sync.applied_offset = 0;
    n->sync.applied_valid = true;
    n->have_prev_beacon = false;
    n->have_coord = true;
    memcpy(n->coord_mac, n->mac, 6);
    n->coord_id = n->node_id;
    n->coord_score = own_score(n);
    n->coord_last_ms = ms;
    /* cluster time is now our own clock; keep the tick index we had */
    n->cluster_valid = false;
    set_state(n, NODE_STATE_COORDINATOR);
    update_self_entry(n, ms);
    rebuild_plan(n);
    if (had_tick) n->tick_origin_us = local - (uint64_t)tick_now * n->tick_period_us;
    n->tm.elections++;
    n->beacon_tx_us = 0;
    n->beacon_tx_seq = 0;
    n->beacon_inflight = false;
    n->beacon_seq = 0;
    emit(n, NODE_EV_COORDINATOR, n->node_id, 0, 0, n->epoch);
    emit(n, NODE_EV_JOINED, n->node_id, 0, 0, n->epoch);
    nlog(n, NODE_LOG_INFO, "coordinator, epoch %u, node id %u", (unsigned)n->epoch, (unsigned)n->node_id);
    send_heartbeat(n, ms);
    send_beacon(n, ms);
    n->rebuild_wanted = true;
}

/*
 * A node claims to coordinate. Compares its key with the coordinator we
 * follow (or with ourselves) and switches when it wins.
 */
static void consider_leader(node_t *n, const uint8_t mac[6], uint16_t node_id, uint16_t epoch,
                            uint32_t score, uint32_t ms)
{
    int cmp;

    if (n->state == NODE_STATE_COORDINATOR) {
        cmp = election_compare(epoch, score, mac, n->epoch, own_score(n), n->mac);
        if (cmp <= 0) return;
        nlog(n, NODE_LOG_INFO, "yielding to %u epoch %u", (unsigned)node_id, (unsigned)epoch);
        n->cluster_valid = false;
        n->tick_started = false;
        n->bundle_pending = false;
        n->have_coord = false;
        follow(n, mac, node_id, epoch, score, ms);
        start_join(n, ms);
        return;
    }
    if (n->have_coord && memcmp(n->coord_mac, mac, 6) == 0) {
        n->coord_last_ms = ms;
        if (epoch > n->epoch) n->epoch = epoch;
        n->coord_id = node_id;
        n->coord_score = score;
        return;
    }
    if (n->have_coord) {
        cmp = election_compare(epoch, score, mac, n->epoch, n->coord_score, n->coord_mac);
        if (cmp <= 0) return;
    }
    follow(n, mac, node_id, epoch, score, ms);
    n->tick_started = false;
    n->bundle_pending = false;
    start_join(n, ms);
}

/* Number of live eligible peers whose key beats ours: they get the first chance to lead. */
static uint32_t better_candidates(const node_t *n)
{
    unsigned i;
    uint32_t higher = 0;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        const member_t *m = &n->members.m[i];
        if (m->liveness != MEMBER_ALIVE || !(m->cap.flags & CAP_FLAG_ELIGIBLE)) continue;
        if (memcmp(m->mac, n->mac, 6) == 0) continue;
        if (election_compare(n->epoch, cap_score(&m->cap), m->mac, n->epoch, own_score(n), n->mac) > 0)
            higher++;
    }
    return higher;
}

/* Remembers where the tick count stands so a successor can continue it. */
static void carry_tick(node_t *n, uint32_t ms)
{
    uint32_t t;
    if (n->cluster_valid && node_current_tick(n, &t)) {
        n->have_tick_carry = true;
        n->tick_carry = t;
        n->tick_carry_ms = ms;
    }
}

static void coordinator_lost(node_t *n, uint32_t ms)
{
    nlog(n, NODE_LOG_WARN, "coordinator %u silent", (unsigned)n->coord_id);
    carry_tick(n, ms);
    n->have_coord = false;
    n->cluster_valid = false;
    n->tick_started = false;
    n->bundle_pending = false;
    set_state(n, NODE_STATE_DISCOVER);
    if (!eligible(n)) {
        n->discover_until_ms = ms + n->cfg.discover_ms;
        return;
    }
    n->promote_at_ms = ms + better_candidates(n) * NODE_PROMOTE_STEP_MS;
    n->discover_until_ms = n->promote_at_ms;
    n->promote_scheduled = true;
}

/* ---- shard management ------------------------------------------------- */

/* Neurons the recipe gives this node, never more than the arena can hold. */
static uint16_t recipe_neurons_capped(const node_t *n)
{
    uint32_t want = recipe_neurons_for(&n->recipe, &n->cap);
    uint32_t fit = n->cfg.arena_size /
                   (sizeof(neuron_state_t) + SHARD_DELAY_SLOTS * sizeof(accum_t) + sizeof(uint16_t));
    if (fit < 16u) fit = 16u;
    return (uint16_t)(want > fit ? fit : want);
}

static void bind_artifact(node_t *n)
{
    int err = shard_bind(&n->shard, n->cfg.artifact, n->cfg.artifact_size, n->cfg.arena,
                         n->cfg.arena_size, &n->tm);
    if (err) {
        nlog(n, NODE_LOG_ERROR, "artifact rejected: %s", artifact_strerror(err));
        n->shard_valid = false;
        return;
    }
    n->shard_valid = true;
    n->shard_version = n->shard.art.shard_version;
    n->tick_started = false;
    emit(n, NODE_EV_SHARD, n->node_id, n->shard.art.shard_id, 0, n->shard.art.n_neurons);
}

static int cmp_member(const void *a, const void *b);

static int cmp_member(const void *a, const void *b)
{
    const recipe_member_t *x = (const recipe_member_t *)a, *y = (const recipe_member_t *)b;
    return x->node_id < y->node_id ? -1 : x->node_id > y->node_id ? 1 : 0;
}

static void rebuild_recipe_shard(node_t *n, uint32_t ms)
{
    recipe_member_t roster[MEMBERSHIP_MAX];
    uint16_t count = 0;
    unsigned i, j;
    int32_t size;
    int err;

    if (n->cfg.mode != NODE_MODE_RECIPE || !n->recipe_valid || n->node_id == NODE_ID_NONE) return;
    if (n->cfg.image_buf == NULL || n->cfg.arena == NULL) return;
    if (n->recipe_neurons == 0) n->recipe_neurons = recipe_neurons_capped(n);

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        const member_t *m = &n->members.m[i];
        if (m->liveness != MEMBER_ALIVE && m->liveness != MEMBER_SUSPECT) continue;
        if (m->node_id == NODE_ID_NONE || m->n_neurons == 0) continue;
        if (memcmp(m->mac, n->mac, 6) == 0) continue;
        if (!(m->flags & HB_FLAG_RECIPE)) continue;
        roster[count].node_id = m->node_id;
        roster[count].n_neurons = m->n_neurons;
        count++;
    }
    roster[count].node_id = n->node_id;
    roster[count].n_neurons = n->recipe_neurons;
    count++;
    /* insertion sort by id; the roster is small */
    for (i = 1; i < count; i++) {
        recipe_member_t key = roster[i];
        j = i;
        while (j > 0 && cmp_member(&roster[j - 1], &key) > 0) { roster[j] = roster[j - 1]; j--; }
        roster[j] = key;
    }

    size = recipe_build(&n->recipe, n->node_id, (uint16_t)(n->shard_version + 1u), roster, count,
                        n->cfg.image_buf, n->cfg.image_cap);
    if (size < 0) {
        nlog(n, NODE_LOG_ERROR, "recipe build failed: %s", artifact_strerror(size));
        n->shard_valid = false;
        n->roster_version_built = membership_roster_signature(&n->members);
        n->rebuild_wanted = false;
        n->rebuild_after_ms = ms + NODE_REBUILD_HOLDOFF_MS;
        emit(n, NODE_EV_SHARD, n->node_id, n->node_id, 0, 0);
        return;
    }
    if (n->shard_valid)
        err = shard_rebind(&n->shard, n->cfg.image_buf, (uint32_t)size);
    else
        err = shard_bind(&n->shard, n->cfg.image_buf, (uint32_t)size, n->cfg.arena, n->cfg.arena_size,
                         &n->tm);
    if (err) {
        nlog(n, NODE_LOG_ERROR, "recipe image rejected: %s", artifact_strerror(err));
        n->shard_valid = false;
        return;
    }
    n->shard_valid = true;
    n->shard_version++;
    n->roster_version_built = membership_roster_signature(&n->members);
    n->rebuild_wanted = false;
    n->rebuild_after_ms = ms + NODE_REBUILD_HOLDOFF_MS;
    emit(n, NODE_EV_SHARD, n->node_id, n->node_id, n->shard.tick, n->shard.art.n_neurons);
    nlog(n, NODE_LOG_INFO, "shard %u: %u neurons, %u sources, %ld bytes", (unsigned)n->node_id,
         (unsigned)n->shard.art.n_neurons, (unsigned)n->shard.art.n_subs, (long)size);
}

/* ---- tick loop and spike transmission --------------------------------- */

static void flush_bundle(node_t *n)
{
    uint16_t sent = 0, total = n->shard.n_fired;

    if (!n->bundle_pending) return;
    n->bundle_pending = false;
    while (sent < total) {
        unsigned left = (unsigned)(total - sent);
        uint16_t chunk = (uint16_t)(left > BUNDLE_MAX_EVENTS ? BUNDLE_MAX_EVENTS : left);
        uint8_t flags = (uint8_t)(sent + chunk < total ? BUNDLE_FLAG_MORE : 0);
        size_t len = bundle_encode(n->txbuf, sizeof n->txbuf, n->node_id, n->bundle_tick, n->bundle_seq++,
                                   flags, n->shard.fired + sent, (uint8_t)chunk);
        send_frame(n, NULL, n->txbuf, len, FRAME_BUNDLE);
        n->tm.bundles_tx++;
        n->tm.events_tx += chunk;
        sent = (uint16_t)(sent + chunk);
    }
}

static void run_ticks(node_t *n, uint64_t local)
{
    uint64_t ct;
    uint32_t t_now;

    if (!n->shard_valid || !n->cluster_valid || n->paused) return;
    if (!node_cluster_time(n, local, &ct)) return;
    t_now = tick_of(ct, n->tick_origin_us, n->tick_period_us);

    if (!n->tick_started) {
        n->shard.tick = t_now;
        n->tick_started = true;
        n->last_integrated = t_now - 1u;
    }
    if (n->shard.tick > t_now + 1u) {
        /* cluster time moved backwards (coordinator change): start over at the new tick */
        n->tm.tick_resyncs++;
        emit(n, NODE_EV_TICK_RESYNC, n->node_id, 0, t_now, -(int32_t)(n->shard.tick - t_now));
        n->shard.tick = t_now;
        n->bundle_pending = false;
    }
    if (t_now - n->shard.tick > NODE_MAX_CATCHUP_TICKS && t_now >= n->shard.tick) {
        n->tm.tick_resyncs++;
        emit(n, NODE_EV_TICK_RESYNC, n->node_id, 0, t_now, (int32_t)(t_now - n->shard.tick));
        n->shard.tick = t_now;
        n->bundle_pending = false;
    }
    while (n->shard.tick <= t_now) {
        uint32_t t = n->shard.tick;
        uint16_t nf;

        if (n->bundle_pending) flush_bundle(n);
        if (t < t_now) n->tm.tick_overruns++;
        if (n->cfg.mode == NODE_MODE_RECIPE) recipe_drive(&n->recipe, &n->shard);
        nf = shard_tick(&n->shard);
        n->last_integrated = t;
        if (nf) {
            node_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.type = NODE_EV_SPIKES;
            ev.node_id = n->node_id;
            ev.shard_id = n->shard.art.shard_id;
            ev.tick = t;
            ev.count = nf;
            ev.events = n->shard.fired;
            if (n->pf.event) n->pf.event(n->pf.ctx, &ev);
            n->bundle_pending = true;
            n->bundle_tick = t;
            if (t < t_now) flush_bundle(n);   /* late already: no point waiting for a slot */
        }
    }
    if (n->bundle_pending) {
        int rank = membership_rank(&n->members, n->node_id);
        uint64_t start = tick_start_us(n->bundle_tick, n->tick_origin_us, n->tick_period_us);
        uint64_t slot_at = start + TICK_INTEGRATE_US + (uint64_t)(rank < 0 ? 0 : rank + 1) * n->slot_len_us;
        if (ct >= slot_at) flush_bundle(n);
    }
}

/* ---- probes ----------------------------------------------------------- */

static void probe_suspects(node_t *n, uint32_t ms)
{
    unsigned i, j, asked;

    for (i = 0; i < MEMBERSHIP_MAX; i++) {
        member_t *m = &n->members.m[i];
        if (m->liveness != MEMBER_SUSPECT || m->probes != 0) continue;
        if (memcmp(m->mac, n->mac, 6) == 0) { membership_confirm(&n->members, m, ms); continue; }
        m->probes = 1;
        ctrl_send(&n->ctrl, m->mac, n->node_id, m->node_id, CTRL_PING, NULL, 0, false, ms);
        asked = 0;
        for (j = 0; j < MEMBERSHIP_MAX && asked < 2; j++) {
            const member_t *o = &n->members.m[j];
            if (o->liveness != MEMBER_ALIVE || o == m || memcmp(o->mac, n->mac, 6) == 0) continue;
            ctrl_send(&n->ctrl, o->mac, n->node_id, o->node_id, CTRL_PROBE_REQ, m->mac, 6, false, ms);
            asked++;
        }
    }
}

/* ---- frame handlers --------------------------------------------------- */

static void on_bundle(node_t *n, const uint8_t mac[6], const uint8_t *data, size_t len)
{
    bundle_view_t v;
    member_t *m;
    uint16_t gap, i;
    uint16_t events[BUNDLE_MAX_EVENTS];

    if (!bundle_decode(data, len, &v)) { n->tm.bundles_bad++; return; }
    m = membership_find_mac(&n->members, mac);
    if (m == NULL || m->liveness == MEMBER_DEAD || m->node_id != v.src_node || m->node_id == NODE_ID_NONE) {
        n->tm.bundles_ignored++;
        return;
    }
    gap = seq_tracker_observe(&m->bundle_seq, v.seq);
    if (gap) { n->tm.seq_gaps += gap; m->gaps = (uint16_t)(m->gaps + gap); }
    if (!n->shard_valid || m->shard_id == SHARD_NONE || !shard_subscribes(&n->shard, m->shard_id)) {
        n->tm.bundles_ignored++;
        return;
    }
    n->tm.bundles_rx++;
    n->tm.events_rx += v.count;
    for (i = 0; i < v.count; i++) events[i] = bundle_event(&v, i);
    shard_deliver(&n->shard, m->shard_id, v.tick, events, v.count);
}

static void on_heartbeat(node_t *n, const uint8_t mac[6], const uint8_t *data, size_t len, uint32_t ms)
{
    heartbeat_t h;
    member_t *m;
    uint8_t was;
    uint32_t version = n->members.version;

    if (!heartbeat_decode(data, len, &h) || h.cluster_id != n->cfg.cluster_id) return;
    if (memcmp(h.mac, mac, 6) != 0) return;
    n->tm.heartbeats_rx++;
    m = membership_find_mac(&n->members, mac);
    was = m ? m->liveness : MEMBER_EMPTY;
    m = membership_update(&n->members, &h, ms);
    if (m == NULL) { nlog(n, NODE_LOG_WARN, "membership table full"); return; }
    if (was != MEMBER_ALIVE) emit(n, NODE_EV_MEMBER, h.node_id, h.shard_id, 0, MEMBER_ALIVE);
    if (h.flags & HB_FLAG_COORDINATOR)
        consider_leader(n, mac, h.node_id, h.epoch, cap_score(&h.cap), ms);
    (void)version;
}

static void on_beacon(node_t *n, const uint8_t mac[6], const uint8_t *data, size_t len, uint64_t rx_us,
                      uint32_t ms)
{
    beacon_t b;

    if (!beacon_decode(data, len, &b) || b.cluster_id != n->cfg.cluster_id) return;
    if (memcmp(b.coord_mac, mac, 6) != 0) return;
    consider_leader(n, mac, b.coord_id, b.epoch, b.coord_score, ms);
    if (n->state == NODE_STATE_COORDINATOR || !n->have_coord || memcmp(n->coord_mac, mac, 6) != 0)
        return;

    /* cluster parameters are whatever the coordinator says */
    n->tick_origin_us = b.tick_origin_us;
    n->tick_period_us = b.tick_period_us;
    n->slot_len_us = b.slot_len_us;
    n->slot_events = b.slot_events;
    n->phy_rate = b.phy_rate;
    n->slot_version = b.slot_version;
    n->cluster_members = b.member_count;
    n->cluster_flags = b.flags;
    n->cluster_valid = true;
    n->coord_last_ms = ms;

    /* pair a remembered beacon's receive instant with its reported transmit time */
    if (n->have_prev_beacon && n->prev_beacon_seq == b.prev_tx_seq && b.prev_tx_time_us != 0) {
        int32_t residual = timesync_add(&n->sync, n->prev_beacon_rx_us,
                                        b.prev_tx_time_us - b.tx_latency_us);
        n->tm.sync_residual_us = residual;
        n->tm.sync_samples++;
        emit(n, NODE_EV_SYNC, n->coord_id, 0, b.tick_now, residual);
    }
    n->have_prev_beacon = true;
    n->prev_beacon_seq = b.seq;
    n->prev_beacon_rx_us = rx_us;
}

static void send_status(node_t *n, const uint8_t mac[6], uint16_t dst, uint8_t status, uint32_t ms)
{
    ctrl_send(&n->ctrl, mac, n->node_id, dst, CTRL_ART_STATUS, &status, 1, false, ms);
}

static void on_control(node_t *n, const uint8_t mac[6], const uint8_t *data, size_t len, uint32_t ms)
{
    ctrl_view_t v;
    member_t *m;

    if (!ctrl_decode(data, len, &v)) return;
    if (!ctrl_on_frame(&n->ctrl, mac, &v, n->node_id)) return;
    m = membership_find_mac(&n->members, mac);

    switch (v.type) {
    case CTRL_JOIN_REQ: {
        heartbeat_t h;
        uint8_t ack[24];
        uint16_t preferred, id = NODE_ID_NONE;
        if (n->state != NODE_STATE_COORDINATOR || v.len != 28) return;
        memset(&h, 0, sizeof h);
        h.cluster_id = n->cfg.cluster_id;
        cap_decode(v.payload, &h.cap);
        rd_mac(v.payload + 16, h.mac);
        preferred = rd16(v.payload + 22);
        h.shard_id = rd16(v.payload + 24);
        h.n_neurons = rd16(v.payload + 26);
        if (memcmp(h.mac, mac, 6) != 0) return;
        if (m != NULL && m->node_id != NODE_ID_NONE) {
            member_t *holder = membership_find_id(&n->members, m->node_id);
            if (holder == NULL || holder == m) id = m->node_id;
        }
        if (id == NODE_ID_NONE && preferred != NODE_ID_NONE && preferred <= NODE_ID_MAX &&
            membership_find_id(&n->members, preferred) == NULL)
            id = preferred;
        if (id == NODE_ID_NONE) id = membership_free_id(&n->members);
        if (id == NODE_ID_NONE) { nlog(n, NODE_LOG_ERROR, "no node id left"); return; }
        h.node_id = id;
        h.epoch = n->epoch;
        h.coord_id = n->node_id;
        h.state = NODE_STATE_JOINING;
        h.flags = (uint8_t)(n->cfg.mode == NODE_MODE_RECIPE ? HB_FLAG_RECIPE : 0);
        if (m != NULL) { h.hb_seq = m->hb_seq; h.tick = m->tick; }
        membership_update(&n->members, &h, ms);
        wr16(ack, id);
        wr16(ack + 2, n->epoch);
        wr16(ack + 4, n->node_id);
        ack[6] = n->cfg.mode;
        ack[7] = 0;
        recipe_encode(ack + 8, &n->recipe);
        ctrl_send(&n->ctrl, mac, n->node_id, id, CTRL_JOIN_ACK, ack, sizeof ack, true, ms);
        nlog(n, NODE_LOG_INFO, "admitted %02x%02x as node %u", mac[4], mac[5], (unsigned)id);
        break;
    }
    case CTRL_JOIN_ACK: {
        uint16_t id, epoch;
        if (v.len != 24) return;
        if (!n->have_coord || memcmp(n->coord_mac, mac, 6) != 0) return;
        id = rd16(v.payload);
        epoch = rd16(v.payload + 2);
        if (id == NODE_ID_NONE || id > NODE_ID_MAX) return;
        n->node_id = id;
        if (epoch > n->epoch) n->epoch = epoch;
        n->coord_id = rd16(v.payload + 4);
        n->coord_last_ms = ms;
        if (v.payload[6] == NODE_MODE_RECIPE && n->cfg.mode == NODE_MODE_RECIPE) {
            recipe_t r;
            if (recipe_decode(v.payload + 8, &r)) {
                if (!n->recipe_valid || memcmp(&r, &n->recipe, sizeof r) != 0) {
                    n->recipe = r;
                    n->recipe_valid = true;
                    n->recipe_neurons = recipe_neurons_capped(n);
                    n->shard_valid = false;
                    n->shard_version = 0;
                }
                n->rebuild_wanted = true;
            }
        }
        set_state(n, NODE_STATE_MEMBER);
        n->tick_started = false;
        emit(n, NODE_EV_JOINED, n->node_id, 0, 0, n->epoch);
        nlog(n, NODE_LOG_INFO, "joined as node %u (epoch %u)", (unsigned)id, (unsigned)n->epoch);
        send_heartbeat(n, ms);
        break;
    }
    case CTRL_PING:
        if (m) membership_confirm(&n->members, m, ms);
        ctrl_send(&n->ctrl, mac, n->node_id, v.src_node, CTRL_PONG, NULL, 0, false, ms);
        break;
    case CTRL_PONG:
        if (m) membership_confirm(&n->members, m, ms);
        break;
    case CTRL_PROBE_REQ: {
        uint8_t rsp[7];
        member_t *t;
        if (v.len != 6) return;
        t = membership_find_mac(&n->members, v.payload);
        memcpy(rsp, v.payload, 6);
        rsp[6] = (uint8_t)(t != NULL && t->liveness == MEMBER_ALIVE);
        ctrl_send(&n->ctrl, mac, n->node_id, v.src_node, CTRL_PROBE_RSP, rsp, 7, false, ms);
        break;
    }
    case CTRL_PROBE_RSP: {
        member_t *t;
        if (v.len != 7 || v.payload[6] == 0) return;
        t = membership_find_mac(&n->members, v.payload);
        if (t) membership_confirm(&n->members, t, ms);
        break;
    }
    case CTRL_FENCE: {
        if (v.len != 6) return;
        n->paused = v.payload[0] == 0;
        n->resume_tick = rd32(v.payload + 2);
        if (!n->paused) n->tick_started = false;
        break;
    }
    case CTRL_INJECT: {
        uint32_t at;
        if (v.len != 12 || !n->shard_valid) return;
        at = rd32(v.payload + 8);
        node_inject(n, rd16(v.payload), (accum_t)rd32(v.payload + 4), at);
        break;
    }
    case CTRL_TELEMETRY_REQ: {
        uint8_t out[sizeof(telemetry_t)];
        const uint32_t *w = (const uint32_t *)(const void *)&n->tm;
        unsigned i;
        for (i = 0; i < sizeof(telemetry_t) / 4u; i++) wr32(out + 4u * i, w[i]);
        ctrl_send(&n->ctrl, mac, n->node_id, v.src_node, CTRL_TELEMETRY_RSP, out, (uint8_t)sizeof out,
                  false, ms);
        break;
    }
    case CTRL_ART_BEGIN: {
        uint32_t size;
        if (v.len != 12) return;
        size = rd32(v.payload + 4);
        if (n->art_busy) { send_status(n, mac, v.src_node, 1, ms); return; }
        if (n->cfg.image_buf == NULL || size > n->cfg.image_cap || size < ARTIFACT_HDR_LEN) {
            send_status(n, mac, v.src_node, 3, ms);
            return;
        }
        n->art_busy = true;
        n->art_shard = rd16(v.payload);
        n->art_version = rd16(v.payload + 2);
        n->art_size = size;
        n->art_crc = rd32(v.payload + 8);
        n->shard_valid = false;          /* the buffer is about to be overwritten */
        send_status(n, mac, v.src_node, 0, ms);
        break;
    }
    case CTRL_ART_CHUNK: {
        uint32_t off;
        if (v.len < 5 || !n->art_busy) { send_status(n, mac, v.src_node, 4, ms); return; }
        off = rd32(v.payload);
        if (off + (v.len - 4u) > n->art_size) { send_status(n, mac, v.src_node, 4, ms); return; }
        memcpy(n->cfg.image_buf + off, v.payload + 4, v.len - 4u);
        break;
    }
    case CTRL_ART_END: {
        int err;
        if (v.len != 4 || !n->art_busy) { send_status(n, mac, v.src_node, 4, ms); return; }
        n->art_busy = false;
        if (rd32(v.payload) != n->art_crc || crc32_of(n->cfg.image_buf, n->art_size) != n->art_crc) {
            send_status(n, mac, v.src_node, 2, ms);
            emit(n, NODE_EV_ARTIFACT, n->node_id, n->art_shard, 0, 2);
            return;
        }
        err = shard_bind(&n->shard, n->cfg.image_buf, n->art_size, n->cfg.arena, n->cfg.arena_size, &n->tm);
        if (err) {
            nlog(n, NODE_LOG_ERROR, "transferred image rejected: %s", artifact_strerror(err));
            send_status(n, mac, v.src_node, 4, ms);
            emit(n, NODE_EV_ARTIFACT, n->node_id, n->art_shard, 0, 4);
            return;
        }
        n->cfg.mode = NODE_MODE_ARTIFACT;
        n->recipe_valid = false;
        n->shard_valid = true;
        n->shard_version = n->shard.art.shard_version;
        n->tick_started = false;
        send_status(n, mac, v.src_node, 0, ms);
        emit(n, NODE_EV_ARTIFACT, n->node_id, n->shard.art.shard_id, 0, 0);
        emit(n, NODE_EV_SHARD, n->node_id, n->shard.art.shard_id, 0, n->shard.art.n_neurons);
        break;
    }
    default:
        break;
    }
}

/* ---- public API ------------------------------------------------------- */

int node_init(node_t *n, const node_platform_t *pf, const node_config_t *cfg)
{
    memset(n, 0, sizeof(*n));
    n->pf = *pf;
    n->cfg = *cfg;
    if (!pf->now_us || !pf->send || !pf->get_mac || !pf->get_cap || !pf->random) return -1;
    pf->get_mac(pf->ctx, n->mac);
    pf->get_cap(pf->ctx, &n->cap);
    cap_classify(&n->cap);
    membership_init(&n->members);
    ctrl_init(&n->ctrl, cfg->cluster_id, n, ctrl_send_cb, ctrl_fail_cb, ctrl_acked_cb);
    timesync_init(&n->sync);
    telemetry_reset(&n->tm);
    n->state = NODE_STATE_BOOT;
    if (cfg->mode == NODE_MODE_ARTIFACT && cfg->artifact != NULL) bind_artifact(n);
    return 0;
}

void node_start(node_t *n)
{
    uint32_t ms = now_ms_of(now_us(n));

    set_state(n, NODE_STATE_DISCOVER);
    n->discover_until_ms = ms + n->cfg.discover_ms;
    n->promote_at_ms = n->discover_until_ms;
    n->promote_scheduled = false;
    n->next_hb_ms = ms + rnd(n, HB_JITTER_MS);
    nlog(n, NODE_LOG_INFO, "DBrain %s node %02x:%02x:%02x:%02x:%02x:%02x chip %u", DBRAIN_VERSION,
         n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5], (unsigned)n->cap.chip);
}

void node_poll(node_t *n)
{
    uint64_t local = now_us(n);
    uint32_t ms = now_ms_of(local);
    bool roster_changed;

    if (n->state == NODE_STATE_BOOT) return;

    /* liveness */
    roster_changed = membership_tick(&n->members, ms);
    if (roster_changed) {
        unsigned i;
        for (i = 0; i < MEMBERSHIP_MAX; i++) {
            member_t *m = &n->members.m[i];
            if (m->liveness == MEMBER_DEAD && m->probes != 0xFF) {
                m->probes = 0xFF;
                emit(n, NODE_EV_MEMBER, m->node_id, m->shard_id, 0, MEMBER_DEAD);
                ctrl_cancel(&n->ctrl, m->mac);
                nlog(n, NODE_LOG_WARN, "node %u is dead", (unsigned)m->node_id);
            }
        }
    }
    probe_suspects(n, ms);
    ctrl_poll(&n->ctrl, ms);

    /* heartbeats */
    if ((int32_t)(ms - n->next_hb_ms) >= 0) send_heartbeat(n, ms);

    switch (n->state) {
    case NODE_STATE_DISCOVER:
        if ((int32_t)(ms - n->discover_until_ms) >= 0) {
            if (!eligible(n)) {
                n->discover_until_ms = ms + n->cfg.discover_ms;   /* keep listening */
            } else if (!n->promote_scheduled) {
                /* heard nobody leading: queue up behind any better candidate */
                n->promote_at_ms = ms + better_candidates(n) * NODE_PROMOTE_STEP_MS;
                n->promote_scheduled = true;
            } else if ((int32_t)(ms - n->promote_at_ms) >= 0) {
                n->promote_scheduled = false;
                become_coordinator(n, ms);
            }
        }
        break;
    case NODE_STATE_JOINING:
    case NODE_STATE_MEMBER:
        if (n->state == NODE_STATE_JOINING && (int32_t)(ms - n->join_next_ms) >= 0 &&
            ctrl_pending(&n->ctrl) == 0)
            start_join(n, ms);
        if (n->have_coord && ms - n->coord_last_ms > BEACON_TIMEOUT_MS) coordinator_lost(n, ms);
        if (n->state == NODE_STATE_MEMBER && n->cluster_valid && ms != n->last_slew_ms) {
            uint32_t elapsed = ms - n->last_slew_ms;
            if (elapsed > 1000u) elapsed = 1000u;
            n->last_slew_ms = ms;
            timesync_slew(&n->sync, local, (int64_t)elapsed * NODE_SLEW_US_PER_MS, n->tick_period_us);
        }
        break;
    case NODE_STATE_COORDINATOR:
        if (n->roster_version_planned != membership_plan_signature(&n->members)) {
            uint32_t period = n->tick_period_us;
            rebuild_plan(n);
            if (period != n->tick_period_us) n->next_beacon_ms = ms;   /* announce the new plan at once */
        }
        if ((int32_t)(ms - n->next_beacon_ms) >= 0) send_beacon(n, ms);
        break;
    default:
        break;
    }

    /* recipe shard follows the roster */
    if (n->cfg.mode == NODE_MODE_RECIPE && n->recipe_valid &&
        (n->state == NODE_STATE_MEMBER || n->state == NODE_STATE_COORDINATOR) &&
        (n->rebuild_wanted || n->roster_version_built != membership_roster_signature(&n->members)) &&
        (int32_t)(ms - n->rebuild_after_ms) >= 0)
        rebuild_recipe_shard(n, ms);

    if (n->state == NODE_STATE_MEMBER || n->state == NODE_STATE_COORDINATOR) run_ticks(n, local);
}

void node_on_frame(node_t *n, const uint8_t src_mac[6], const uint8_t *data, size_t len,
                   uint64_t rx_us, bool hw_ts)
{
    uint32_t ms = now_ms_of(now_us(n));
    int type = frame_type(data, len);

    (void)hw_ts;
    if (n->state == NODE_STATE_BOOT) return;
    switch (type) {
    case FRAME_BUNDLE:    on_bundle(n, src_mac, data, len); break;
    case FRAME_HEARTBEAT: on_heartbeat(n, src_mac, data, len, ms); break;
    case FRAME_BEACON:    on_beacon(n, src_mac, data, len, rx_us, ms); break;
    case FRAME_CONTROL:   on_control(n, src_mac, data, len, ms); break;
    default:              break;
    }
}

void node_on_sent(node_t *n, uint8_t tag, uint64_t tx_us)
{
    if (tag != FRAME_BEACON) return;
    n->beacon_tx_us = tx_us;
    n->beacon_tx_seq = n->beacon_seq;
    n->beacon_inflight = false;
}

bool node_cluster_time(const node_t *n, uint64_t local_us, uint64_t *cluster_us)
{
    return timesync_cluster_time(&n->sync, local_us, cluster_us);
}

bool node_current_tick(const node_t *n, uint32_t *tick)
{
    uint64_t ct;
    if (!n->cluster_valid || !node_cluster_time(n, n->pf.now_us(n->pf.ctx), &ct)) return false;
    *tick = tick_of(ct, n->tick_origin_us, n->tick_period_us);
    return true;
}

int node_inject(node_t *n, uint16_t neuron, accum_t current, uint32_t at_tick)
{
    if (!n->shard_valid) return -1;
    if (at_tick == 0) at_tick = n->shard.tick;
    return shard_inject(&n->shard, neuron, current, at_tick);
}
