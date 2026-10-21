/*
 * Console commands shared by every board. One line in, text or JSON out.
 * The port supplies what differs between boards through cli_hooks_t.
 */
#ifndef DBRAIN_FW_CLI_H
#define DBRAIN_FW_CLI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dbrain/node.h"

typedef struct {
    node_t  *node;
    int    (*set)(const char *key, const char *value);
    void   (*erase)(void);
    void   (*reboot)(void);
    int    (*save_artifact)(const void *image, uint32_t size);
    int    (*send_raw)(const uint8_t *mac, const uint8_t *frame, size_t len);
    uint64_t (*now_us)(void);
    uint32_t (*rx_dropped)(void);
    void   (*lock)(void);     /* serialises access to the node with its task */
    void   (*unlock)(void);
} cli_hooks_t;

void cli_init(const cli_hooks_t *hooks);

/* Executes one command line (modified in place). */
void cli_execute(char *line);

/* Collects characters into lines and executes them. */
void cli_feed_char(char c);

#endif
