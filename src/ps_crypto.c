/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope Ltd. See LICENSE: for use with the Pinscope service only. */
/*
 * X25519 uses the public-domain ref10 field arithmetic (Bernstein, Duif,
 * Lange, Schwabe, Yang) and the ladder of RFC 7748, Poly1305 the
 * public-domain poly1305-donna (Andrew Moon); ChaCha20 and the AEAD construction
 * follow RFC 8439; SHA-256 follows FIPS 180-4. The server speaks the same
 * algorithms through OpenSSL, and the test suite runs one against the other.
 */
#include <string.h>

#include "ps_crypto.h"

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void wr64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* ---- SHA-256 ---- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(ps_sha256_t *c, const uint8_t *p)
{
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, c->h, sizeof(s));
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25);
        uint32_t ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
        uint32_t t1 = s[7] + s1 + ch + K256[i] + w[i];
        uint32_t s0 = ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22);
        uint32_t maj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
        uint32_t t2 = s0 + maj;
        s[7] = s[6];
        s[6] = s[5];
        s[5] = s[4];
        s[4] = s[3] + t1;
        s[3] = s[2];
        s[2] = s[1];
        s[1] = s[0];
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) {
        c->h[i] += s[i];
    }
}

void ps_sha256_init(ps_sha256_t *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
}

void ps_sha256_update(ps_sha256_t *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = (size_t)(c->len & 63);
    c->len += len;
    while (len) {
        size_t n = 64 - have < len ? 64 - have : len;
        memcpy(c->buf + have, p, n);
        have += n;
        p += n;
        len -= n;
        if (have == 64) {
            sha256_block(c, c->buf);
            have = 0;
        }
    }
}

