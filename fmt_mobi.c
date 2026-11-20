/* Diccionarios Mobipocket / Kindle (.mobi, .prc, .azw sin DRM).
 *
 * Un diccionario MOBI lleva su propio índice ordenado (índice "orth"): una
 * serie de registros INDX con las palabras y, para cada una, la posición y
 * la longitud de su artículo en el texto. Se busca directamente en él (búsqueda
 * binaria por registros y dentro de cada registro) y el artículo se descomprime
 * del registro de texto que lo contiene (PalmDOC o HUFF/CDIC).
 *
 * Si el orden del índice no coincide con nuestra comparación (colaciones
 * raras), se crea un índice auxiliar (.mdidx) a partir de él.
 *
 * Formato según KindleUnpack (mobi_index.py, mobi_dict.py, mobi_uncompress.py).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "md_internal.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

typedef struct {
    uint32_t start, count;      /* IDXT */
    uint32_t code, otype, oent, op2;
} indx_hdr;

/* registro del índice ya decodificado */
typedef struct {
    long rec;
    int n;
    char **keys;        /* claves plegadas */
    char **labels;      /* texto original */
    uint32_t *pos, *len;
    uint32_t *gofs;     /* grupos de flexión (etiqueta 42): gval[gofs[i]..gofs[i+1]) */
    uint32_t *gval;
    uint32_t ngval;
} irec;

typedef struct {
    FILE *fp;
    uint32_t nrec;
    uint32_t *roff;
    int comp;               /* 1 sin comprimir, 2 PalmDOC, 17480 HUFF */
    uint32_t ntext, recsize;
    uint16_t extra_flags;
    int cp1252;
    /* índice orth */
    uint32_t orth, icount;
    int cbc;
    int ntags;
    uint8_t tags[64][4];
    uint16_t *ordt2, *ordt1;
    uint32_t oent;
    uint32_t *rev;          /* (código << 16 | símbolo) ordenado, para codificar consultas */
    int otype, utf16;
    char **first;           /* primera clave plegada de cada registro (perezosa) */
    irec cache[2];
    int cnext;
    sidx *aux;
    /* disposición del texto (caché .mdpos): inicio real de cada registro de
     * texto y posiciones de los artículos, para los índices sin longitud */
    uint32_t *rstart;       /* ntext + 1 */
    uint32_t *epos, nepos;
    uint32_t firstimg;      /* primer registro de imagen (recindex 1) */
    uint32_t infl;          /* índice de flexiones (INFL) */
    int has_infl;           /* el índice orth tiene la etiqueta 42 */
    sidx *forms;            /* forma flexionada -> (registro << 32 | entrada) */
    /* texto */
    long tcache_rec[2];
    mbuf tcache[2];
    int tnext;
    /* HUFF/CDIC */
    uint32_t dict1[256];
    uint64_t mincode[33], maxcode[33];   /* pueden pasar de 32 bits */
    uint32_t nphr;
    mbuf *phr;
    uint8_t *phr_done;
} mobi;

static int read_rec(mobi *m, uint32_t i, mbuf *out)
{
    out->n = 0;
    if (i >= m->nrec) return -1;
    uint32_t a = m->roff[i], b = m->roff[i + 1];
    if (b < a || b - a > 16u << 20) return -1;
    if (mb_reserve(out, b - a) || fseeko(m->fp, a, SEEK_SET) || fread(out->p, 1, b - a, m->fp) != b - a) return -1;
    out->n = b - a;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* descompresión                                                           */
/* ---------------------------------------------------------------------- */

static size_t trailing(const uint8_t *r, size_t size, uint16_t flags)
{
    size_t num = 0;
    for (uint16_t tf = flags >> 1; tf; tf >>= 1) {
        if (!(tf & 1)) continue;
        size_t sz = size - num;
        uint32_t res = 0;
        int bp = 0;
        while (sz > 0) {
            uint8_t v = r[sz - 1];
            res |= (uint32_t)(v & 0x7F) << bp;
            bp += 7;
            sz--;
            if ((v & 0x80) || bp >= 28) break;
        }
        num += res;
        if (num > size) return size;
    }
    if ((flags & 1) && num < size) num += (r[size - num - 1] & 3) + 1;
    return num > size ? size : num;
}

static void palmdoc(const uint8_t *x, size_t n, mbuf *o)
{
    for (size_t i = 0; i < n;) {
        uint8_t c = x[i++];
        if (c == 0 || (c >= 9 && c <= 0x7F)) mb_putc(o, (char)c);
        else if (c <= 8) { size_t k = c; if (i + k > n) k = n - i; mb_put(o, x + i, k); i += k; }
        else if (c <= 0xBF) {
            if (i >= n) break;
            uint32_t mm = ((uint32_t)c << 8) | x[i++];
            size_t dist = (mm >> 3) & 0x7FF, len = (mm & 7) + 3;
            if (!dist || dist > o->n) break;
            for (size_t k = 0; k < len; k++) mb_putc(o, o->p[o->n - dist]);
        } else { mb_putc(o, ' '); mb_putc(o, (char)(c ^ 0x80)); }
    }
}

static int huff_load(mobi *m, uint32_t first, uint32_t count)
{
    mbuf r = {0};
    int rc = -1;
    if (count < 1 || read_rec(m, first, &r) || r.n < 24 || memcmp(r.p, "HUFF\0\0\0\x18", 8)) goto out;
    const uint8_t *h = (const uint8_t *)r.p;
    uint32_t o1 = be32(h + 8), o2 = be32(h + 12);
    if (o1 + 1024 > r.n || o2 + 256 > r.n) goto out;
    for (int i = 0; i < 256; i++) m->dict1[i] = be32(h + o1 + 4 * i);
    m->mincode[0] = 0; m->maxcode[0] = ((uint64_t)1 << 32) - 1;
    for (int l = 1; l <= 32; l++) {
        uint64_t mn = be32(h + o2 + 8 * (l - 1)), mx = be32(h + o2 + 8 * (l - 1) + 4);
        m->mincode[l] = mn << (32 - l);
        m->maxcode[l] = ((mx + 1) << (32 - l)) - 1;
    }
    for (uint32_t k = 1; k < count; k++) {
        if (read_rec(m, first + k, &r) || r.n < 16 || memcmp(r.p, "CDIC\0\0\0\x10", 8)) goto out;
        const uint8_t *c = (const uint8_t *)r.p;
        uint32_t phrases = be32(c + 8), bits = be32(c + 12);
        if (bits > 16) goto out;
        uint32_t n = phrases - m->nphr;
        if (n > (1u << bits)) n = 1u << bits;
        m->phr = realloc(m->phr, sizeof(mbuf) * (m->nphr + n));
        m->phr_done = realloc(m->phr_done, m->nphr + n);
        for (uint32_t i = 0; i < n; i++) {
            mbuf *p = &m->phr[m->nphr + i];
            memset(p, 0, sizeof *p);
            m->phr_done[m->nphr + i] = 0;
            if (16 + 2 * i + 2 > r.n) continue;
            uint32_t off = be16(c + 16 + 2 * i);
            if (16 + off + 2 > r.n) continue;
            uint32_t bl = be16(c + 16 + off);
            size_t l = bl & 0x7FFF;
            if (18 + off + l > r.n) l = r.n - 18 - off;
            mb_put(p, c + 18 + off, l);
            m->phr_done[m->nphr + i] = (bl & 0x8000) ? 1 : 0;
        }
        m->nphr += n;
    }
    rc = 0;
out:
    mb_free(&r);
    return rc;
}

static void huff_unpack(mobi *m, const uint8_t *data, size_t n, mbuf *o, int depth)
{
    if (depth > 32) return;
    long long bitsleft = (long long)n * 8;
    size_t pos = 0;
    uint64_t x = 0;
    for (int k = 0; k < 8; k++) x = (x << 8) | (pos + k < n ? data[pos + k] : 0);
    int bits = 32;
    for (;;) {
        if (bits <= 0) {
            pos += 4;
            x = 0;
            for (int k = 0; k < 8; k++) x = (x << 8) | (pos + k < n ? data[pos + k] : 0);
            bits += 32;
        }
        uint64_t code = (uint32_t)(x >> bits);
        uint32_t d = m->dict1[code >> 24];
        int codelen = d & 0x1F;
        uint64_t maxcode;
        if (!codelen) return;
        if (!(d & 0x80)) {
            while (codelen < 32 && code < m->mincode[codelen]) codelen++;
            maxcode = m->maxcode[codelen];
        } else maxcode = (((uint64_t)(d >> 8) + 1) << (32 - codelen)) - 1;
        bits -= codelen;
        bitsleft -= codelen;
        if (bitsleft < 0) break;
        if (maxcode < code) return;
        uint64_t r = (maxcode - code) >> (32 - codelen);
        if (r >= m->nphr) return;
        if (!m->phr_done[r]) {
            mbuf t = {0};
            m->phr_done[r] = 2; /* en curso: evita recursión infinita */
            huff_unpack(m, (uint8_t *)m->phr[r].p, m->phr[r].n, &t, depth + 1);
            mb_free(&m->phr[r]);
            m->phr[r] = t;
            m->phr_done[r] = 1;
        }
        mb_put(o, m->phr[r].p, m->phr[r].n);
    }
}

/* Texto descomprimido del registro de texto `i` (1..ntext). */
static mbuf *text_rec(mobi *m, uint32_t i)
{
    for (int k = 0; k < 2; k++) if (m->tcache_rec[k] == (long)i) return &m->tcache[k];
    int k = m->tnext;
    m->tnext ^= 1;
    mbuf raw = {0};
    m->tcache_rec[k] = -1;
    mbuf *o = &m->tcache[k];
    o->n = 0;
    if (i < 1 || i > m->ntext || read_rec(m, i, &raw)) { mb_free(&raw); return NULL; }
    size_t n = raw.n - trailing((uint8_t *)raw.p, raw.n, m->extra_flags);
    if (m->comp == 2) palmdoc((uint8_t *)raw.p, n, o);
    else if (m->comp == 17480) huff_unpack(m, (uint8_t *)raw.p, n, o, 0);
    else mb_put(o, raw.p, n);
    mb_free(&raw);
    m->tcache_rec[k] = (long)i;
    return o;
}

/* Registro de texto (1..ntext) que contiene la posición p y su inicio. */
static uint32_t rec_of(mobi *m, uint64_t p, uint64_t *start)
{
    if (!m->rstart) { *start = p - p % m->recsize; return (uint32_t)(p / m->recsize) + 1; }
    uint32_t lo = 0, hi = m->ntext;     /* rstart[i] = inicio del registro i+1 */
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (m->rstart[mid] <= p) lo = mid; else hi = mid;
    }
    *start = m->rstart[lo];
    return lo + 1;
}

