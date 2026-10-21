#ifndef DBRAIN_CRC32_H
#define DBRAIN_CRC32_H

#include <stdint.h>
#include <stddef.h>

/* IEEE 802.3 CRC-32 (polynomial 0xEDB88320, init and xor-out 0xFFFFFFFF). */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t crc32_of(const void *data, size_t len);

#endif /* DBRAIN_CRC32_H */