void ps_sha256_final(ps_sha256_t *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad[72] = { 0x80 }, lenb[8];
    size_t have = (size_t)(c->len & 63);
    size_t padlen = (have < 56 ? 56 : 120) - have;
    for (int i = 0; i < 8; i++) {
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    ps_sha256_update(c, pad, padlen);
    ps_sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

static void hmac(const uint8_t key[32], const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
                 uint8_t out[32])
{
    ps_sha256_t c;
    uint8_t pad[64];
    memset(pad, 0x36, sizeof(pad));
    for (int i = 0; i < 32; i++) {
        pad[i] ^= key[i];
    }
    ps_sha256_init(&c);
    ps_sha256_update(&c, pad, 64);
    ps_sha256_update(&c, a, alen);
    ps_sha256_update(&c, b, blen);
    ps_sha256_final(&c, out);
    memset(pad, 0x5c, sizeof(pad));
    for (int i = 0; i < 32; i++) {
        pad[i] ^= key[i];
    }
    ps_sha256_init(&c);
    ps_sha256_update(&c, pad, 64);
    ps_sha256_update(&c, out, 32);
    ps_sha256_final(&c, out);
}

void ps_hkdf2(const uint8_t ck[32], const uint8_t *ikm, size_t len, uint8_t o1[32], uint8_t o2[32])
{
    static const uint8_t one = 1, two = 2;
    uint8_t prk[32], t1[32];
    hmac(ck, ikm, len, NULL, 0, prk);
    hmac(prk, &one, 1, NULL, 0, t1);
    hmac(prk, t1, 32, &two, 1, o2);
    memcpy(o1, t1, 32);
    memset(prk, 0, sizeof(prk));
}

/* ---- X25519 ----
 * Field elements in ten signed limbs of alternately 26 and 25 bits (the
 * "ref10" representation), so that every product is 32 x 32 -> 64 bits and
 * a multiplication takes 100 of them. The ladder follows RFC 7748. */

typedef int32_t fe[10];

static void fe_carry(fe h, int64_t t[10])
{
    int64_t c;
    c = (t[0] + (1 << 25)) >> 26; t[1] += c; t[0] -= c * (1LL << 26);
    c = (t[4] + (1 << 25)) >> 26; t[5] += c; t[4] -= c * (1LL << 26);
    c = (t[1] + (1 << 24)) >> 25; t[2] += c; t[1] -= c * (1LL << 25);
    c = (t[5] + (1 << 24)) >> 25; t[6] += c; t[5] -= c * (1LL << 25);
    c = (t[2] + (1 << 25)) >> 26; t[3] += c; t[2] -= c * (1LL << 26);
    c = (t[6] + (1 << 25)) >> 26; t[7] += c; t[6] -= c * (1LL << 26);
    c = (t[3] + (1 << 24)) >> 25; t[4] += c; t[3] -= c * (1LL << 25);
    c = (t[7] + (1 << 24)) >> 25; t[8] += c; t[7] -= c * (1LL << 25);
    c = (t[4] + (1 << 25)) >> 26; t[5] += c; t[4] -= c * (1LL << 26);
    c = (t[8] + (1 << 25)) >> 26; t[9] += c; t[8] -= c * (1LL << 26);
    c = (t[9] + (1 << 24)) >> 25; t[0] += c * 19; t[9] -= c * (1LL << 25);
    c = (t[0] + (1 << 25)) >> 26; t[1] += c; t[0] -= c * (1LL << 26);
    for (int i = 0; i < 10; i++) {
        h[i] = (int32_t)t[i];
    }
}

/* Inputs may be sums or differences of two carried elements. */
static void fe_mul(fe h, const fe f, const fe g)
{
    int32_t g19[10];
    int64_t t[10] = { 0 };
    for (int j = 0; j < 10; j++) {
        g19[j] = 19 * g[j];
    }
    for (int i = 0; i < 10; i++) {
        /* An odd limb times an odd limb lands half a bit above the even
         * position it is added to, hence the factor 2. */
        int32_t fi = f[i], fi2 = (i & 1) ? 2 * f[i] : f[i];
        for (int j = 0; j < 10 - i; j++) {
            t[i + j] += (int64_t)((j & 1) ? fi2 : fi) * g[j];
        }
        for (int j = 10 - i; j < 10; j++) { /* 2^255 = 19 */
            t[i + j - 10] += (int64_t)((j & 1) ? fi2 : fi) * g19[j];
        }
    }
    fe_carry(h, t);
}

static void fe_mul_small(fe h, const fe f, int32_t k)
{
    int64_t t[10];
    for (int i = 0; i < 10; i++) {
        t[i] = (int64_t)f[i] * k;
    }
    fe_carry(h, t);
}

static void fe_add(fe h, const fe f, const fe g)
{
    for (int i = 0; i < 10; i++) {
        h[i] = f[i] + g[i];
    }
}

static void fe_sub(fe h, const fe f, const fe g)
{
    for (int i = 0; i < 10; i++) {
        h[i] = f[i] - g[i];
    }
}

static void fe_cswap(fe f, fe g, int32_t b)
{
    int32_t mask = -b;
    for (int i = 0; i < 10; i++) {
        int32_t x = mask & (f[i] ^ g[i]);
        f[i] ^= x;
        g[i] ^= x;
    }
}

static uint32_t ld3(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

static void fe_frombytes(fe h, const uint8_t s[32])
{
    int64_t t[10];
    t[0] = rd32le(s);
    t[1] = (int64_t)ld3(s + 4) << 6;
    t[2] = (int64_t)ld3(s + 7) << 5;
    t[3] = (int64_t)ld3(s + 10) << 3;
    t[4] = (int64_t)ld3(s + 13) << 2;
    t[5] = rd32le(s + 16);
    t[6] = (int64_t)ld3(s + 20) << 7;
    t[7] = (int64_t)ld3(s + 23) << 5;
    t[8] = (int64_t)ld3(s + 26) << 4;
    t[9] = (int64_t)(ld3(s + 29) & 0x7fffff) << 2; /* the top bit is ignored */
    fe_carry(h, t);
}

/* The unique representative below p, little-endian. */
static void fe_tobytes(uint8_t s[32], const fe f)
{
    int32_t h[10], q;
    memcpy(h, f, sizeof(h));
    q = (19 * h[9] + (1 << 24)) >> 25;
    for (int i = 0; i < 10; i++) {
        q = (h[i] + q) >> ((i & 1) ? 25 : 26);
    }
    h[0] += 19 * q; /* now h - q * p, and 0 <= h < p */
    for (int i = 0; i < 9; i++) {
        int bits = (i & 1) ? 25 : 26;
        int32_t c = h[i] >> bits;
        h[i + 1] += c;
        h[i] -= c * (1 << bits);
    }
    h[9] &= (1 << 25) - 1;

    static const uint8_t pos[10] = { 0, 26, 51, 77, 102, 128, 153, 179, 204, 230 };
    memset(s, 0, 32);
    for (int i = 0; i < 10; i++) {
        uint64_t v = (uint64_t)(uint32_t)h[i] << (pos[i] & 7);
        for (int k = pos[i] >> 3; v && k < 32; k++, v >>= 8) {
            s[k] |= (uint8_t)v;
        }
    }
}

static void fe_invert(fe out, const fe z)
{
    fe e;
    memcpy(e, z, sizeof(fe));
    for (int i = 253; i >= 0; i--) { /* z^(p - 2) */
        fe_mul(e, e, e);
        if (i != 2 && i != 4) {
            fe_mul(e, e, z);
        }
    }
    memcpy(out, e, sizeof(fe));
}

void ps_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t *point)
{
    static const uint8_t base[32] = { 9 };
    uint8_t k[32];
    fe x1, x2 = { 1 }, z2 = { 0 }, x3, z3 = { 1 }, a, aa, b, bb, e, c, d, da, cb;
    int32_t swap = 0;

    memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] = (uint8_t)((k[31] & 127) | 64);
    fe_frombytes(x1, point ? point : base);
    memcpy(x3, x1, sizeof(fe));

    for (int t = 254; t >= 0; t--) {
        int32_t bit = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(a, x2, z2);
        fe_mul(aa, a, a);
        fe_sub(b, x2, z2);
        fe_mul(bb, b, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(x3, da, cb);
        fe_mul(x3, x3, x3);
        fe_sub(z3, da, cb);
        fe_mul(z3, z3, z3);
        fe_mul(z3, z3, x1);
        fe_mul(x2, aa, bb);
        fe_mul_small(z2, e, 121665);
        fe_add(z2, z2, aa);
        fe_mul(z2, z2, e);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
    memset(k, 0, sizeof(k));
}

/* ---- ChaCha20 ---- */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROL(d, 16); \
    c += d; b ^= c; b = ROL(b, 12); \
    a += b; d ^= a; d = ROL(d, 8);  \
    c += d; b ^= c; b = ROL(b, 7)

static void chacha_block(const uint32_t in[16], uint8_t out[64])
{
    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        out[4 * i] = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}

static void chacha_init(uint32_t st[16], const uint8_t key[32], uint64_t n)
{
    st[0] = 0x61707865;
    st[1] = 0x3320646e;
    st[2] = 0x79622d32;
    st[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) {
        st[4 + i] = rd32le(key + 4 * i);
    }
    st[12] = 0;                    /* block counter */
    st[13] = 0;                    /* nonce: four zero bytes, then the counter */
    st[14] = (uint32_t)n;
    st[15] = (uint32_t)(n >> 32);
}

static void chacha_xor(uint32_t st[16], uint8_t *buf, size_t len)
{
    uint8_t ks[64];
    while (len) {
        size_t n = len < 64 ? len : 64;
        chacha_block(st, ks);
        st[12]++;
        for (size_t i = 0; i < n; i++) {
            buf[i] ^= ks[i];
        }
        buf += n;
        len -= n;
    }
    memset(ks, 0, sizeof(ks));
}

/* ---- Poly1305 ----
 * Five 26-bit limbs, after the public-domain poly1305-donna (Andrew Moon). */

typedef struct {
    uint32_t r[5], h[5];
} poly_t;

static void poly_init(poly_t *p, const uint8_t key[16])
{
    p->r[0] = rd32le(key) & 0x3ffffff;
    p->r[1] = (rd32le(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (rd32le(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (rd32le(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (rd32le(key + 12) >> 8) & 0x00fffff;
    memset(p->h, 0, sizeof(p->h));
}

static void poly_block(poly_t *p, const uint8_t m[16])
{
    const uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    uint64_t d0, d1, d2, d3, d4;

    h0 += rd32le(m) & 0x3ffffff;
    h1 += (rd32le(m + 3) >> 2) & 0x3ffffff;
    h2 += (rd32le(m + 6) >> 4) & 0x3ffffff;
    h3 += (rd32le(m + 9) >> 6) & 0x3ffffff;
    h4 += (rd32le(m + 12) >> 8) | (1UL << 24);

    d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

/* Absorbs data in 16-byte blocks, the last one padded with zeros (which is
 * how the AEAD feeds every part of its input). */
static void poly_blocks(poly_t *p, const uint8_t *m, size_t n)
{
    for (; n >= 16; m += 16, n -= 16) {
        poly_block(p, m);
    }
    if (n) {
        uint8_t last[16] = { 0 };
        memcpy(last, m, n);
        poly_block(p, last);
    }
}

static void poly_final(poly_t *p, const uint8_t s[16], uint8_t out[16])
{
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    uint32_t g0, g1, g2, g3, g4, c, mask;
    uint64_t f;

    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    /* h - p, kept only when h >= p */
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1UL << 26);
    mask = (g4 >> 31) - 1;
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);

    /* h + s, mod 2^128 */
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);
    f = (uint64_t)h0 + rd32le(s);
    wr32le(out, (uint32_t)f);
    f = (uint64_t)h1 + rd32le(s + 4) + (f >> 32);
    wr32le(out + 4, (uint32_t)f);
    f = (uint64_t)h2 + rd32le(s + 8) + (f >> 32);
    wr32le(out + 8, (uint32_t)f);
    f = (uint64_t)h3 + rd32le(s + 12) + (f >> 32);
    wr32le(out + 12, (uint32_t)f);
}

/* ---- AEAD ---- */

static void aead_tag(uint32_t st[16], const uint8_t *ad, size_t adlen, const uint8_t *ct, size_t len,
                     uint8_t tag[16])
{
    uint8_t otk[64], lens[16];
    poly_t p;
    chacha_block(st, otk); /* block 0 gives the one-time Poly1305 key */
    st[12] = 1;
    poly_init(&p, otk);
    poly_blocks(&p, ad, adlen);
    poly_blocks(&p, ct, len);
    wr64le(lens, adlen);
    wr64le(lens + 8, len);
    poly_blocks(&p, lens, 16);
    poly_final(&p, otk + 16, tag);
    memset(otk, 0, sizeof(otk));
    memset(&p, 0, sizeof(p));
}

void ps_aead_seal(const uint8_t key[32], uint64_t n, const uint8_t *ad, size_t adlen,
                  uint8_t *buf, size_t len)
{
    uint32_t st[16], mac[16];
    chacha_init(st, key, n);
    memcpy(mac, st, sizeof(st));
    st[12] = 1;
    chacha_xor(st, buf, len);
    aead_tag(mac, ad, adlen, buf, len, buf + len);
    memset(st, 0, sizeof(st));
    memset(mac, 0, sizeof(mac));
}

int ps_aead_open(const uint8_t key[32], uint64_t n, const uint8_t *ad, size_t adlen,
                 uint8_t *buf, size_t len)
{
    uint32_t st[16], mac[16];
    uint8_t tag[16], diff = 0;
    chacha_init(st, key, n);
    memcpy(mac, st, sizeof(st));
    aead_tag(mac, ad, adlen, buf, len, tag);
    for (int i = 0; i < 16; i++) {
        diff |= (uint8_t)(tag[i] ^ buf[len + i]);
    }
    if (diff == 0) {
        st[12] = 1;
        chacha_xor(st, buf, len);
    }
    memset(st, 0, sizeof(st));
    memset(mac, 0, sizeof(mac));
    return diff ? -1 : 0;
}