static int read_text(mobi *m, uint64_t off, size_t len, mbuf *out)
{
    out->n = 0;
    while (out->n < len) {
        uint64_t p = off + out->n, rs;
        uint32_t r = rec_of(m, p, &rs);
        mbuf *t = text_rec(m, r);
        if (!t) return -1;
        size_t in = (size_t)(p - rs);
        if (in >= t->n) return out->n ? 0 : -1;   /* fin del texto */
        size_t take = t->n - in;
        if (take > len - out->n) take = len - out->n;
        mb_put(out, t->p + in, take);
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* índice orth                                                             */
/* ---------------------------------------------------------------------- */

static int parse_hdr(const mbuf *r, indx_hdr *h)
{
    const uint8_t *d = (const uint8_t *)r->p;
    if (r->n < 0xB8 || memcmp(d, "INDX", 4)) return -1;
    h->start = be32(d + 20);
    h->count = be32(d + 24);
    h->code = be32(d + 28);
    h->otype = be32(d + 0xA4);
    h->oent = be32(d + 0xA8);
    h->op2 = be32(d + 0xB0);
    if (h->start + 4 + 2 * (uint64_t)h->count > r->n) return -1;
    return 0;
}

static uint32_t varw(const uint8_t *d, size_t n, size_t *p)
{
    uint32_t v = 0;
    while (*p < n) {
        uint8_t b = d[(*p)++];
        v = (v << 7) | (b & 0x7F);
        if (b & 0x80) break;
    }
    return v;
}

/* Valores de la etiqueta `want` de una entrada INDX (getTagMap de KindleUnpack).
 * Devuelve cuántos hay (guarda hasta `max`). */
static int tag_get(int ntags, uint8_t (*tags)[4], int cbc, const uint8_t *d, size_t sp, size_t ep,
                   int want, uint32_t *vals, int max)
{
    struct { int tag, cnt; uint32_t bytes; int vpe; } tl[64];
    int nt = 0, ci = 0, got = 0;
    size_t ds = sp + (size_t)cbc;
    for (int i = 0; i < ntags && nt < 64; i++) {
        int tag = tags[i][0], vpe = tags[i][1], mask = tags[i][2], ef = tags[i][3];
        if (ef) { ci++; continue; }
        if (sp + (size_t)ci >= ep || !mask) break;
        int v = d[sp + ci] & mask;
        if (!v) continue;
        if (v == mask) {
            if (__builtin_popcount(mask) > 1) {
                uint32_t b = varw(d, ep, &ds);
                tl[nt].tag = tag; tl[nt].cnt = -1; tl[nt].bytes = b; tl[nt].vpe = vpe; nt++;
            } else { tl[nt].tag = tag; tl[nt].cnt = 1; tl[nt].vpe = vpe; nt++; }
        } else {
            int mm = mask;
            while (!(mm & 1)) { mm >>= 1; v >>= 1; }
            tl[nt].tag = tag; tl[nt].cnt = v; tl[nt].vpe = vpe; nt++;
        }
    }
    for (int i = 0; i < nt; i++) {
        if (tl[i].cnt >= 0) {
            for (int k = 0; k < tl[i].cnt * tl[i].vpe; k++) {
                uint32_t v = varw(d, ep, &ds);
                if (tl[i].tag == want && got < max) vals[got] = v;
                if (tl[i].tag == want) got++;
            }
        } else {
            size_t st = ds;
            while (ds - st < tl[i].bytes && ds < ep) {
                uint32_t v = varw(d, ep, &ds);
                if (tl[i].tag == want && got < max) vals[got] = v;
                if (tl[i].tag == want) got++;
            }
        }
    }
    return got;
}

/* Lee los valores de las etiquetas 1 (posición) y 2 (longitud). */
static void tag_values(mobi *m, const uint8_t *d, size_t sp, size_t ep, uint32_t *pos, uint32_t *len)
{
    *pos = *len = UINT32_MAX;
    uint32_t v;
    if (tag_get(m->ntags, m->tags, m->cbc, d, sp, ep, 1, &v, 1) > 0) *pos = v;
    if (tag_get(m->ntags, m->tags, m->cbc, d, sp, ep, 2, &v, 1) > 0) *len = v;
}

/* ---------------------------------------------------------------------- */
/* colación del Kindle (según kindling, ordt.rs: device_weight)            */
/* ---------------------------------------------------------------------- */

static int latin_base(uint32_t u)
{
    static const struct { uint16_t a, b; char c; } R[] = {
        {0xC0, 0xC5, 'a'}, {0xE0, 0xE5, 'a'}, {0x100, 0x105, 'a'}, {0x1CD, 0x1CE, 'a'}, {0x1DE, 0x1E1, 'a'}, {0x1FA, 0x1FC, 'a'}, {0x200, 0x203, 'a'}, {0x250, 0x250, 'a'},
        {0x180, 0x183, 'b'}, {0x253, 0x253, 'b'}, {0x299, 0x299, 'b'},
        {0xC7, 0xC7, 'c'}, {0xE7, 0xE7, 'c'}, {0x106, 0x10D, 'c'}, {0x187, 0x188, 'c'}, {0x255, 0x255, 'c'}, {0x297, 0x297, 'c'},
        {0xD0, 0xD0, 'd'}, {0xF0, 0xF0, 'd'}, {0x10E, 0x111, 'd'}, {0x189, 0x18C, 'd'}, {0x256, 0x257, 'd'},
        {0xC8, 0xCB, 'e'}, {0xE8, 0xEB, 'e'}, {0x112, 0x11B, 'e'}, {0x18E, 0x190, 'e'}, {0x1DD, 0x1DD, 'e'}, {0x204, 0x207, 'e'}, {0x258, 0x25A, 'e'},
        {0x191, 0x192, 'f'},
        {0x11C, 0x123, 'g'}, {0x193, 0x194, 'g'}, {0x1E4, 0x1E7, 'g'}, {0x1F4, 0x1F5, 'g'}, {0x260, 0x262, 'g'}, {0x29B, 0x29B, 'g'},
        {0x124, 0x127, 'h'}, {0x265, 0x267, 'h'}, {0x29C, 0x29C, 'h'},
        {0xCC, 0xCF, 'i'}, {0xEC, 0xEF, 'i'}, {0x128, 0x131, 'i'}, {0x197, 0x197, 'i'}, {0x1CF, 0x1D0, 'i'}, {0x208, 0x20B, 'i'}, {0x268, 0x268, 'i'}, {0x26A, 0x26A, 'i'},
        {0x134, 0x135, 'j'}, {0x1F0, 0x1F0, 'j'}, {0x25F, 0x25F, 'j'}, {0x284, 0x284, 'j'}, {0x29D, 0x29D, 'j'},
        {0x136, 0x138, 'k'}, {0x198, 0x199, 'k'}, {0x1E8, 0x1E9, 'k'}, {0x29E, 0x29E, 'k'},
        {0x139, 0x142, 'l'}, {0x19A, 0x19A, 'l'}, {0x26B, 0x26D, 'l'}, {0x29F, 0x29F, 'l'},
        {0xB5, 0xB5, 'm'}, {0x19C, 0x19C, 'm'}, {0x26F, 0x271, 'm'},
        {0xD1, 0xD1, 'n'}, {0xF1, 0xF1, 'n'}, {0x143, 0x14B, 'n'}, {0x19D, 0x19E, 'n'}, {0x272, 0x274, 'n'},
        {0xD2, 0xD6, 'o'}, {0xD8, 0xD8, 'o'}, {0xF2, 0xF6, 'o'}, {0xF8, 0xF8, 'o'}, {0x14C, 0x151, 'o'}, {0x186, 0x186, 'o'}, {0x19F, 0x1A1, 'o'},
        {0x1D1, 0x1D2, 'o'}, {0x1EA, 0x1ED, 'o'}, {0x1FE, 0x1FF, 'o'}, {0x20C, 0x20F, 'o'}, {0x254, 0x254, 'o'}, {0x275, 0x275, 'o'},
        {0x1A4, 0x1A5, 'p'}, {0x2A0, 0x2A0, 'q'},
        {0x154, 0x159, 'r'}, {0x1A6, 0x1A6, 'r'}, {0x210, 0x213, 'r'}, {0x279, 0x281, 'r'},
        {0x15A, 0x161, 's'}, {0x17F, 0x17F, 's'}, {0x282, 0x282, 's'},
        {0xDE, 0xDE, 't'}, {0xFE, 0xFE, 't'}, {0x162, 0x167, 't'}, {0x1AB, 0x1AE, 't'}, {0x287, 0x288, 't'},
        {0xD9, 0xDC, 'u'}, {0xF9, 0xFC, 'u'}, {0x168, 0x173, 'u'}, {0x1AF, 0x1B0, 'u'}, {0x1D3, 0x1DC, 'u'}, {0x214, 0x217, 'u'}, {0x289, 0x289, 'u'},
        {0x1B2, 0x1B2, 'v'}, {0x28B, 0x28C, 'v'},
        {0x174, 0x175, 'w'}, {0x28D, 0x28D, 'w'},
        {0xDD, 0xDD, 'y'}, {0xFD, 0xFD, 'y'}, {0xFF, 0xFF, 'y'}, {0x176, 0x178, 'y'}, {0x1B3, 0x1B4, 'y'}, {0x28E, 0x28F, 'y'},
        {0x179, 0x17E, 'z'}, {0x1B5, 0x1B6, 'z'}, {0x290, 0x291, 'z'},
    };
    if (u >= 'A' && u <= 'Z') return (int)u + 32;
    if (u >= 'a' && u <= 'z') return (int)u;
    for (size_t i = 0; i < sizeof R / sizeof *R; i++) if (u >= R[i].a && u <= R[i].b) return R[i].c;
    return 0;
}

static uint32_t device_weight(uint32_t u)
{
    static const char hw[] = "\xe3\x82\x92\xe3\x81\x81\xe3\x81\x83\xe3\x81\x85\xe3\x81\x87\xe3\x81\x89\xe3\x82\x83\xe3\x82\x85\xe3\x82\x87\xe3\x81\xa3"; /* sólo para documentar */
    (void)hw;
    if (u == 1 || u == 2) return 'o';
    if (u == 3 || u == 4) return 'a';
    if (u == 5) return 's';
    if (u >= 0x10 && u <= 0x14) { static const uint16_t v[] = {0x3042, 0x3044, 0x3046, 0x3048, 0x304A}; return v[u - 0x10]; }
    if (u == 0x20 || u == 0xA0 || u == 0x3000) return 0x20;
    if (u >= '0' && u <= '9') return u;
    if (u == 0xB9) return '1';
    if (u == 0xB2) return '2';
    if (u == 0xB3) return '3';
    if (u <= 0x2FF) return (uint32_t)latin_base(u);
    if ((u >= 0x115A && u <= 0x1160) || (u >= 0x11A3 && u <= 0x11A7)) return 0;
    if ((u >= 0x2000 && u <= 0x206F) || (u >= 0x2190 && u <= 0x23FF) || (u >= 0x2500 && u <= 0x27FF) || (u >= 0x2900 && u <= 0x2BFF)) return 0;
    if ((u >= 0x3001 && u <= 0x3040) || (u >= 0x3099 && u <= 0x309E) || u == 0x30A0 || (u >= 0x30FB && u <= 0x30FF) || u == 0x3164) return 0;
    if (u == 0x3094 || u == 0x3097) return 0x3046;
    if (u == 0x3095) return 0x3042;
    if (u == 0x3096) return 0x3044;
    if (u == 0x3098) return 0x3048;
    if (u == 0x309F) return 0x304A;
    if (u >= 0x30A1 && u <= 0x30F3) return u - 0x60;
    if (u == 0x30F4) return 0x3046;
    if (u == 0x30F5) return 0x304B;
    if (u == 0x30F6) return 0x3051;
    if (u >= 0x30F7 && u <= 0x30FA) return u - 0x68;
    if (u >= 0xAC00 && u <= 0xD7A3) return 0;
    if ((u >= 0xFF00 && u <= 0xFF0F) || (u >= 0xFF1A && u <= 0xFF20) || (u >= 0xFF3B && u <= 0xFF40) || (u >= 0xFF5B && u <= 0xFF65)) return 0;
    if (u == 0xFF70 || u == 0xFF9E || u == 0xFF9F) return 0;
    if (u >= 0xFF10 && u <= 0xFF19) return u - 0xFF10 + '0';
    if (u >= 0xFF21 && u <= 0xFF3A) return u - 0xFF21 + 'a';
    if (u >= 0xFF41 && u <= 0xFF5A) return u - 0xFF41 + 'a';
    if (u >= 0xFF66 && u <= 0xFF9D) {
        static const uint16_t k[] = {0x3092, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3083, 0x3085, 0x3087, 0x3063, 0x30FC, 0x3042, 0x3044, 0x3046,
            0x3048, 0x304A, 0x304B, 0x304D, 0x304F, 0x3051, 0x3053, 0x3055, 0x3057, 0x3059, 0x305B, 0x305D, 0x305F, 0x3061, 0x3064, 0x3066, 0x3068,
            0x306A, 0x306B, 0x306C, 0x306D, 0x306E, 0x306F, 0x3072, 0x3075, 0x3078, 0x307B, 0x307E, 0x307F, 0x3080, 0x3081, 0x3082, 0x3084, 0x3086,
            0x3088, 0x3089, 0x308A, 0x308B, 0x308C, 0x308D, 0x308F, 0x3093};
        return k[u - 0xFF66];
    }
    return u;
}

/* Añade un peso a la clave: 3 bytes de 7 bits (+1) que conservan el orden y no tienen ceros. */
static void key_put(mbuf *k, uint32_t w)
{
    mb_putc(k, (char)(((w >> 14) & 0x7F) + 1));
    mb_putc(k, (char)(((w >> 7) & 0x7F) + 1));
    mb_putc(k, (char)((w & 0x7F) + 1));
}

/* peso de un elemento de etiqueta (símbolo ORDT o unidad UTF-16) */
static void elem_weight(mobi *m, uint32_t v, mbuf *key)
{
    if (m->ordt1) {
        if (v < m->oent) { if (m->ordt1[v]) key_put(key, m->ordt1[v]); }
        else key_put(key, 0x10000 + v);
    } else {
        uint32_t w = device_weight(v);
        if (w) key_put(key, w);
    }
}

static int rev_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a >> 16, y = *(const uint32_t *)b >> 16;
    return (x > y) - (x < y);
}

/* unidad UTF-16 -> elemento de etiqueta (símbolo si está en la tabla ORDT) */
static uint32_t unit_elem(mobi *m, uint32_t u)
{
    if (!m->rev) return u;
    uint32_t k = u << 16;
    uint32_t *r = bsearch(&k, m->rev, m->oent, sizeof *m->rev, rev_cmp);
    return r ? (*r & 0xFFFF) : u;
}

/* marcadores de letras dobles: 1 Œ, 2 œ, 3 Æ, 4 æ, 5 ß (seguidos de su segunda letra) */
static const uint32_t pair_letter[6] = {0, 0x152, 0x153, 0xC6, 0xE6, 0xDF};

/* Etiqueta del índice -> texto UTF-8 para mostrar y clave de colación. */
static void label_parse(mobi *m, const uint8_t *t, size_t l, mbuf *disp, mbuf *key)
{
    disp->n = key->n = 0;
    uint32_t units[256];
    int nu = 0;
    if (m->ordt2 && m->otype == 1) {
        for (size_t i = 0; i < l && nu < 256; i++) {
            elem_weight(m, t[i], key);
            units[nu++] = t[i] < m->oent ? m->ordt2[t[i]] : t[i];
        }
    } else if (m->ordt2 || m->utf16) {
        for (size_t i = 0; i + 1 < l && nu < 256; i += 2) {
            uint32_t v = be16(t + i);
            elem_weight(m, v, key);
            units[nu++] = m->ordt2 && v < m->oent ? m->ordt2[v] : v;
        }
    } else {
        mbuf u = {0};
        if (m->cp1252) iconv_to_utf8("CP1252", (const char *)t, l, &u); else mb_put(&u, t, l);
        const char *p = u.p, *e = u.p + u.n;
        while (p < e && nu < 255) {
            uint32_t cp = utf8_next(&p, e);
            if (cp > 0xFFFF) { cp -= 0x10000; units[nu++] = 0xD800 + (cp >> 10); units[nu++] = 0xDC00 + (cp & 0x3FF); }
            else units[nu++] = cp;
        }
        mb_free(&u);
        for (int i = 0; i < nu; i++) elem_weight(m, units[i], key);
    }
    for (int i = 0; i < nu; i++) {
        uint32_t u = units[i];
        if (u >= 1 && u <= 5 && i + 1 < nu) { utf8_put(disp, pair_letter[u]); i++; continue; }
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < nu && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            utf8_put(disp, 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            i++;
            continue;
        }
        if (u) utf8_put(disp, u);
    }
    mb_cstr(disp);
    mb_cstr(key);
}

/* Palabra buscada -> clave, escribiendo ß æ œ como lo hace el Kindle. */
static void query_key(mobi *m, const char *w, int lower, mbuf *key)
{
    key->n = 0;
    const char *p = w, *e = w + strlen(w);
    while (p < e) {
        uint32_t cp = utf8_next(&p, e);
        if (lower) cp = md_lower(cp);
        uint32_t seq[3];
        int n = 0;
        switch (cp) {
        case 0x152: seq[0] = 1; seq[1] = 'E'; n = 2; break;
        case 0x153: seq[0] = 2; seq[1] = 'e'; n = 2; break;
        case 0xC6: seq[0] = 3; seq[1] = 'E'; n = 2; break;
        case 0xE6: seq[0] = 4; seq[1] = 'e'; n = 2; break;
        case 0xDF: seq[0] = 5; seq[1] = 's'; n = 2; break;
        case 0x133: seq[0] = 'i'; seq[1] = 'j'; n = 2; break;
        case 0x132: seq[0] = 'I'; seq[1] = 'J'; n = 2; break;
        case 1: case 2: case 3: case 4: case 5: n = 0; break;
        default:
            if (cp > 0xFFFF) { cp -= 0x10000; seq[0] = 0xD800 + (cp >> 10); seq[1] = 0xDC00 + (cp & 0x3FF); n = 2; }
            else { seq[0] = cp; n = 1; }
        }
        for (int i = 0; i < n; i++) elem_weight(m, m->ordt1 ? unit_elem(m, seq[i]) : seq[i], key);
    }
    mb_cstr(key);
}

static void irec_free(irec *r)
{
    for (int i = 0; i < r->n; i++) { free(r->keys[i]); free(r->labels[i]); }
    free(r->keys); free(r->labels); free(r->pos); free(r->len);
    free(r->gofs); free(r->gval);
    memset(r, 0, sizeof *r);
    r->rec = -1;
}

/* Decodifica el registro k (0..icount-1) del índice orth. */
static irec *load_irec(mobi *m, uint32_t k)
{
    for (int i = 0; i < 2; i++) if (m->cache[i].rec == (long)k) return &m->cache[i];
    irec *r = &m->cache[m->cnext];
    m->cnext ^= 1;
    irec_free(r);
    mbuf d = {0}, lab = {0}, key = {0};
    indx_hdr h;
    if (read_rec(m, m->orth + 1 + k, &d) || parse_hdr(&d, &h)) { mb_free(&d); return NULL; }
    const uint8_t *p = (const uint8_t *)d.p;
    r->keys = calloc(h.count + 1, sizeof(char *));
    r->labels = calloc(h.count + 1, sizeof(char *));
    r->pos = calloc(h.count + 1, 4);
    r->len = calloc(h.count + 1, 4);
    r->gofs = calloc(h.count + 2, 4);
    uint32_t gcap = 0;
    for (uint32_t j = 0; j < h.count; j++) {
        size_t sp = be16(p + h.start + 4 + 2 * j);
        size_t ep = j + 1 < h.count ? be16(p + h.start + 4 + 2 * (j + 1)) : h.start;
        if (sp >= d.n || ep > d.n || sp >= ep) continue;
        size_t l = p[sp];
        if (sp + 1 + l > ep) continue;
        label_parse(m, p + sp + 1, l, &lab, &key);
        uint32_t ps, ln;
        tag_values(m, p, sp + 1 + l, ep, &ps, &ln);
        if (ps == UINT32_MAX) continue;
        if (m->has_infl) {
            uint32_t gv[64];
            int ng = tag_get(m->ntags, m->tags, m->cbc, p, sp + 1 + l, ep, 42, gv, 64);
            if (ng > 64) ng = 64;
            if (r->ngval + (uint32_t)ng > gcap) { gcap = (gcap + (uint32_t)ng) * 2; r->gval = realloc(r->gval, 4 * gcap); }
            for (int g = 0; g < ng; g++) r->gval[r->ngval++] = gv[g];
        }
        r->gofs[r->n + 1] = r->ngval;
        r->labels[r->n] = md_strndup(lab.p, lab.n);
        r->keys[r->n] = md_strndup(key.p, key.n);
        r->pos[r->n] = ps;
        r->len[r->n] = ln == UINT32_MAX ? 0 : ln;
        r->n++;
        r->gofs[r->n] = r->ngval;
    }
    r->rec = (long)k;
    mb_free(&d); mb_free(&lab); mb_free(&key);
    if (!m->first[k]) m->first[k] = md_strdup(r->n ? r->keys[0] : "");
    return r;
}

static const char *first_key(mobi *m, uint32_t k)
{
    if (!m->first[k] && !load_irec(m, k)) m->first[k] = md_strdup("");
    return m->first[k];
}

/* ¿Coincide el orden del índice con nuestra comparación? (muestra) */
static int check_order(mobi *m)
{
    long inv = 0, tot = 0;
    uint32_t step = m->icount > 8 ? m->icount / 8 : 1;
    const char *prev_first = NULL;
    for (uint32_t k = 0; k < m->icount; k += step) {
        irec *r = load_irec(m, k);
        if (!r) return 0;
        for (int i = 0; i + 1 < r->n; i++, tot++) if (strcmp(r->keys[i], r->keys[i + 1]) > 0) inv++;
        if (prev_first && r->n && strcmp(prev_first, r->keys[0]) > 0) return 0;
        prev_first = first_key(m, k);
    }
    return tot == 0 || inv * 20 <= tot;
}

/* ---------------------------------------------------------------------- */

static const char *lcid_iso(uint32_t lcid)
{
    static const struct { uint8_t id; const char *iso; } T[] = {
        {0x01, "ar"}, {0x02, "bg"}, {0x03, "ca"}, {0x04, "zh"}, {0x05, "cs"}, {0x06, "da"}, {0x07, "de"}, {0x08, "el"},
        {0x09, "en"}, {0x0A, "es"}, {0x0B, "fi"}, {0x0C, "fr"}, {0x0D, "he"}, {0x0E, "hu"}, {0x0F, "is"}, {0x10, "it"},
        {0x11, "ja"}, {0x12, "ko"}, {0x13, "nl"}, {0x14, "no"}, {0x15, "pl"}, {0x16, "pt"}, {0x18, "ro"}, {0x19, "ru"},
        {0x1A, "hr"}, {0x1B, "sk"}, {0x1C, "sq"}, {0x1D, "sv"}, {0x1E, "th"}, {0x1F, "tr"}, {0x20, "ur"}, {0x21, "id"},
        {0x22, "uk"}, {0x23, "be"}, {0x24, "sl"}, {0x25, "et"}, {0x26, "lv"}, {0x27, "lt"}, {0x29, "fa"}, {0x2A, "vi"},
        {0x2B, "hy"}, {0x2D, "eu"}, {0x2F, "mk"}, {0x36, "af"}, {0x37, "ka"}, {0x38, "fo"}, {0x39, "hi"}, {0x3E, "ms"},
        {0x41, "sw"}, {0x56, "gl"},
    };
    uint32_t id = lcid & 0xFF;
    for (size_t i = 0; i < sizeof T / sizeof *T; i++) if (T[i].id == id) return T[i].iso;
    return NULL;
}

static int mobi_probe(const char *p)
{
    if (!ends_with_ci(p, ".mobi") && !ends_with_ci(p, ".prc") && !ends_with_ci(p, ".azw")) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint8_t h[68];
    int ok = fread(h, 1, 68, f) == 68 && !memcmp(h + 60, "BOOKMOBI", 8);
    fclose(f);
    return ok;
}

static void mobi_free(mobi *m)
{
    if (!m) return;
    if (m->fp) fclose(m->fp);
    free(m->roff);
    free(m->ordt2);
    free(m->rstart);
    free(m->epos);
    free(m->ordt1);
    free(m->rev);
    if (m->first) for (uint32_t i = 0; i < m->icount; i++) free(m->first[i]);
    free(m->first);
    irec_free(&m->cache[0]); irec_free(&m->cache[1]);
    sidx_close(m->aux);
    sidx_close(m->forms);
    mb_free(&m->tcache[0]); mb_free(&m->tcache[1]);
    for (uint32_t i = 0; i < m->nphr; i++) mb_free(&m->phr[i]);
    free(m->phr); free(m->phr_done);
    free(m);
}

/* nombre del índice auxiliar (solo si el orden no es fiable) */
static int aux_needed(const char *path, const char *cache_dir, char **out)
{
    for (int k = 0; k < 2; k++) {
        char *ip = sidx_path_for(path, cache_dir, k);
        if (sidx_is_fresh(ip, path)) { if (out) *out = ip; else free(ip); return 0; }
        free(ip);
    }
    return 1;
}

/* ---------------------------------------------------------------------- */
/* caché de disposición (.mdpos)                                           */
/*   "MDMOBIP1" tamaño(8) mtime(8) ntext(4) nepos(4) rstart[ntext+1] epos   */
/* ---------------------------------------------------------------------- */

static char *pos_path(const char *path, const char *cache_dir, int k)
{
    char *ip = sidx_path_for(path, cache_dir, k);
    size_t n = strlen(ip);
    if (n > 6 && !strcmp(ip + n - 6, ".mdidx")) ip[n - 6] = 0;
    mbuf b = {0};
    mb_puts(&b, ip);
    mb_puts(&b, ".mdpos");
    free(ip);
    return mb_steal(&b);
}

static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t r32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int pos_load(mobi *m, const char *pp, const char *src)
{
    struct stat st;
    if (stat(src, &st)) return -1;
    FILE *f = fopen(pp, "rb");
    if (!f) return -1;
    uint8_t h[32];
    int rc = -1;
    if (fread(h, 1, 32, f) == 32 && !memcmp(h, "MDMOBIP1", 8)) {
        uint64_t sz = 0, mt = 0;
        for (int i = 7; i >= 0; i--) { sz = (sz << 8) | h[8 + i]; mt = (mt << 8) | h[16 + i]; }
        uint32_t nt = r32(h + 24), ne = r32(h + 28);
        if (sz == (uint64_t)st.st_size && mt == (uint64_t)st.st_mtime && nt == m->ntext && ne < (1u << 26)) {
            uint8_t *b = malloc(4 * ((size_t)nt + 1 + ne) + 1);
            if (b && fread(b, 4, (size_t)nt + 1 + ne, f) == (size_t)nt + 1 + ne) {
                m->rstart = malloc(4 * ((size_t)nt + 1));
                m->epos = ne ? malloc(4 * (size_t)ne) : NULL;
                for (uint32_t i = 0; i <= nt; i++) m->rstart[i] = r32(b + 4 * i);
                for (uint32_t i = 0; i < ne; i++) m->epos[i] = r32(b + 4 * ((size_t)nt + 1 + i));
                m->nepos = ne;
                rc = 0;
            }
            free(b);
        }
    }
    fclose(f);
    return rc;
}

static int u32cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Recorre el texto (tamaño real de cada registro) y el índice (posiciones). */
static int pos_build(mobi *m, const char *pp, const char *src)
{
    m->rstart = malloc(4 * ((size_t)m->ntext + 1));
    uint64_t acc = 0;
    for (uint32_t t = 1; t <= m->ntext; t++) {
        m->rstart[t - 1] = (uint32_t)acc;
        mbuf *x = text_rec(m, t);
        if (!x) return -1;
        acc += x->n;
        if (acc > 0xFFFFFFFFu) return -1;
    }
    m->rstart[m->ntext] = (uint32_t)acc;
    /* posiciones de los artículos: solo si a alguno le falta la longitud */
    size_t cap = 0;
    int need = 0;
    for (uint32_t k = 0; k < m->icount; k++) {
        irec *ir = load_irec(m, k);
        if (!ir) continue;
        for (int i = 0; i < ir->n; i++) {
            if (!ir->len[i]) need = 1;
            if (m->nepos == cap) { cap = cap ? cap * 2 : 65536; m->epos = realloc(m->epos, 4 * cap); }
            m->epos[m->nepos++] = ir->pos[i];
        }
    }
    if (!need) { free(m->epos); m->epos = NULL; m->nepos = 0; }
    else {
        qsort(m->epos, m->nepos, 4, u32cmp);
        uint32_t j = 0;
        for (uint32_t i = 0; i < m->nepos; i++) if (!j || m->epos[i] != m->epos[j - 1]) m->epos[j++] = m->epos[i];
        m->nepos = j;
    }
    /* se guarda en un archivo temporal y se renombra: nunca queda a medias */
    struct stat st;
    if (stat(src, &st)) return 0;
    mbuf tmp = {0};
    mb_puts(&tmp, pp);
    mb_puts(&tmp, ".tmp");
    FILE *f = fopen(mb_cstr(&tmp), "wb");
    if (f) {
        uint8_t h[32];
        memcpy(h, "MDMOBIP1", 8);
        for (int i = 0; i < 8; i++) { h[8 + i] = (uint8_t)((uint64_t)st.st_size >> (8 * i)); h[16 + i] = (uint8_t)((uint64_t)st.st_mtime >> (8 * i)); }
        w32(h + 24, m->ntext);
        w32(h + 28, m->nepos);
        int ok = fwrite(h, 1, 32, f) == 32;
        uint8_t b[4];
        for (uint32_t i = 0; ok && i <= m->ntext; i++) { w32(b, m->rstart[i]); ok = fwrite(b, 1, 4, f) == 4; }
        for (uint32_t i = 0; ok && i < m->nepos; i++) { w32(b, m->epos[i]); ok = fwrite(b, 1, 4, f) == 4; }
        if (fclose(f) || !ok || rename(tmp.p, pp)) remove(tmp.p);
    }
    mb_free(&tmp);
    return 0; /* sin caché también funciona: solo se repetirá al abrir */
}

/* ---------------------------------------------------------------------- */
/* flexiones (índice INFL + reglas, según KindleUnpack mobi_dict.py)       */
/* ---------------------------------------------------------------------- */

/* applyInflectionRule: aplica una regla de Kindle a la palabra base */
static int apply_rule(const char *base, const uint8_t *rule, size_t rn, mbuf *out)
{
    out->n = 0;
    mb_puts(out, base);
    int mode = -1;
    size_t pos = out->n;
    for (size_t i = 0; i < rn; i++) {
        uint8_t a = rule[i];
        if (a >= 0x0A && a <= 0x13) {
            if (mode != 2 && mode != 3) { mode = 2; pos = out->n; }
            if ((size_t)(a - 0x0A) > pos) return -1;
            pos -= (size_t)(a - 0x0A);
        } else if (a > 0x13) {
            if (mode == 1 || mode == 2) {
                mb_putc(out, 0);
                memmove(out->p + pos + 1, out->p + pos, out->n - 1 - pos);
                out->p[pos] = (char)a;
                if (mode == 1) pos++;
            } else if (mode == 3) {
                if (!pos || (uint8_t)out->p[pos - 1] != a) return -1;
                pos--;
                memmove(out->p + pos, out->p + pos + 1, out->n - pos - 1);
                out->n--;
            } else if (mode == 4) {
                if (pos >= out->n || (uint8_t)out->p[pos] != a) return -1;
                memmove(out->p + pos, out->p + pos + 1, out->n - pos - 1);
                out->n--;
            } else return -1;
        } else if (a == 1 || a == 4) {
            if (mode != 1 && mode != 4) pos = 0;
            mode = a;
        } else if (a == 2 || a == 3) {
            if (mode != 2 && mode != 3) pos = out->n;
            mode = a;
        } else return -1;
    }
    mb_cstr(out);
    return 0;
}

typedef struct {
    uint32_t n;
    uint32_t *loff, *llen;      /* etiqueta (regla) dentro de buf */
    uint32_t *roff;             /* reglas (etiqueta 0x1a): rval[roff[i]..roff[i+1]) */
    uint32_t *rval, nrval;
    mbuf buf;
} infl_tab;

static void infl_free(infl_tab *t)
{
    free(t->loff); free(t->llen); free(t->roff); free(t->rval);
    mb_free(&t->buf);
}

static int infl_load(mobi *m, infl_tab *t)
{
    mbuf r = {0};
    indx_hdr h;
    memset(t, 0, sizeof *t);
    if (read_rec(m, m->infl, &r) || parse_hdr(&r, &h)) { mb_free(&r); return -1; }
    const uint8_t *ix = (const uint8_t *)r.p;
    uint8_t tags[64][4];
    int ntags = 0;
    uint32_t tl = be32(ix + 4);
    if (tl + 12 > r.n || memcmp(ix + tl, "TAGX", 4)) { mb_free(&r); return -1; }
    uint32_t tlen = be32(ix + tl + 4);
    int cbc = (int)be32(ix + tl + 8);
    for (uint32_t i = 0; 12 + 4 * i + 4 <= tlen && tl + 12 + 4 * i + 4 <= r.n && i < 64; i++) { memcpy(tags[i], ix + tl + 12 + 4 * i, 4); ntags++; }
    uint32_t nrecs = h.count, cap = 0, rcap = 0;
    for (uint32_t k = 0; k < nrecs; k++) {
        indx_hdr dh;
        if (read_rec(m, m->infl + 1 + k, &r) || parse_hdr(&r, &dh)) break;
        const uint8_t *p = (const uint8_t *)r.p;
        for (uint32_t j = 0; j < dh.count; j++) {
            size_t sp = be16(p + dh.start + 4 + 2 * j);
            size_t ep = j + 1 < dh.count ? be16(p + dh.start + 4 + 2 * (j + 1)) : dh.start;
            if (t->n + 1 >= cap) {
                cap = cap ? cap * 2 : 4096;
                t->loff = realloc(t->loff, 4 * cap); t->llen = realloc(t->llen, 4 * cap); t->roff = realloc(t->roff, 4 * (cap + 1));
            }
            if (!t->n) t->roff[0] = 0;
            t->loff[t->n] = (uint32_t)t->buf.n;
            t->llen[t->n] = 0;
            if (sp < ep && ep <= r.n && sp + 1 + p[sp] <= ep) {
                size_t l = p[sp];
                mb_put(&t->buf, p + sp + 1, l);
                t->llen[t->n] = (uint32_t)l;
                uint32_t v[64];
                int nv = tag_get(ntags, tags, cbc, p, sp + 1 + l, ep, 0x1A, v, 64);
                if (nv > 64) nv = 64;
                if (t->nrval + (uint32_t)nv > rcap) { rcap = (rcap + (uint32_t)nv) * 2; t->rval = realloc(t->rval, 4 * rcap); }
                for (int q = 0; q < nv; q++) t->rval[t->nrval++] = v[q];
            }
            t->n++;
            t->roff[t->n] = t->nrval;
        }
    }
    mb_free(&r);
    return t->n ? 0 : -1;
}

static char *forms_path(const char *path, const char *cache_dir, int k)
{
    char *pp = pos_path(path, cache_dir, k);
    size_t n = strlen(pp);
    if (n > 6) strcpy(pp + n - 6, ".mdinf");
    return pp;
}

/* Índice auxiliar de formas flexionadas -> entrada del índice orth. */
static int forms_build(mobi *m, md_dict *d, const char *fp, char *err, int errlen)
{
    infl_tab t;
    if (infl_load(m, &t)) { infl_free(&t); return -1; }
    sidx_builder *b = sidx_new();
    mbuf f = {0};
    for (uint32_t k = 0; k < m->icount; k++) {
        irec *ir = load_irec(m, k);
        if (!ir) continue;
        for (int i = 0; i < ir->n; i++) {
            for (uint32_t g = ir->gofs[i]; g < ir->gofs[i + 1]; g++) {
                uint32_t gi = ir->gval[g];
                if (gi >= t.n) continue;
                for (uint32_t q = t.roff[gi]; q < t.roff[gi + 1]; q++) {
                    uint32_t ri = t.rval[q];
                    if (ri >= t.n || !t.llen[ri]) continue;
                    if (apply_rule(ir->labels[i], (uint8_t *)t.buf.p + t.loff[ri], t.llen[ri], &f) || !f.n) continue;
                    if (!strcmp(f.p, ir->labels[i])) continue;
                    sidx_add(b, f.p, f.n, ((uint64_t)k << 32) | (uint32_t)i, 0);
                }
            }
        }
    }
    int rc = sidx_write(b, fp, d->path, d->name, NULL, NULL, err, errlen);
    sidx_free(b);
    mb_free(&f);
    infl_free(&t);
    return rc;
}

static int mobi_needs_index(const char *path, const char *cache_dir)
{
    for (int k = 0; k < 2; k++) {
        char *pp = pos_path(path, cache_dir, k);
        FILE *f = fopen(pp, "rb");
        free(pp);
        if (f) { fclose(f); return 0; }
    }
    return 1;
}

static int mobi_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    mobi *m = calloc(1, sizeof *m);
    m->tcache_rec[0] = m->tcache_rec[1] = -1;
    m->cache[0].rec = m->cache[1].rec = -1;
    mbuf r = {0}, t = {0};
    m->fp = fopen(d->path, "rb");
    uint8_t h[78];
    if (!m->fp || fread(h, 1, 78, m->fp) != 78) { md_seterr(err, errlen, "no se puede abrir %s", d->path); goto fail; }
    m->nrec = be16(h + 76);
    m->roff = calloc(m->nrec + 1, 4);
    uint8_t *ro = malloc((size_t)m->nrec * 8 + 1);
    if (!ro || fread(ro, 8, m->nrec, m->fp) != m->nrec) { free(ro); md_seterr(err, errlen, "MOBI dañado: %s", d->path); goto fail; }
    for (uint32_t i = 0; i < m->nrec; i++) m->roff[i] = be32(ro + 8 * i);
    free(ro);
    fseeko(m->fp, 0, SEEK_END);
    m->roff[m->nrec] = (uint32_t)ftello(m->fp);
    if (read_rec(m, 0, &r) || r.n < 0x84 || memcmp(r.p + 16, "MOBI", 4)) { md_seterr(err, errlen, "MOBI sin cabecera: %s", d->path); goto fail; }
    const uint8_t *r0 = (const uint8_t *)r.p;
    m->comp = (int)be16(r0);
    m->ntext = be16(r0 + 8);
    m->recsize = be16(r0 + 10);
    if (be16(r0 + 12)) { md_seterr(err, errlen, "el MOBI tiene DRM: %s", d->path); goto fail; }
    if (m->comp != 1 && m->comp != 2 && m->comp != 17480) { md_seterr(err, errlen, "compresión MOBI desconocida (%d): %s", m->comp, d->path); goto fail; }
    if (!m->recsize) m->recsize = 4096;
    uint32_t hl = be32(r0 + 20), ver = be32(r0 + 0x24);
    m->cp1252 = be32(r0 + 0x1C) == 1252;
    m->orth = be32(r0 + 0x28);
    m->infl = be32(r0 + 0x2C);
    m->firstimg = r.n >= 0x70 ? be32(r0 + 0x6C) : 0xFFFFFFFF;
    if (hl >= 0xE4 && ver >= 5 && r.n >= 0xF4) m->extra_flags = (uint16_t)be16(r0 + 0xF2);
    if (m->orth == 0xFFFFFFFF || m->orth >= m->nrec) { md_seterr(err, errlen, "el MOBI no es un diccionario (sin índice de palabras): %s", d->path); goto fail; }
    uint32_t fno = be32(r0 + 0x54), fnl = be32(r0 + 0x58);
    if (fnl && fno + fnl <= r.n) {
        if (m->cp1252) { t.n = 0; iconv_to_utf8("CP1252", r.p + fno, fnl, &t); }
        else { t.n = 0; mb_put(&t, r.p + fno, fnl); }
        if (t.n) { free(d->name); d->name = md_strndup(t.p, t.n); }
    }
    const char *li = lcid_iso(be32(r0 + 0x60)), *lo = lcid_iso(be32(r0 + 0x64));
    if (li) d->lang_from = md_strdup(li);
    if (lo) d->lang_to = md_strdup(lo);
    if (m->comp == 17480 && huff_load(m, be32(r0 + 0x70), be32(r0 + 0x74))) { md_seterr(err, errlen, "tablas HUFF dañadas: %s", d->path); goto fail; }

    /* registro de metadatos del índice: TAGX y ORDT */
    indx_hdr ih;
    if (read_rec(m, m->orth, &r) || parse_hdr(&r, &ih)) { md_seterr(err, errlen, "índice MOBI dañado: %s", d->path); goto fail; }
    const uint8_t *ix = (const uint8_t *)r.p;
    m->icount = ih.count;
    d->word_count = (long)be32(ix + 36);
    uint32_t tl = be32(ix + 4);
    if (tl + 12 > r.n || memcmp(ix + tl, "TAGX", 4)) { md_seterr(err, errlen, "índice MOBI sin TAGX: %s", d->path); goto fail; }
    uint32_t tlen = be32(ix + tl + 4);
    m->cbc = (int)be32(ix + tl + 8);
    for (uint32_t i = 0; 12 + 4 * i + 4 <= tlen && tl + 12 + 4 * i + 4 <= r.n && i < 64; i++) {
        memcpy(m->tags[i], ix + tl + 12 + 4 * i, 4);
        m->ntags++;
    }
    m->utf16 = ih.code == 65002;
    if (m->infl != 0xFFFFFFFF && m->infl < m->nrec)
        for (int i = 0; i < m->ntags; i++) if (m->tags[i][0] == 42 && !m->tags[i][3]) m->has_infl = 1;
    if (ih.oent > 0 && ih.oent < 65536 && ih.op2 + 4 + 2 * ih.oent <= r.n) {
        m->oent = ih.oent;
        m->otype = (int)ih.otype;
        m->ordt2 = malloc(2 * ih.oent);
        for (uint32_t i = 0; i < ih.oent; i++) m->ordt2[i] = (uint16_t)be16(ix + ih.op2 + 4 + 2 * i);
        uint32_t op1 = be32(ix + 0xAC);
        int w1 = (ih.otype == 0 && ih.op2 >= op1 + 4 + 2 * ih.oent) ? 2 : 1;
        if (op1 + 4 + (uint64_t)w1 * ih.oent <= r.n && !memcmp(ix + op1, "ORDT", 4)) {
            m->ordt1 = malloc(2 * ih.oent);
            for (uint32_t i = 0; i < ih.oent; i++) m->ordt1[i] = (uint16_t)(w1 == 2 ? be16(ix + op1 + 4 + 2 * i) : ix[op1 + 4 + i]);
            m->rev = malloc(4 * ih.oent);
            for (uint32_t i = 0; i < ih.oent; i++) m->rev[i] = ((uint32_t)m->ordt2[i] << 16) | i;
            qsort(m->rev, ih.oent, 4, rev_cmp);
        }
    }
    m->first = calloc(m->icount + 1, sizeof(char *));

    /* disposición del texto: de la caché o recorriéndolo una vez */
    {
        int ok = 0;
        for (int k = 0; k < 2 && !ok; k++) {
            char *pp = pos_path(d->path, cache_dir, k);
            ok = !pos_load(m, pp, d->path);
            if (!ok) { free(m->rstart); m->rstart = NULL; free(m->epos); m->epos = NULL; m->nepos = 0; }
            free(pp);
        }
        if (!ok) {
            char *pp = pos_path(d->path, cache_dir, 1);
            int rc = pos_build(m, pp, d->path);
            free(pp);
            if (rc) { md_seterr(err, errlen, "texto MOBI dañado: %s", d->path); goto fail; }
        }
    }

    /* formas flexionadas (plurales, tiempos verbales...) */
    if (m->has_infl) {
        char *fp = NULL;
        for (int k = 0; k < 2 && !fp; k++) {
            char *p2 = forms_path(d->path, cache_dir, k);
            if (sidx_is_fresh(p2, d->path)) fp = p2; else free(p2);
        }
        if (!fp) {
            fp = forms_path(d->path, cache_dir, 1);
            char e2[256];
            if (forms_build(m, d, fp, e2, sizeof e2)) { free(fp); fp = NULL; }
        }
        if (fp) {
            long wc = 0;
            m->forms = sidx_open(fp, NULL, NULL, NULL, &wc, NULL, 0);
            free(fp);
        }
    }

    /* orden: si no es fiable, índice auxiliar construido a partir del propio índice */
    char *ip = NULL;
    if (!aux_needed(d->path, cache_dir, &ip) || !check_order(m)) {
        if (!ip) {
            ip = sidx_path_for(d->path, cache_dir, 1);
            sidx_builder *b = sidx_new();
            for (uint32_t k = 0; k < m->icount; k++) {
                irec *ir = load_irec(m, k);
                if (!ir) continue;
                for (int i = 0; i < ir->n; i++) sidx_add(b, ir->labels[i], strlen(ir->labels[i]), ir->pos[i], ir->len[i]);
            }
            int rc = sidx_write(b, ip, d->path, d->name, d->lang_from, d->lang_to, err, errlen);
            sidx_free(b);
            if (rc) { free(ip); goto fail; }
        }
        m->aux = sidx_open(ip, NULL, NULL, NULL, &d->word_count, err, errlen);
        free(ip);
        if (!m->aux) goto fail;
    }
    mb_free(&r); mb_free(&t);
    d->is_html = 1;
    d->priv = m;
    return 0;
fail:
    mb_free(&r); mb_free(&t);
    mobi_free(m);
    return -1;
}

