/* RIPEMD-128 (para las claves de cifrado de MDict) y descompresor LZO1X. */
#include <string.h>
#include "md_internal.h"

/* ---------------------------------------------------------------------- */
/* RIPEMD-128 (transcrito de pyglossary/plugin_lib/ripemd128.py)           */
/* ---------------------------------------------------------------------- */

static const uint8_t R_L[64] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
    3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
    1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2};
static const uint8_t R_R[64] = {
    5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
    6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
    15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
    8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14};
static const uint8_t S_L[64] = {
    11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
    7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
    11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
    11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12};
static const uint8_t S_R[64] = {
    8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
    9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
    9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
    15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8};

static uint32_t rf(int j, uint32_t x, uint32_t y, uint32_t z)
{
    if (j < 16) return x ^ y ^ z;
    if (j < 32) return (x & y) | (z & ~x);
    if (j < 48) return (x | ~y) ^ z;
    return (x & z) | (y & ~z);
}
static uint32_t KL(int j) { return j < 16 ? 0 : j < 32 ? 0x5A827999 : j < 48 ? 0x6ED9EBA1 : 0x8F1BBCDC; }
static uint32_t KR(int j) { return j < 16 ? 0x50A28BE6 : j < 32 ? 0x5C4DD124 : j < 48 ? 0x6D703EF3 : 0; }
static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

void ripemd128(const uint8_t *msg, size_t len, uint8_t out[16])
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476;
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t buf[256];
    uint8_t *m = total <= sizeof buf ? buf : NULL;
    if (!m) return; /* solo se usa con mensajes cortos */
    memset(m, 0, total);
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 8 + i] = (uint8_t)(bits >> (8 * i));

    for (size_t blk = 0; blk < total; blk += 64) {
        uint32_t X[16];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = m + blk + 4 * i;
            X[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        uint32_t A = h0, B = h1, C = h2, D = h3, Ap = h0, Bp = h1, Cp = h2, Dp = h3, T;
        for (int j = 0; j < 64; j++) {
            T = rol(A + rf(j, B, C, D) + X[R_L[j]] + KL(j), S_L[j]);
            A = D; D = C; C = B; B = T;
            T = rol(Ap + rf(63 - j, Bp, Cp, Dp) + X[R_R[j]] + KR(j), S_R[j]);
            Ap = Dp; Dp = Cp; Cp = Bp; Bp = T;
        }
        T = h1 + C + Dp;
        h1 = h2 + D + Ap;
        h2 = h3 + A + Bp;
        h3 = h0 + B + Cp;
        h0 = T;
    }
    uint32_t h[4] = {h0, h1, h2, h3};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (8 * k));
}

/* ---------------------------------------------------------------------- */
/* LZO1X: descompresión segura (misma lógica que lzo1x_decompress_safe     */
/* del kernel de Linux, sin la extensión RLE).                              */
/* ---------------------------------------------------------------------- */

#define NEED_IP(x) do { if ((size_t)(ip_end - ip) < (size_t)(x)) return -1; } while (0)
#define NEED_OP(x) do { if ((size_t)(op_end - op) < (size_t)(x)) return -2; } while (0)
#define TEST_LB(p) do { if ((p) < out) return -3; } while (0)

int md_lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
    const uint8_t *ip = in, *ip_end = in + in_len;
    uint8_t *op = out, *op_end = out + *out_len;
    const uint8_t *m_pos;
    size_t t, next;
    int state = 0;

    if (in_len < 3) return -1;
    if (*ip > 17) {
        t = (size_t)(*ip++ - 17);
        if (t < 4) {
            next = t;
            goto match_next;
        }
        goto copy_literal_run;
    }

    for (;;) {
        NEED_IP(1);
        t = *ip++;
        if (t < 16) {
            if (state == 0) {
                if (t == 0) {
                    while (1) {
                        NEED_IP(1);
                        if (*ip != 0) break;
                        t += 255;
                        ip++;
                    }
                    t += 15 + *ip++;
                }
                t += 3;
            copy_literal_run:
                NEED_IP(t);
                NEED_OP(t);
                memcpy(op, ip, t);
                op += t;
                ip += t;
                state = 4;
                continue;
            } else if (state != 4) {
                next = t & 3;
                NEED_IP(1);
                m_pos = op - 1 - (t >> 2) - ((size_t)*ip++ << 2);
                TEST_LB(m_pos);
                NEED_OP(2);
                op[0] = m_pos[0];
                op[1] = m_pos[1];
                op += 2;
                goto match_next;
            } else {
                next = t & 3;
                NEED_IP(1);
                m_pos = op - (1 + 0x0800) - (t >> 2) - ((size_t)*ip++ << 2);
                TEST_LB(m_pos);
                NEED_OP(3);
                op[0] = m_pos[0];
                op[1] = m_pos[1];
                op[2] = m_pos[2];
                op += 3;
                goto match_next;
            }
        } else if (t >= 64) {
            next = t & 3;
            NEED_IP(1);
            m_pos = op - 1 - ((t >> 2) & 7) - ((size_t)*ip++ << 3);
            t = (t >> 5) - 1 + (3 - 1);
        } else if (t >= 32) {
            t = (t & 31) + (3 - 1);
            if (t == 2) {
                while (1) {
                    NEED_IP(1);
                    if (*ip != 0) break;
                    t += 255;
                    ip++;
                }
                t += 31 + *ip++;
            }
            NEED_IP(2);
            next = (size_t)ip[0] | ((size_t)ip[1] << 8);
            ip += 2;
            m_pos = op - 1 - (next >> 2);
            next &= 3;
        } else {
            size_t hi = (t & 8) << 11;
            t = (t & 7) + (3 - 1);
            if (t == 2) {
                while (1) {
                    NEED_IP(1);
                    if (*ip != 0) break;
                    t += 255;
                    ip++;
                }
                t += 7 + *ip++;
            }
            NEED_IP(2);
            next = (size_t)ip[0] | ((size_t)ip[1] << 8);
            ip += 2;
            size_t dist = hi + (next >> 2);
            next &= 3;
            if (dist == 0) goto eof_found;
            m_pos = op - dist - 0x4000;
        }
        TEST_LB(m_pos);
        NEED_OP(t);
        for (size_t k = 0; k < t; k++) op[k] = m_pos[k]; /* puede solaparse */
        op += t;
    match_next:
        state = (int)next;
        t = next;
        if (t) {
            NEED_IP(t);
            NEED_OP(t);
            for (size_t k = 0; k < t; k++) *op++ = *ip++;
        }
    }

eof_found:
    *out_len = (size_t)(op - out);
    return t != 3 ? -4 : 0;
}
