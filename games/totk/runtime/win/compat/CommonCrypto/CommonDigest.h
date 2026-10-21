/* Windows stand-in for CommonCrypto's SHA-256 (runtime/win/sha256.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t CC_LONG;
typedef struct { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; unsigned used; } CC_SHA256_CTX;
#define CC_SHA256_DIGEST_LENGTH 32
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);