static void mobi_close(md_dict *d) { mobi_free(d->priv); }

static void base64(const uint8_t *d, size_t n, mbuf *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        mb_putc(out, T[(v >> 18) & 63]);
        mb_putc(out, T[(v >> 12) & 63]);
        mb_putc(out, i + 1 < n ? T[(v >> 6) & 63] : '=');
        mb_putc(out, i + 2 < n ? T[v & 63] : '=');
    }
}

/* Copia un GIF sin sus extensiones de aplicación (XMP, "MBPW" de Mobipocket):
 * MuPDF rechaza las que no miden 11 bytes y deja de dibujar la definición. */
static void gif_strip_app(const uint8_t *d, size_t n, mbuf *out)
{
    size_t p = 13;
    if (n < 13) { mb_put(out, d, n); return; }
    if (d[10] & 0x80) p += 3u * (2u << (d[10] & 7));
    if (p > n) { mb_put(out, d, n); return; }
    mb_put(out, d, p);
    while (p < n) {
        if (d[p] == 0x21 && p + 1 < n) {
            size_t q = p + 2;
            while (q < n && d[q]) q += 1u + d[q];
            q++;
            if (q > n) break;
            if (d[p + 1] != 0xFF) mb_put(out, d + p, q - p);
            p = q;
        } else {
            mb_put(out, d + p, n - p);   /* imagen(es) y final: tal cual */
            break;
        }
    }
}

