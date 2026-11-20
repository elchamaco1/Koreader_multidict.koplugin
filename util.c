/* Utilidades comunes: búferes, lectura de archivos (dictzip/gzip), texto. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "md_internal.h"
#include "miniz/miniz.h"

/* ====================================================================== */
/* mbuf                                                                    */
/* ====================================================================== */

int mb_reserve(mbuf *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + extra + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

void mb_put(mbuf *b, const void *s, size_t n)
{
    if (!n) return;
    if (mb_reserve(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

void mb_puts(mbuf *b, const char *s) { mb_put(b, s, strlen(s)); }

void mb_putc(mbuf *b, char c)
{
    if (mb_reserve(b, 1)) return;
    b->p[b->n++] = c;
}

void mb_printf(mbuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) mb_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

char *mb_cstr(mbuf *b)
{
    if (mb_reserve(b, 1)) return (char *)"";
    b->p[b->n] = 0;
    return b->p;
}

char *mb_steal(mbuf *b)
{
    mb_cstr(b);
    char *p = b->p;
    if (!p) p = md_strdup("");
    b->p = NULL;
    b->n = b->cap = 0;
    return p;
}

void mb_free(mbuf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void mb_html_escape(mbuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '<': mb_puts(b, "&lt;"); break;
        case '>': mb_puts(b, "&gt;"); break;
        case '&': mb_puts(b, "&amp;"); break;
        case '"': mb_puts(b, "&quot;"); break;
        default: mb_putc(b, s[i]);
        }
    }
}

/* ====================================================================== */
/* cadenas varias                                                          */
/* ====================================================================== */

char *md_strdup(const char *s) { return md_strndup(s, strlen(s)); }

char *md_strndup(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

int ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return 0;
    return strcasecmp(s + n - m, suffix) == 0;
}

char *md_basename_noext(const char *path)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char *r = md_strdup(b);
    if (!r) return NULL;
    /* quita extensiones conocidas, incluida la doble .dsl.dz */
    static const char *exts[] = {".dict.dz", ".dsl.dz", ".mdx", ".dsl", ".xdxf", ".index",
                                 ".tab", ".tsv", ".dz", ".dc", ".txt.db", ".db", ".zip", ".xml", ".csv", ".quickdic.v006.zip", ".quickdic", ".slob", ".zim", ".dic", ".bgl", ".epub", ".mobi", ".prc", ".azw", NULL};
    for (int i = 0; exts[i]; i++) {
        if (ends_with_ci(r, exts[i])) {
            r[strlen(r) - strlen(exts[i])] = 0;
            break;
        }
    }
    return r;
}

void md_seterr(char *err, int errlen, const char *fmt, ...)
{
    if (!err || errlen <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, (size_t)errlen, fmt, ap);
    va_end(ap);
}

/* ====================================================================== */
/* UTF-8 / UTF-16                                                          */
/* ====================================================================== */

void utf8_put(mbuf *b, uint32_t c)
{
    char t[4];
    if (c < 0x80) { mb_putc(b, (char)c); return; }
    if (c < 0x800) {
        t[0] = (char)(0xC0 | (c >> 6)); t[1] = (char)(0x80 | (c & 0x3F));
        mb_put(b, t, 2);
    } else if (c < 0x10000) {
        t[0] = (char)(0xE0 | (c >> 12)); t[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        t[2] = (char)(0x80 | (c & 0x3F));
        mb_put(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (c >> 18)); t[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((c >> 6) & 0x3F)); t[3] = (char)(0x80 | (c & 0x3F));
        mb_put(b, t, 4);
    }
}

uint32_t utf8_next(const char **ps, const char *end)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    int extra = 0;
    if (c >= 0xF0 && c < 0xF8) { c &= 0x07; extra = 3; }
    else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
    else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
    else if (c >= 0x80) { *ps = (const char *)s; return 0xFFFD; }
    while (extra-- > 0) {
        if ((const char *)s >= end || (*s & 0xC0) != 0x80) { *ps = (const char *)s; return 0xFFFD; }
        c = (c << 6) | (*s++ & 0x3F);
    }
    *ps = (const char *)s;
    return c;
}

void utf16_to_utf8(const uint8_t *s, size_t n, int be, mbuf *out)
{
    mb_reserve(out, n + n / 2);
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = be ? ((uint32_t)s[i] << 8 | s[i + 1]) : ((uint32_t)s[i + 1] << 8 | s[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            uint32_t c2 = be ? ((uint32_t)s[i + 2] << 8 | s[i + 3]) : ((uint32_t)s[i + 3] << 8 | s[i + 2]);
            if (c2 >= 0xDC00 && c2 < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i += 2;
            }
        }
        if (c == 0xFEFF) continue; /* BOM */
        utf8_put(out, c);
    }
}

/* iconv solo se usa con MDict en GBK/Big5. En Android anterior a la API 28
 * bionic no tiene iconv, así que allí se busca en tiempo de ejecución para que
 * la librería cargue igualmente en Android 4.3+. */
#if defined(__ANDROID__) && __ANDROID_API__ < 28
#include <dlfcn.h>
typedef void *iconv_t;
static iconv_t (*p_iconv_open)(const char *, const char *);
static size_t (*p_iconv)(iconv_t, char **, size_t *, char **, size_t *);
static int (*p_iconv_close)(iconv_t);
static int iconv_resolve(void)
{
    static int done = 0;
    if (!done) {
        done = 1;
        void *h = dlopen("libc.so", RTLD_NOW);
        if (h) {
            *(void **)&p_iconv_open = dlsym(h, "iconv_open");
            *(void **)&p_iconv = dlsym(h, "iconv");
            *(void **)&p_iconv_close = dlsym(h, "iconv_close");
        }
    }
    return p_iconv_open && p_iconv && p_iconv_close;
}
#define iconv_open(a, b) (iconv_resolve() ? p_iconv_open(a, b) : (iconv_t)-1)
#define iconv(c, a, b, d, e) p_iconv(c, a, b, d, e)
#define iconv_close(c) p_iconv_close(c)
#else
#include <iconv.h>
#endif

int iconv_to_utf8(const char *enc, const char *s, size_t n, mbuf *out)
{
    iconv_t cd = iconv_open("UTF-8", enc);
    if (cd == (iconv_t)-1) return -1;
    char *in = (char *)s;
    size_t inleft = n;
    while (inleft > 0) {
        if (mb_reserve(out, inleft * 2 + 16)) break;
        char *o = out->p + out->n;
        size_t oleft = out->cap - out->n - 1;
        size_t r = iconv(cd, &in, &inleft, &o, &oleft);
        out->n = (size_t)(o - out->p);
        if (r == (size_t)-1) {
            if (errno == E2BIG) continue;
            /* secuencia inválida: la saltamos */
            in++;
            inleft--;
        }
    }
    iconv_close(cd);
    return 0;
}

void decode_raw(const char *s, size_t n, int enc, mbuf *out)
{
    if (enc == ENC_UTF8) {
        if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
            s += 3;
            n -= 3;
        }
        mb_put(out, s, n);
    } else {
        utf16_to_utf8((const uint8_t *)s, n, enc == ENC_UTF16BE, out);
    }
}

/* Minúsculas para los bloques más habituales (latín, griego, cirílico). */
uint32_t md_lower(uint32_t c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x137) return c | 1;
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177) return c | 1;
    if (c == 0x178) return 0xFF;
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x460 && c <= 0x4FF && c != 0x482 && !(c >= 0x483 && c <= 0x489)) return c | 1;
    if (c >= 0x1E00 && c <= 0x1EFF) return c | 1;
    return c;
}

