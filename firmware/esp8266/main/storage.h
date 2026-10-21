#ifndef DBRAIN_FW_STORAGE_H
#define DBRAIN_FW_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "dbrain/node.h"

void storage_load_config(node_config_t *cfg, uint8_t *channel, bool *stream);
int  storage_set(const char *key, const char *value);
void storage_erase(void);

/* Copies a valid artifact from the partition into buf. Returns its size or 0. */
uint32_t storage_read_artifact(void *buf, uint32_t cap);
int  storage_write_artifact(const void *image, uint32_t size);

#endif