/* <img recindex="N"> (o hirecindex/lorecindex): imagen guardada en el propio
 * MOBI. Se incrusta como data: URI; las pequeñas (p. ej. símbolos fonéticos)
 * se ajustan a la altura del texto. */
static int embed_img(mobi *m, const char *tag, size_t tn, mbuf *out)
{
    static const char *const names[] = {"hirecindex=\"", "recindex=\"", "lorecindex=\""};
    long idx = -1;
    for (int k = 0; k < 3 && idx < 0; k++) {
        size_t nl = strlen(names[k]);
        for (size_t i = 0; i + nl < tn; i++)
            if (!memcmp(tag + i, names[k], nl) && (i == 0 || tag[i - 1] == ' ')) { idx = strtol(tag + i + nl, NULL, 10); break; }
    }
    if (idx < 1 || m->firstimg == 0xFFFFFFFF || m->firstimg + (uint32_t)idx - 1 >= m->nrec) return 0;
    mbuf im = {0};
    if (read_rec(m, m->firstimg + (uint32_t)idx - 1, &im) || im.n < 8) { mb_free(&im); return 0; }
    const uint8_t *d = (const uint8_t *)im.p;
    const char *mime = !memcmp(d, "GIF8", 4) ? "image/gif" : (d[0] == 0xFF && d[1] == 0xD8) ? "image/jpeg"
                     : !memcmp(d, "\x89PNG", 4) ? "image/png" : !memcmp(d, "BM", 2) ? "image/bmp" : NULL;
    if (!mime) { mb_free(&im); return 0; }
    int h = 0;
    if (!memcmp(d, "GIF8", 4)) h = d[8] | (d[9] << 8);
    else if (!memcmp(d, "\x89PNG", 4) && im.n > 24) h = (int)be32(d + 20);
    mb_puts(out, "<img src=\"data:");
    mb_puts(out, mime);
    mb_puts(out, ";base64,");
    if (!memcmp(d, "GIF8", 4)) {
        mbuf g = {0};
        gif_strip_app(d, im.n, &g);
        base64((uint8_t *)g.p, g.n, out);
        mb_free(&g);
    } else base64(d, im.n, out);
    mb_puts(out, h && h <= 48 ? "\" style=\"height:1.2em;vertical-align:middle\"/>" : "\"/>");
    mb_free(&im);
    return 1;
}