static const unsigned char UNACCENT_C0[400] = {97,97,97,97,97,97,0,99,101,101,101,101,105,105,105,105,100,110,111,111,111,111,111,0,111,117,117,117,117,121,116,115,97,97,97,97,97,97,0,99,101,101,101,101,105,105,105,105,100,110,111,111,111,111,111,0,111,117,117,117,117,121,116,121,97,97,97,97,97,97,99,99,99,99,99,99,99,99,100,100,100,100,101,101,101,101,101,101,101,101,101,101,103,103,103,103,103,103,103,103,104,104,104,104,105,105,105,105,105,105,105,105,105,105,0,0,106,106,107,107,0,108,108,108,108,108,108,0,0,108,108,110,110,110,110,110,110,0,0,0,111,111,111,111,111,111,0,0,114,114,114,114,114,114,115,115,115,115,115,115,115,115,116,116,116,116,116,116,117,117,117,117,117,117,117,117,117,117,117,117,119,119,121,121,121,122,122,122,122,122,122,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,111,111,0,0,0,0,0,0,0,0,0,0,0,0,0,117,117,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,97,97,105,105,111,111,117,117,117,117,117,117,117,117,117,117,0,97,97,97,97,0,0,0,0,103,103,107,107,111,111,111,111,0,0,106,0,0,0,103,103,0,0,110,110,97,97,0,0,0,0,97,97,97,97,101,101,101,101,105,105,105,105,111,111,111,111,114,114,114,114,117,117,117,117,115,115,116,116,0,0,104,104,0,0,0,0,0,0,97,97,101,101,111,111,111,111,111,111,111,111,121,121,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

/* Quita diacríticos de letras latinas (y ё -> е). Solo para comparaciones laxas. */
static uint32_t md_unaccent(uint32_t c)
{
    if (c >= 0xC0 && c < 0xC0 + sizeof UNACCENT_C0 && UNACCENT_C0[c - 0xC0]) return UNACCENT_C0[c - 0xC0];
    if (c == 0x451 || c == 0x401) return 0x435;
    return c;
}

static int is_space_cp(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000;
}

static int is_mdx_punct(uint32_t c)
{
    if (c < 0x80) return strchr(" _=,.;:!?@%&#~`()[]<>{}-/\\'\"$^*+|", (int)c) != NULL && c != 0;
    return c == 0x201C || c == 0x201D || c == 0x2018 || c == 0x2019;
}

void md_fold(const char *s, size_t n, int mode, int keep_case, mbuf *out)
{
    const char *end = s + n;
    int pending_space = 0;
    size_t start = out->n;
    while (s < end) {
        uint32_t c = utf8_next(&s, end);
        if (c == 0) continue;
        if (mode == FOLD_LOOSE || mode == FOLD_LOOSE_T || mode == FOLD_ROOT) {
            /* aproximación al nivel primario de ICU: sin mayúsculas ni acentos.
             * FOLD_LOOSE ignora la puntuación (colación "shifted");
             * FOLD_LOOSE_T se detiene en ella (puntuación significativa, que en ICU
             * ordena antes que cualquier letra). */
            int punct = is_mdx_punct(c) || is_space_cp(c) || (c < 0x80 && !isalnum((int)c));
            if (punct) {
                if (mode == FOLD_LOOSE_T) break;
                continue;
            }
            c = md_lower(c);
            if (c == 0xDF) { mb_puts(out, "ss"); continue; }          /* ß */
            if (c == 0xE6) { mb_puts(out, "ae"); continue; }          /* æ */
            if (c == 0x153) { mb_puts(out, "oe"); continue; }         /* œ */
            if (c == 0x439 && mode != FOLD_ROOT) c = 0x438;           /* й -> и (colación rusa) */
            utf8_put(out, md_unaccent(c));
            continue;
        }
        if (mode == FOLD_MDX) {
            if (is_mdx_punct(c) || is_space_cp(c)) continue;
        } else {
            if (is_space_cp(c)) {
                pending_space = 1;
                continue;
            }
            if (mode == FOLD_DICTD && c < 0x80 && !isalnum((int)c)) continue;
        }
        if (pending_space && out->n > start) mb_putc(out, ' ');
        pending_space = 0;
        utf8_put(out, keep_case ? c : md_lower(c));
    }
    mb_cstr(out);
}

int md_keycmp(const char *a, size_t an, const char *b, size_t bn)
{
    size_t m = an < bn ? an : bn;
    int r = memcmp(a, b, m);
    if (r) return r;
    return an < bn ? -1 : (an > bn ? 1 : 0);
}

int md_has_prefix(const char *s, size_t sn, const char *pre, size_t pn)
{
    return sn >= pn && memcmp(s, pre, pn) == 0;
}

/* ====================================================================== */
/* resultados                                                              */
/* ====================================================================== */

int md_results_add(md_results *r, const char *word, size_t wn, const char *def, size_t dn)
{
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8;
        char **w = realloc(r->words, sizeof(char *) * (size_t)cap);
        if (!w) return -1;
        r->words = w;
        char **d = realloc(r->defs, sizeof(char *) * (size_t)cap);
        if (!d) return -1;
        r->defs = d;
        r->cap = cap;
    }
    r->words[r->n] = md_strndup(word, wn);
    r->defs[r->n] = md_strndup(def, dn);
    r->n++;
    return 0;
}

