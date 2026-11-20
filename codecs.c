/* Descompresores y cifrado de bloques MDict.
 *
 * Métodos de compresión (byte bajo de la cabecera de cada bloque, igual en
 * MDict 1.2, 2.0 y 3.0):
 *   0 sin comprimir, 1 LZO1X, 2 zlib, 3 LZMA ("alone"), 4 bzip2, 5 LZ4 (frame)
 * Los métodos 3-5 solo aparecen en MDict 3.0.
 *
 * Código de terceros incluido:
 *   lz4/   LZ4 1.10 + xxHash (BSD 2 cláusulas)
 *   lzma/  LzmaDec del LZMA SDK de Igor Pavlov (dominio público)
 *   bzip2/ bzip2 1.0.8 (licencia tipo BSD)
 */
#include <stdlib.h>
#include <string.h>

#include "md_internal.h"

#include "lz4/lz4frame.h"
#include "lz4/xxhash.h"
#include "lzma/LzmaDec.h"
#include "lzma/Lzma2Dec.h"
#include "lzma/Xz.h"
#include "lzma/7zCrc.h"
#include "lzma/XzCrc64.h"
#include "bzip2/bzlib.h"

/* bzip2 compilado con BZ_NO_STDIO exige esta función */
void bz_internal_error(int errcode) { (void)errcode; }

static void *sz_alloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void sz_free(ISzAllocPtr p, void *addr) { (void)p; free(addr); }
static const ISzAlloc g_alloc = {sz_alloc, sz_free};

static int lzma_alone(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
    if (in_len < 13) return -1;
    SizeT dst = *out_len, src = in_len - 13;
    ELzmaStatus st;
    SRes r = LzmaDecode(out, &dst, in + 13, &src, in, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &st, &g_alloc);
    if (r != SZ_OK && !(r == SZ_ERROR_INPUT_EOF && dst == *out_len)) return -1;
    *out_len = dst;
    return 0;
}

static int bzip2_buf(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
    unsigned int dst = (unsigned int)*out_len;
    int r = BZ2_bzBuffToBuffDecompress((char *)out, &dst, (char *)in, (unsigned int)in_len, 1, 0);
    if (r != BZ_OK) return -1;
    *out_len = dst;
    return 0;
}

static int lz4_frame(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
    LZ4F_dctx *ctx;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) return -1;
    size_t done_in = 0, done_out = 0;
    int rc = 0;
    while (done_in < in_len && done_out < *out_len) {
        size_t isz = in_len - done_in, osz = *out_len - done_out;
        size_t r = LZ4F_decompress(ctx, out + done_out, &osz, in + done_in, &isz, NULL);
        if (LZ4F_isError(r)) { rc = -1; break; }
        done_in += isz;
        done_out += osz;
        if (r == 0) break; /* fin del frame */
        if (isz == 0 && osz == 0) break;
    }
    LZ4F_freeDecompressionContext(ctx);
    *out_len = done_out;
    return rc;
}

int md_decompress(int method, const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
    switch (method) {
    case 0:
        if (in_len > *out_len) in_len = *out_len;
        memcpy(out, in, in_len);
        *out_len = in_len;
        return 0;
    case 1:
        return md_lzo1x_decompress(in, in_len, out, out_len);
    case 2: {
        int r = md_inflate(in, in_len, out, *out_len, 1);
        if (r < 0) return -1;
        *out_len = (size_t)r;
        return 0;
    }
    case 3:
        return lzma_alone(in, in_len, out, out_len);
    case 4:
        return bzip2_buf(in, in_len, out, out_len);
    case 5:
        return lz4_frame(in, in_len, out, out_len);
    }
    return -1;
}

uint64_t md_xxh64(const void *data, size_t n) { return XXH64(data, n, 0); }

/* Salsa20 con 8 rondas, clave de 128 bits ("expand 16-byte k") y nonce cero.
 * Es un cifrado de flujo: cifrar y descifrar son la misma operación. */
static uint32_t rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

