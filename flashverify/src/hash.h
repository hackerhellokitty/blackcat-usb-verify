#ifndef HASH_H
#define HASH_H

#include <stdint.h>
#include <stddef.h>

void sha256_chunk(const uint8_t *data, size_t len, uint8_t out[32]);
void sha256_to_hex(const uint8_t hash[32], char out[65]);

#endif /* HASH_H */
