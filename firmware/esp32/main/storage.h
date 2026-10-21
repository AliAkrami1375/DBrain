/* Persistent settings (NVS) and the artifact partition. */
#ifndef DBRAIN_FW_STORAGE_H
#define DBRAIN_FW_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "dbrain/node.h"

/* Fills cfg from Kconfig defaults, then overrides from NVS. */
void storage_load_config(node_config_t *cfg, uint8_t *channel, bool *stream);

/* Stores one setting; returns 0 or -1 for an unknown key or bad value. */
int  storage_set(const char *key, const char *value);
void storage_erase(void);

/* Memory-maps a valid artifact image from the partition. Returns false if none. */
bool storage_map_artifact(const void **image, uint32_t *size);

/* Writes an image into the artifact partition (erases first). */
int  storage_write_artifact(const void *image, uint32_t size);

#endif