void md_salsa20_8(uint8_t *data, size_t n, const uint8_t key[16])
{
    static const uint8_t tau[16] = {'e', 'x', 'p', 'a', 'n', 'd', ' ', '1', '6', '-', 'b', 'y', 't', 'e', ' ', 'k'};
    uint32_t in[16];
    in[0] = le32(tau);
    in[1] = le32(key); in[2] = le32(key + 4); in[3] = le32(key + 8); in[4] = le32(key + 12);
    in[5] = le32(tau + 4);
    in[6] = in[7] = 0;          /* nonce */
    in[8] = in[9] = 0;          /* contador de bloque */
    in[10] = le32(tau + 8);
    in[11] = in[1]; in[12] = in[2]; in[13] = in[3]; in[14] = in[4];
    in[15] = le32(tau + 12);

    for (size_t pos = 0; pos < n; pos += 64) {
        uint32_t x[16];
        memcpy(x, in, sizeof x);
        for (int i = 0; i < 8; i += 2) {
            x[4] ^= rotl(x[0] + x[12], 7);   x[8] ^= rotl(x[4] + x[0], 9);
            x[12] ^= rotl(x[8] + x[4], 13);  x[0] ^= rotl(x[12] + x[8], 18);
            x[9] ^= rotl(x[5] + x[1], 7);    x[13] ^= rotl(x[9] + x[5], 9);
            x[1] ^= rotl(x[13] + x[9], 13);  x[5] ^= rotl(x[1] + x[13], 18);
            x[14] ^= rotl(x[10] + x[6], 7);  x[2] ^= rotl(x[14] + x[10], 9);
            x[6] ^= rotl(x[2] + x[14], 13);  x[10] ^= rotl(x[6] + x[2], 18);
            x[3] ^= rotl(x[15] + x[11], 7);  x[7] ^= rotl(x[3] + x[15], 9);
            x[11] ^= rotl(x[7] + x[3], 13);  x[15] ^= rotl(x[11] + x[7], 18);
            x[1] ^= rotl(x[0] + x[3], 7);    x[2] ^= rotl(x[1] + x[0], 9);
            x[3] ^= rotl(x[2] + x[1], 13);   x[0] ^= rotl(x[3] + x[2], 18);
            x[6] ^= rotl(x[5] + x[4], 7);    x[7] ^= rotl(x[6] + x[5], 9);
            x[4] ^= rotl(x[7] + x[6], 13);   x[5] ^= rotl(x[4] + x[7], 18);
            x[11] ^= rotl(x[10] + x[9], 7);  x[8] ^= rotl(x[11] + x[10], 9);
            x[9] ^= rotl(x[8] + x[11], 13);  x[10] ^= rotl(x[9] + x[8], 18);
            x[12] ^= rotl(x[15] + x[14], 7); x[13] ^= rotl(x[12] + x[15], 9);
            x[14] ^= rotl(x[13] + x[12], 13); x[15] ^= rotl(x[14] + x[13], 18);
        }
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) {
            uint32_t v = x[i] + in[i];
            ks[4 * i] = (uint8_t)v; ks[4 * i + 1] = (uint8_t)(v >> 8);
            ks[4 * i + 2] = (uint8_t)(v >> 16); ks[4 * i + 3] = (uint8_t)(v >> 24);
        }
        size_t take = n - pos < 64 ? n - pos : 64;
        for (size_t i = 0; i < take; i++) data[pos + i] ^= ks[i];
        if (++in[8] == 0) in[9]++;
    }
}


/* ---------------------------------------------------------------------- */
/* LZMA2 sin contenedor (Slob "lzma2") y XZ (clusters de ZIM)              */
/* El tamaño descomprimido no se conoce: se prueba con búferes crecientes. */
/* ---------------------------------------------------------------------- */

uint8_t *md_lzma2_raw_heap(const uint8_t *in, size_t in_len, size_t *out_len)
{
    size_t cap = in_len * 4 + 65536;
    for (int tries = 0; tries < 12; tries++, cap *= 2) {
        if (cap > 512u * 1024 * 1024) break;
        uint8_t *out = malloc(cap);
        if (!out) return NULL;
        SizeT dst = cap, src = in_len;
        ELzmaStatus st;
        /* propiedad 40 = diccionario máximo: en una sola llamada el búfer de
         * salida hace de diccionario, así que no se reserva memoria extra */
        SRes r = Lzma2Decode(out, &dst, in, &src, 40, LZMA_FINISH_ANY, &st, &g_alloc);
        if (r == SZ_OK && (st == LZMA_STATUS_FINISHED_WITH_MARK || src == in_len)) {
            *out_len = dst;
            return out;
        }
        free(out);
        if (r != SZ_OK && r != SZ_ERROR_INPUT_EOF) return NULL;
    }
    return NULL;
}

static void xz_init_tables(void)
{
    static int done = 0;
    if (!done) {
        CrcGenerateTable();
        Crc64GenerateTable();
        done = 1;
    }
}

uint8_t *md_xz_heap(const uint8_t *in, size_t in_len, size_t *out_len)
{
    xz_init_tables();
    CXzUnpacker xz;
    XzUnpacker_Construct(&xz, &g_alloc);
    XzUnpacker_Init(&xz);
    size_t cap = in_len * 4 + 65536, n = 0, pos = 0;
    uint8_t *out = malloc(cap);
    int ok = 0;
    while (out) {
        SizeT dst = cap - n, src = in_len - pos;
        ECoderStatus st;
        SRes r = XzUnpacker_Code(&xz, out + n, &dst, in + pos, &src, 1, CODER_FINISH_ANY, &st);
        n += dst;
        pos += src;
        if (r != SZ_OK) break;
        if (XzUnpacker_IsStreamWasFinished(&xz) || (pos >= in_len && st != CODER_STATUS_NOT_FINISHED)) { ok = 1; break; }
        if (n == cap) {
            if (cap > 512u * 1024 * 1024) break;
            uint8_t *nb = realloc(out, cap * 2);
            if (!nb) break;
            out = nb;
            cap *= 2;
        } else if (src == 0 && dst == 0) {
            ok = n > 0;
            break;
        }
    }
    XzUnpacker_Free(&xz);
    if (!ok) { free(out); return NULL; }
    *out_len = n;
    return out;
}

/* bzip2 a un búfer creciente (Slob "bz2") */
uint8_t *md_bzip2_heap(const uint8_t *in, size_t in_len, size_t *out_len)
{
    size_t cap = in_len * 6 + 65536;
    for (int tries = 0; tries < 10; tries++, cap *= 2) {
        uint8_t *out = malloc(cap);
        if (!out) return NULL;
        unsigned int dst = (unsigned int)cap;
        int r = BZ2_bzBuffToBuffDecompress((char *)out, &dst, (char *)in, (unsigned int)in_len, 1, 0);
        if (r == BZ_OK) { *out_len = dst; return out; }
        free(out);
        if (r != BZ_OUTBUFF_FULL) return NULL;
    }
    return NULL;
}