/* quita etiquetas propias de Mobipocket que no son HTML e incrusta imágenes */
static void clean(mobi *m, const char *s, size_t n, mbuf *out)
{
    out->n = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '<' && i + 5 < n && (!memcmp(s + i + 1, "mbp:", 4) || !memcmp(s + i + 1, "/mbp:", 5) ||
                                         !memcmp(s + i + 1, "idx:", 4) || !memcmp(s + i + 1, "/idx:", 5))) {
            const char *g = memchr(s + i, '>', n - i);
            if (!g) break;
            i = (size_t)(g - s) + 1;
            continue;
        }
        if (s[i] == '<' && i + 4 < n && (s[i + 1] | 32) == 'i' && (s[i + 2] | 32) == 'm' && (s[i + 3] | 32) == 'g' && s[i + 4] == ' ') {
            const char *g = memchr(s + i, '>', n - i);
            if (g && embed_img(m, s + i, (size_t)(g - (s + i)), out)) { i = (size_t)(g - s) + 1; continue; }
        }
        mb_putc(out, s[i++]);
    }
    mb_cstr(out);
}

static void add_result(mobi *m, const char *word, uint32_t pos, uint32_t len, md_results *r)
{
    mbuf t = {0}, u = {0}, h = {0};
    if (!len) {
        /* sin longitud en el índice: hasta el artículo siguiente en el texto */
        uint64_t end = m->rstart ? m->rstart[m->ntext] : (uint64_t)pos + 4096;
        if (m->epos) {
            uint32_t lo = 0, hi = m->nepos;
            while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (m->epos[mid] <= pos) lo = mid + 1; else hi = mid; }
            if (lo < m->nepos) end = m->epos[lo];
        }
        len = end > pos ? (uint32_t)(end - pos) : 0;
        if (len > (256u << 10)) len = 256u << 10;
        if (!len) len = 4096;
    }
    if (!read_text(m, pos, len, &t)) {
        if (m->cp1252) { iconv_to_utf8("CP1252", t.p, t.n, &u); clean(m, u.p, u.n, &h); }
        else clean(m, t.p, t.n, &h);
        if (!md_results_has(r, word, h.p)) md_results_add(r, word, strlen(word), h.p, h.n);
    }
    mb_free(&t); mb_free(&u); mb_free(&h);
}