int md_results_has(md_results *r, const char *word, const char *def)
{
    for (int i = 0; i < r->n; i++)
        if (!strcmp(r->words[i], word) && !strcmp(r->defs[i], def)) return 1;
    return 0;
}

/* ====================================================================== */
/* descompresión                                                           */
/* ====================================================================== */

/* Inflado tolerante: acepta flujos que terminan sin bloque final (dictzip). */
int md_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, int zlib_header)
{
    tinfl_decompressor dec;
    tinfl_init(&dec);
    size_t in_sz = in_len, out_sz = out_len;
    int flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_HAS_MORE_INPUT;
    if (zlib_header) flags |= TINFL_FLAG_PARSE_ZLIB_HEADER;
    tinfl_status st = tinfl_decompress(&dec, in, &in_sz, out, out, &out_sz, flags);
    if (st == TINFL_STATUS_DONE || st == TINFL_STATUS_NEEDS_MORE_INPUT || st == TINFL_STATUS_HAS_MORE_OUTPUT)
        return (int)out_sz;
    return -1;
}

uint8_t *md_inflate_heap(const uint8_t *in, size_t in_len, size_t *out_len, int zlib_header)
{
    return tinfl_decompress_mem_to_heap(in, in_len, out_len, zlib_header ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
}

/* ====================================================================== */
/* rfile: acceso aleatorio a archivos planos, dictzip y gzip               */
/* ====================================================================== */

enum { RF_PLAIN, RF_DICTZIP, RF_GZFULL };

struct rfile {
    FILE *fp;
    int kind;
    uint64_t size;
    /* dictzip */
    uint32_t chlen, chcnt;
    uint64_t *choff; /* chcnt+1 offsets comprimidos */
    int cached_chunk;
    uint8_t *cache;
    size_t cache_len;
    uint8_t *cbuf; size_t cbuf_cap;
    /* gzip completo en memoria */
    uint8_t *data;
};

static int rd_u16le(FILE *fp, uint32_t *v)
{
    unsigned char b[2];
    if (fread(b, 1, 2, fp) != 2) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    return 0;
}

static int skip_cstr(FILE *fp)
{
    int c;
    while ((c = fgetc(fp)) != EOF && c != 0) {}
    return c == EOF ? -1 : 0;
}

rfile *rf_open(const char *path, char *err, int errlen)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        md_seterr(err, errlen, "no se puede abrir %s: %s", path, strerror(errno));
        return NULL;
    }
    rfile *f = calloc(1, sizeof *f);
    f->fp = fp;
    f->cached_chunk = -1;
    unsigned char h[10];
    size_t got = fread(h, 1, 10, fp);
    if (got == 10 && h[0] == 0x1F && h[1] == 0x8B && h[2] == 8) {
        int flags = h[3];
        uint32_t ra_chlen = 0, ra_chcnt = 0;
        uint32_t *ra_sizes = NULL;
        if (flags & 4) {
            uint32_t xlen;
            if (rd_u16le(fp, &xlen)) goto bad;
            long xend = ftello(fp) + (long)xlen;
            while (ftello(fp) + 4 <= xend) {
                int si1 = fgetc(fp), si2 = fgetc(fp);
                uint32_t slen;
                if (rd_u16le(fp, &slen)) goto bad;
                long sstart = ftello(fp);
                if (si1 == 'R' && si2 == 'A' && slen >= 6) {
                    uint32_t ver;
                    rd_u16le(fp, &ver);
                    rd_u16le(fp, &ra_chlen);
                    rd_u16le(fp, &ra_chcnt);
                    ra_sizes = malloc(sizeof(uint32_t) * (ra_chcnt + 1));
                    for (uint32_t i = 0; i < ra_chcnt; i++)
                        if (rd_u16le(fp, &ra_sizes[i])) goto bad;
                }
                fseeko(fp, sstart + (long)slen, SEEK_SET);
            }
            fseeko(fp, xend, SEEK_SET);
        }
        if (flags & 8) skip_cstr(fp);
        if (flags & 16) skip_cstr(fp);
        if (flags & 2) fseeko(fp, 2, SEEK_CUR);
        uint64_t data_start = (uint64_t)ftello(fp);

        /* tamaño descomprimido: campo ISIZE del final */
        fseeko(fp, -4, SEEK_END);
        unsigned char t[4];
        uint64_t isize = 0;
        if (fread(t, 1, 4, fp) == 4)
            isize = (uint64_t)t[0] | ((uint64_t)t[1] << 8) | ((uint64_t)t[2] << 16) | ((uint64_t)t[3] << 24);

        if (ra_sizes && ra_chcnt > 0 && ra_chlen > 0) {
            f->kind = RF_DICTZIP;
            f->chlen = ra_chlen;
            f->chcnt = ra_chcnt;
            f->choff = malloc(sizeof(uint64_t) * (ra_chcnt + 1));
            f->choff[0] = data_start;
            for (uint32_t i = 0; i < ra_chcnt; i++) f->choff[i + 1] = f->choff[i] + ra_sizes[i];
            f->cache = malloc(ra_chlen);
            /* con archivos > 4 GB ISIZE se trunca; usamos una cota superior */
            uint64_t maxsz = (uint64_t)ra_chlen * ra_chcnt;
            f->size = (isize && isize <= maxsz && isize > maxsz - ra_chlen) ? isize : maxsz;
            free(ra_sizes);
        } else {
            /* gzip sin índice de acceso aleatorio: se descomprime entero una vez */
            free(ra_sizes);
            fseeko(fp, 0, SEEK_END);
            long total = ftello(fp);
            size_t clen = (size_t)(total - (long)data_start);
            uint8_t *cdata = malloc(clen);
            fseeko(fp, (long)data_start, SEEK_SET);
            if (!cdata || fread(cdata, 1, clen, fp) != clen) { free(cdata); goto bad; }
            size_t outlen = 0;
            f->data = md_inflate_heap(cdata, clen, &outlen, 0);
            free(cdata);
            if (!f->data) goto bad;
            f->size = outlen;
            f->kind = RF_GZFULL;
        }
        return f;
    bad:
        md_seterr(err, errlen, "archivo gzip/dictzip dañado: %s", path);
        rf_close(f);
        return NULL;
    }
    f->kind = RF_PLAIN;
    fseeko(fp, 0, SEEK_END);
    f->size = (uint64_t)ftello(fp);
    return f;
}

