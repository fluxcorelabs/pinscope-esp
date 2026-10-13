/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope Ltd. See LICENSE: for use with the Pinscope service only. */
/*
 * The primitives of the lite transport: X25519, ChaCha20-Poly1305 (RFC 8439)
 * and SHA-256 with HKDF, as the Noise framework combines them. Standard
 * algorithms in small, portable implementations; nothing here allocates.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
} ps_sha256_t;

void ps_sha256_init(ps_sha256_t *c);
void ps_sha256_update(ps_sha256_t *c, const void *data, size_t len);
void ps_sha256_final(ps_sha256_t *c, uint8_t out[32]);

/* Noise HKDF with two outputs: (o1, o2) = HKDF(chaining key, input). */
void ps_hkdf2(const uint8_t ck[32], const uint8_t *ikm, size_t len, uint8_t o1[32], uint8_t o2[32]);

/* out = scalar * point; point NULL means the base point (a public key). */
void ps_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point);

/* ChaCha20-Poly1305 in place, with the Noise nonce (a 64-bit counter).
 * seal writes the 16-byte tag after the data; open returns 0 when it matches. */
void ps_aead_seal(const uint8_t key[32], uint64_t n, const uint8_t *ad, size_t adlen,
                  uint8_t *buf, size_t len);
int  ps_aead_open(const uint8_t key[32], uint64_t n, const uint8_t *ad, size_t adlen,
                  uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
