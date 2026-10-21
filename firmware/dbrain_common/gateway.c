#include <stdio.h>
#include "gateway.h"
#include "dbrain/version.h"

static uint8_t mode;

void gateway_init(uint8_t m)
{
    mode = m;
}

void gateway_set_mode(uint8_t m)
{
    mode = m;
}

uint8_t gateway_mode(void)
{
    return mode;
}

void gateway_event(const node_event_t *ev)
{
    unsigned i;

    if (mode == 0) return;
    switch (ev->type) {
    case NODE_EV_SPIKES:
        if (mode == 1) {
            printf("{\"ev\":\"spikes\",\"node\":%u,\"shard\":%u,\"tick\":%lu,\"n\":%u}\n",
                   (unsigned)ev->node_id, (unsigned)ev->shard_id, (unsigned long)ev->tick, (unsigned)ev->count);
        } else {
            printf("{\"ev\":\"spikes\",\"node\":%u,\"shard\":%u,\"tick\":%lu,\"n\":%u,\"idx\":[",
                   (unsigned)ev->node_id, (unsigned)ev->shard_id, (unsigned long)ev->tick, (unsigned)ev->count);
            for (i = 0; i < ev->count && i < 64; i++) printf("%s%u", i ? "," : "", (unsigned)ev->events[i]);
            printf("]%s}\n", ev->count > 64 ? ",\"truncated\":true" : "");
        }
        break;
    case NODE_EV_STATE:
        printf("{\"ev\":\"state\",\"node\":%u,\"state\":\"%s\"}\n", (unsigned)ev->node_id,
               node_state_name((uint8_t)ev->value));
        break;
    case NODE_EV_COORDINATOR:
        printf("{\"ev\":\"coordinator\",\"node\":%u,\"epoch\":%ld}\n", (unsigned)ev->node_id, (long)ev->value);
        break;
    case NODE_EV_MEMBER:
        printf("{\"ev\":\"member\",\"node\":%u,\"shard\":%u,\"liveness\":%ld}\n", (unsigned)ev->node_id,
               (unsigned)ev->shard_id, (long)ev->value);
        break;
    case NODE_EV_SHARD:
        printf("{\"ev\":\"shard\",\"node\":%u,\"shard\":%u,\"neurons\":%ld,\"tick\":%lu}\n",
               (unsigned)ev->node_id, (unsigned)ev->shard_id, (long)ev->value, (unsigned long)ev->tick);
        break;
    case NODE_EV_SYNC:
        printf("{\"ev\":\"sync\",\"coordinator\":%u,\"residual_us\":%ld}\n", (unsigned)ev->node_id, (long)ev->value);
        break;
    case NODE_EV_TICK_RESYNC:
        printf("{\"ev\":\"resync\",\"tick\":%lu,\"skipped\":%ld}\n", (unsigned long)ev->tick, (long)ev->value);
        break;
    case NODE_EV_JOINED:
        printf("{\"ev\":\"joined\",\"node\":%u,\"epoch\":%ld}\n", (unsigned)ev->node_id, (long)ev->value);
        break;
    case NODE_EV_ARTIFACT:
        printf("{\"ev\":\"artifact\",\"shard\":%u,\"status\":%ld}\n", (unsigned)ev->shard_id, (long)ev->value);
        break;
    default:
        break;
    }
}

void gateway_status(const node_t *n)
{
    unsigned i;
    const telemetry_t *t = &n->tm;
    uint32_t tick = 0;

    if (mode == 0) return;
    node_current_tick(n, &tick);
    printf("{\"ev\":\"status\",\"version\":\"%s\",\"node\":%u,\"state\":\"%s\",\"epoch\":%u,\"coordinator\":%u,"
           "\"tick\":%lu,\"period_us\":%lu,\"shard\":%u,\"neurons\":%u,\"bundles_tx\":%lu,\"bundles_rx\":%lu,"
           "\"events_applied\":%lu,\"events_late\":%lu,\"events_early\":%lu,\"seq_gaps\":%lu,"
           "\"overruns\":%lu,\"resyncs\":%lu,\"sync_residual_us\":%ld,\"members\":[",
           DBRAIN_VERSION, (unsigned)n->node_id, node_state_name(n->state), (unsigned)n->epoch,
           (unsigned)n->coord_id, (unsigned long)tick, (unsigned long)n->tick_period_us,
           (unsigned)(n->shard_valid ? n->shard.art.shard_id : SHARD_NONE),
           (unsigned)(n->shard_valid ? n->shard.art.n_neurons : 0), (unsigned long)t->bundles_tx,
           (unsigned long)t->bundles_rx, (unsigned long)t->events_applied, (unsigned long)t->events_late,
           (unsigned long)t->events_early, (unsigned long)t->seq_gaps, (unsigned long)t->tick_overruns,
           (unsigned long)t->tick_resyncs, (long)t->sync_residual_us);
    {
        int first = 1;
        for (i = 0; i < MEMBERSHIP_MAX; i++) {
            const member_t *m = &n->members.m[i];
            if (m->liveness == MEMBER_EMPTY) continue;
            printf("%s{\"id\":%u,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"chip\":%u,\"tier\":%u,"
                   "\"liveness\":%u,\"state\":%u,\"shard\":%u,\"neurons\":%u,\"tick\":%lu,\"drops\":%u}",
                   first ? "" : ",", (unsigned)m->node_id, m->mac[0], m->mac[1], m->mac[2], m->mac[3],
                   m->mac[4], m->mac[5], (unsigned)m->cap.chip, (unsigned)m->cap.tier, (unsigned)m->liveness,
                   (unsigned)m->node_state, (unsigned)m->shard_id, (unsigned)m->n_neurons,
                   (unsigned long)m->tick, (unsigned)m->drops);
            first = 0;
        }
    }
    printf("]}\n");
}