void rf_close(rfile *f)
{
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f->choff);
    free(f->cache);
    free(f->cbuf);
    free(f->data);
    free(f);
}

uint64_t rf_size(rfile *f) { return f->size; }

static int dz_load_chunk(rfile *f, uint32_t i)
{
    if ((int)i == f->cached_chunk) return 0;
    if (i >= f->chcnt) return -1;
    size_t clen = (size_t)(f->choff[i + 1] - f->choff[i]);
    if (clen > f->cbuf_cap) {
        uint8_t *nb = realloc(f->cbuf, clen);
        if (!nb) return -1;
        f->cbuf = nb;
        f->cbuf_cap = clen;
    }
    fseeko(f->fp, (long)f->choff[i], SEEK_SET);
    if (fread(f->cbuf, 1, clen, f->fp) != clen) return -1;
    int n = md_inflate(f->cbuf, clen, f->cache, f->chlen, 0);
    if (n < 0) return -1;
    f->cache_len = (size_t)n;
    f->cached_chunk = (int)i;
    return 0;
}

size_t rf_pread(rfile *f, uint64_t off, void *dst, size_t len)
{
    if (off >= f->size) return 0;
    if (off + len > f->size) len = (size_t)(f->size - off);
    switch (f->kind) {
    case RF_PLAIN:
        fseeko(f->fp, (long)off, SEEK_SET);
        return fread(dst, 1, len, f->fp);
    case RF_GZFULL:
        memcpy(dst, f->data + off, len);
        return len;
    case RF_DICTZIP: {
        size_t done = 0;
        while (done < len) {
            uint64_t o = off + done;
            uint32_t ci = (uint32_t)(o / f->chlen);
            if (dz_load_chunk(f, ci)) break;
            size_t in_chunk = (size_t)(o % f->chlen);
            if (in_chunk >= f->cache_len) break;
            size_t take = f->cache_len - in_chunk;
            if (take > len - done) take = len - done;
            memcpy((char *)dst + done, f->cache + in_chunk, take);
            done += take;
        }
        return done;
    }
    }
    return 0;
}

