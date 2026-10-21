/*
 * Common framing for every DBrain frame on the air.
 *
 * Every frame starts with a two-byte preamble:
 *
 *   byte 0   FRAME_MAGIC (0xDB)
 *   byte 1   high nibble: DBRAIN_PROTO_VERSION, low nibble: frame type
 *
 * The remaining layout depends on the type and is specified in
 * docs/protocol.md. All multi-byte fields are little-endian and are read and
 * written byte-wise, so frames may sit at any alignment.
 *
 * Frames never exceed FRAME_MAX_LEN (250 bytes), the ESP-NOW v1 payload
 * limit, so that every supported chip, including the ESP8266, can receive
 * every frame.
 */
#ifndef DBRAIN_FRAME_H
#define DBRAIN_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "dbrain/version.h"

#define FRAME_MAGIC    0xDBu
#define FRAME_MAX_LEN  250u
#define FRAME_PREAMBLE 2u

enum {
    FRAME_BUNDLE    = 1,  /* spike bundle, broadcast                 */
    FRAME_HEARTBEAT = 2,  /* membership heartbeat, broadcast          */
    FRAME_BEACON    = 3,  /* coordinator tick/sync beacon, broadcast  */
    FRAME_CONTROL   = 4   /* control plane, unicast, reliable         */
};

static inline uint8_t frame_ver_type(uint8_t type)
{
    return (uint8_t)((DBRAIN_PROTO_VERSION << 4) | (type & 0x0F));
}

/* Returns the frame type, or -1 if the preamble is not ours. */
static inline int frame_type(const uint8_t *buf, size_t len)
{
    if (len < FRAME_PREAMBLE || buf[0] != FRAME_MAGIC) return -1;
    if ((buf[1] >> 4) != DBRAIN_PROTO_VERSION) return -1;
    return buf[1] & 0x0F;
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static inline void wr_mac(uint8_t *p, const uint8_t mac[6])
{
    int i;
    for (i = 0; i < 6; i++) p[i] = mac[i];
}

static inline void rd_mac(const uint8_t *p, uint8_t mac[6])
{
    int i;
    for (i = 0; i < 6; i++) mac[i] = p[i];
}

#endif /* DBRAIN_FRAME_H */