static int lookup_main(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    mobi *m = d->priv;
    if (m->aux) {
        sidx_hit hits[64];
        int n = sidx_find(m->aux, word, fuzzy, hits, max < 64 ? max : 64);
        for (int i = 0; i < n; i++) {
            add_result(m, hits[i].word, (uint32_t)hits[i].off, hits[i].len, r);
            free(hits[i].word);
        }
        return r->n;
    }
    mbuf key = {0};
  for (int pass = 0; pass < 2 && !r->n; pass++) {
    query_key(m, word, pass, &key);
    if (!key.n) break;
    /* último registro cuya primera clave es <= la buscada */
    long lo = 0, hi = (long)m->icount - 1, k = 0;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (strcmp(first_key(m, (uint32_t)mid), key.p) <= 0) { k = mid; lo = mid + 1; } else hi = mid - 1;
    }
    /* la palabra puede repetirse al final del registro anterior */
    long from = k > 0 ? k - 1 : 0;
    for (long rk = from; rk < (long)m->icount && r->n < max; rk++) {
        irec *ir = load_irec(m, (uint32_t)rk);
        if (!ir) break;
        int past = 0;
        for (int i = 0; i < ir->n && r->n < max; i++) {
            int c = fuzzy ? strncmp(ir->keys[i], key.p, key.n) : strcmp(ir->keys[i], key.p);
            if (!c) add_result(m, ir->labels[i], ir->pos[i], ir->len[i], r);
            else if (c > 0 && rk > k) { past = 1; break; }
        }
        if (past || rk > k + 1) break;
        /* si el registro acaba en la clave, la siguiente puede seguir con ella */
        if (rk >= k && ir->n && (fuzzy ? strncmp(ir->keys[ir->n - 1], key.p, key.n) : strcmp(ir->keys[ir->n - 1], key.p)) > 0) break;
    }
  }
    mb_free(&key);
    return r->n;
}

static int mobi_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    mobi *m = d->priv;
    lookup_main(d, word, fuzzy, max, r);
    /* la palabra como forma flexionada de otra (houses -> house) */
    if (m->forms && r->n < max) {
        sidx_hit hits[32];
        int n = sidx_find(m->forms, word, 0, hits, 32);
        for (int i = 0; i < n; i++) {
            uint32_t k = (uint32_t)(hits[i].off >> 32), e = (uint32_t)hits[i].off;
            irec *ir = r->n < max ? load_irec(m, k) : NULL;
            if (ir && e < (uint32_t)ir->n) add_result(m, ir->labels[e], ir->pos[e], ir->len[e], r);
            free(hits[i].word);
        }
    }
    return r->n;
}

const md_format_t md_fmt_mobi = {"mobi", mobi_probe, mobi_needs_index, mobi_open, mobi_close, mobi_lookup};