int rf_read(rfile *f, uint64_t off, size_t len, mbuf *out)
{
    if (mb_reserve(out, len)) return -1;
    size_t got = rf_pread(f, off, out->p + out->n, len);
    out->n += got;
    return got == len ? 0 : -1;
}

/* ====================================================================== */
/* lector de líneas                                                        */
/* ====================================================================== */

#define LR_CHUNK (256 * 1024)

static int lr_fill(linereader *lr)
{
    if (lr->eof) return 0;
    /* compacta */
    if (lr->raw_pos > 0) {
        memmove(lr->raw.p, lr->raw.p + lr->raw_pos, lr->raw.n - lr->raw_pos);
        lr->raw.n -= lr->raw_pos;
        lr->raw_off += lr->raw_pos;
        lr->raw_pos = 0;
    }
    if (mb_reserve(&lr->raw, LR_CHUNK)) return 0;
    size_t got = rf_pread(lr->f, lr->pos, lr->raw.p + lr->raw.n, LR_CHUNK);
    lr->raw.n += got;
    lr->pos += got;
    if (got == 0) lr->eof = 1;
    return (int)got;
}

int lr_init(linereader *lr, rfile *f)
{
    memset(lr, 0, sizeof *lr);
    lr->f = f;
    lr->size = rf_size(f);
    lr_fill(lr);
    const unsigned char *p = (const unsigned char *)lr->raw.p;
    size_t n = lr->raw.n;
    lr->enc = ENC_UTF8;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { lr->enc = ENC_UTF16LE; lr->raw_pos = 2; }
    else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { lr->enc = ENC_UTF16BE; lr->raw_pos = 2; }
    else if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { lr->raw_pos = 3; }
    else if (n >= 4 && p[1] == 0 && p[3] == 0 && p[0] != 0) lr->enc = ENC_UTF16LE;
    else if (n >= 4 && p[0] == 0 && p[2] == 0 && p[1] != 0) lr->enc = ENC_UTF16BE;
    return 0;
}

int lr_next(linereader *lr, mbuf *line, uint64_t *start, uint64_t *end)
{
    line->n = 0;
    for (;;) {
        const char *p = lr->raw.p + lr->raw_pos;
        size_t avail = lr->raw.n - lr->raw_pos;
        size_t nl = (size_t)-1, unit = 1;
        if (lr->enc == ENC_UTF8) {
            const char *q = avail ? memchr(p, '\n', avail) : NULL;
            if (q) nl = (size_t)(q - p);
        } else {
            unit = 2;
            /* el texto UTF-16 está alineado a 2 bytes respecto al inicio del archivo */
            size_t i = ((lr->raw_off + lr->raw_pos) & 1) ? 1 : 0;
            for (; i + 1 < avail; i += 2) {
                if (lr->enc == ENC_UTF16LE ? (p[i] == '\n' && p[i + 1] == 0) : (p[i] == 0 && p[i + 1] == '\n')) {
                    nl = i;
                    break;
                }
            }
        }
        if (nl != (size_t)-1 || (lr->eof && avail > 0)) {
            size_t len = nl != (size_t)-1 ? nl : avail;
            size_t consumed = nl != (size_t)-1 ? nl + unit : avail;
            *start = lr->raw_off + lr->raw_pos;
            *end = *start + consumed;
            decode_raw(p, len, lr->enc, line);
            while (line->n && (line->p[line->n - 1] == '\r')) line->n--;
            mb_cstr(line);
            lr->raw_pos += consumed;
            return 1;
        }
        if (lr->eof) return 0;
        if (!lr_fill(lr) && avail == 0) return 0;
    }
}

void lr_free(linereader *lr) { mb_free(&lr->raw); }
