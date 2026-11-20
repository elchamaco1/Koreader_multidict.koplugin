/* PocketBook SDIC (.dic).
 *
 * Cabecera de 128 bytes ("SDIC", versión, nº de entradas...) con los offsets de
 * las secciones: metadatos JSON (0x20), tabla de ordenación (0x24), morfemas
 * (0x28), teclado (0x2C), índice disperso (0x38) y bloques de datos (0x3C).
 *
 * Índice disperso (zlib): por bloque, u16 tamaño comprimido + primera palabra\0.
 * Bloque (zlib): entradas u16 tamaño, palabra\0, ' ', definición, " \n\0".
 * Las entradas están ordenadas por una clave = palabra pasada por la tabla de
 * ordenación del propio archivo y en mayúsculas, así que la búsqueda es
 * directa y exacta: búsqueda binaria en el índice disperso y un solo bloque.
 *
 * Formato según pyglossary/plugins/pocketbook_sdic.
 */
#include <stdlib.h>
#include <string.h>

#include "md_internal.h"

typedef struct {
    FILE *fp;
    uint32_t *collate;     /* 65536 entradas: 0xFFFFFFFF = sin cambio, 0 = se elimina */
    uint32_t n_blocks;
    uint64_t *boff;        /* offset de cada bloque */
    uint32_t *bsize;
    char **bkey;           /* clave de la primera palabra de cada bloque */
    long cblock;
    mbuf cache;
} sdic;

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int sd_read(sdic *s, uint64_t off, void *dst, size_t n)
{
    if (fseeko(s->fp, (off_t)off, SEEK_SET)) return -1;
    return fread(dst, 1, n, s->fp) == n ? 0 : -1;
}

/* Mayúsculas como str.upper() de Python para los alfabetos habituales. */
static uint32_t up(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x137 && (c & 1)) return c - 1;
    if (c >= 0x139 && c <= 0x148 && !(c & 1)) return c - 1;
    if (c >= 0x14A && c <= 0x177 && (c & 1)) return c - 1;
    if (c >= 0x179 && c <= 0x17E && !(c & 1)) return c - 1;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0x460 && c <= 0x4FF && (c & 1)) return c - 1;
    if (c >= 0x1E00 && c <= 0x1EFF && (c & 1)) return c - 1;
    return c;
}

static void sd_key(sdic *s, const char *w, size_t n, mbuf *out)
{
    out->n = 0;
    const char *p = w, *end = w + n;
    while (p < end) {
        uint32_t c = utf8_next(&p, end);
        if (c < 0x10000 && s->collate[c] != 0xFFFFFFFFu) {
            c = s->collate[c];
            if (!c) continue;
        }
        if (c == 0xDF) { mb_puts(out, "SS"); continue; }
        utf8_put(out, up(c));
    }
    mb_cstr(out);
}

static int sdic_probe(const char *p)
{
    if (!ends_with_ci(p, ".dic")) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char sig[4];
    int ok = fread(sig, 1, 4, f) == 4 && !memcmp(sig, "SDIC", 4);
    fclose(f);
    return ok;
}

static int sdic_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

static void sdic_free(sdic *s)
{
    if (!s) return;
    if (s->fp) fclose(s->fp);
    free(s->collate);
    free(s->boff);
    free(s->bsize);
    for (uint32_t i = 0; i < s->n_blocks; i++) free(s->bkey ? s->bkey[i] : NULL);
    free(s->bkey);
    mb_free(&s->cache);
    free(s);
}

/* Valor de "clave": "texto" en un JSON sencillo. */
static char *json_str(const char *j, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(j, pat);
    if (!k) return NULL;
    k = strchr(k + strlen(pat), ':');
    if (!k) return NULL;
    k++;
    while (*k == ' ' || *k == '\n' || *k == '\t') k++;
    if (*k != '"') return NULL;
    k++;
    mbuf o = {0};
    for (; *k && *k != '"'; k++) {
        if (*k == '\\' && k[1]) { k++; mb_putc(&o, *k == 'n' ? '\n' : *k); continue; }
        mb_putc(&o, *k);
    }
    return mb_steal(&o);
}

static int sdic_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    (void)cache_dir;
    sdic *s = calloc(1, sizeof *s);
    s->cblock = -1;
    s->fp = fopen(d->path, "rb");
    uint8_t h[128];
    uint8_t *buf = NULL;
    if (!s->fp || sd_read(s, 0, h, 128) || memcmp(h, "SDIC", 4)) goto bad;
    d->word_count = le32(h + 8);
    uint32_t meta_off = le32(h + 0x20), coll_off = le32(h + 0x24), sparse_off = le32(h + 0x38), data_off = le32(h + 0x3C);

    /* tabla de ordenación */
    s->collate = malloc(65536 * sizeof(uint32_t));
    memset(s->collate, 0xFF, 65536 * sizeof(uint32_t));
    uint8_t c4[4];
    if (sd_read(s, coll_off, c4, 4)) goto bad;
    uint32_t clen = le32(c4);
    if (clen > 1u << 20) goto bad;
    buf = malloc(clen ? clen : 1);
    if (sd_read(s, coll_off + 4, buf, clen)) goto bad;
    for (uint32_t i = 0; i + 4 <= clen; i += 4) {
        uint32_t k = buf[i] | (buf[i + 1] << 8), v = buf[i + 2] | (buf[i + 3] << 8);
        s->collate[k] = v;
    }
    free(buf);
    buf = NULL;

    /* índice disperso */
    if (sd_read(s, sparse_off, c4, 4)) goto bad;
    uint32_t ulen = le32(c4);
    if (data_off <= sparse_off + 4 || ulen > 256u << 20) goto bad;
    size_t zlen = data_off - sparse_off - 4;
    buf = malloc(zlen);
    uint8_t *idx = malloc(ulen + 1);
    if (!buf || !idx || sd_read(s, sparse_off + 4, buf, zlen) || md_inflate(buf, zlen, idx, ulen, 1) < 0) { free(idx); goto bad; }
    size_t cap = 256;
    s->boff = malloc(cap * sizeof(uint64_t));
    s->bsize = malloc(cap * sizeof(uint32_t));
    s->bkey = malloc(cap * sizeof(char *));
    uint64_t off = data_off;
    mbuf k = {0};
    for (size_t p = 0; p + 2 < ulen;) {
        uint32_t sz = idx[p] | (idx[p + 1] << 8);
        if (!sz) break;
        p += 2;
        const uint8_t *z = memchr(idx + p, 0, ulen - p);
        if (!z) break;
        if (s->n_blocks == cap) {
            cap *= 2;
            s->boff = realloc(s->boff, cap * sizeof(uint64_t));
            s->bsize = realloc(s->bsize, cap * sizeof(uint32_t));
            s->bkey = realloc(s->bkey, cap * sizeof(char *));
        }
        sd_key(s, (const char *)idx + p, (size_t)(z - (idx + p)), &k);
        s->boff[s->n_blocks] = off;
        s->bsize[s->n_blocks] = sz;
        s->bkey[s->n_blocks] = md_strndup(k.p, k.n);
        s->n_blocks++;
        off += sz;
        p = (size_t)(z - idx) + 1;
    }
    mb_free(&k);
    free(idx);
    free(buf);
    buf = NULL;

    /* nombre e idiomas */
    char *name = NULL;
    if (meta_off) {
        if (!sd_read(s, meta_off, c4, 4) && le32(c4) < 65536) {
            uint32_t ml = le32(c4);
            char *j = malloc(ml + 1);
            if (j && !sd_read(s, meta_off + 4, j, ml)) {
                j[ml] = 0;
                name = json_str(j, "name");
                d->lang_from = json_str(j, "localeFrom");
                d->lang_to = json_str(j, "localeTo");
            }
            free(j);
        }
    }
    if (!name || !*name) {
        free(name);
        name = md_strndup((const char *)h + 0x40, strnlen((const char *)h + 0x40, 64));
    }
    if (*name) { free(d->name); d->name = name; } else free(name);
    d->is_html = 1;
    d->priv = s;
    return 0;
bad:
    free(buf);
    md_seterr(err, errlen, "archivo SDIC dañado o no soportado: %s", d->path);
    sdic_free(s);
    return -1;
}

static void sdic_close(md_dict *d) { sdic_free(d->priv); }

static int load_block(sdic *s, uint32_t i)
{
    if ((long)i == s->cblock) return 0;
    uint8_t *c = malloc(s->bsize[i]);
    if (!c || sd_read(s, s->boff[i], c, s->bsize[i])) { free(c); return -1; }
    size_t ol = 0;
    uint8_t *u = md_inflate_heap(c, s->bsize[i], &ol, 1);
    free(c);
    if (!u) return -1;
    s->cache.n = 0;
    mb_put(&s->cache, u, ol);
    free(u);
    s->cblock = (long)i;
    return 0;
}

/* Recorre el bloque i; añade las entradas cuya clave coincide. Devuelve 1 si
 * ya se pasó de la palabra (no hace falta mirar más bloques). */
/* Cuerpo almacenado -> HTML. El conversor de PocketBook guarda <b>/<i> como
 * 0x02/0x03 y 0x01 cierra el último abierto; el lector muestra '\n' como
 * salto de línea. Los de pyglossary llevan " " delante y " \n\0" detrás. */
static char *sd_body(const uint8_t *bs, const uint8_t *be)
{
    while (bs < be && (*bs == 0 || *bs == '\n' || *bs == '\r' || *bs == ' ')) bs++;
    while (be > bs && (be[-1] == 0 || be[-1] == '\n' || be[-1] == '\r' || be[-1] == ' ')) be--;
    mbuf o = {0};
    char st[64];
    int sp = 0;
    for (const uint8_t *p = bs; p < be; p++) {
        uint8_t c = *p;
        if (c >= 0x20 || c == '\t') mb_putc(&o, (char)c);
        else if (c == '\n') mb_puts(&o, "<br>");
        else if (c == 2 || c == 3) {
            if (sp < (int)sizeof st) st[sp++] = c == 2 ? 'b' : 'i';
            mb_puts(&o, c == 2 ? "<b>" : "<i>");
        } else if (c == 1 && sp > 0) {
            mb_puts(&o, st[--sp] == 'b' ? "</b>" : "</i>");
        }
    }
    while (sp > 0) mb_puts(&o, st[--sp] == 'b' ? "</b>" : "</i>");
    char *r = md_strndup(o.p ? o.p : "", o.n);
    mb_free(&o);
    return r;
}

static int scan(sdic *s, uint32_t i, const char *q, size_t qn, int fuzzy, int exact_pass, const char *word,
                int max, md_results *r)
{
    if (load_block(s, i)) return 1;
    const uint8_t *p = (const uint8_t *)s->cache.p, *end = p + s->cache.n;
    mbuf k = {0}, a = {0}, b = {0};
    md_fold(word, strlen(word), FOLD_PLAIN, 0, &b);
    int past = 0;
    while (p + 2 < end && r->n < max) {
        uint32_t sz = p[0] | (p[1] << 8);
        if (sz < 3 || p + sz > end) break;
        const uint8_t *t = p + 2, *te = memchr(t, 0, sz - 2);
        if (!te) break;
        sd_key(s, (const char *)t, (size_t)(te - t), &k);
        int c = md_keycmp(k.p, k.n, q, qn);
        int m = fuzzy ? md_has_prefix(k.p, k.n, q, qn) : c == 0;
        if (m) {
            a.n = 0;
            md_fold((const char *)t, (size_t)(te - t), FOLD_PLAIN, 0, &a);
            int exact = md_keycmp(a.p, a.n, b.p, b.n) == 0;
            if (exact == exact_pass) {
                const uint8_t *bs = te + 1, *be = p + sz;
                char *w = md_strndup((const char *)t, (size_t)(te - t));
                char *def = sd_body(bs, be);
                if (!md_results_has(r, w, def)) md_results_add(r, w, strlen(w), def, strlen(def));
                free(w);
                free(def);
            }
        } else if (c > 0) {
            past = 1;
            break;
        }
        p += sz;
    }
    mb_free(&k); mb_free(&a); mb_free(&b);
    return past;
}

static int sdic_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    sdic *s = d->priv;
    mbuf q = {0};
    sd_key(s, word, strlen(word), &q);
    if (!q.n || !s->n_blocks) { mb_free(&q); return 0; }
    /* primer bloque cuya primera clave es >= q; empezamos en el anterior */
    uint32_t lo = 0, hi = s->n_blocks;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (md_keycmp(s->bkey[mid], strlen(s->bkey[mid]), q.p, q.n) < 0) lo = mid + 1;
        else hi = mid;
    }
    uint32_t start = lo > 0 ? lo - 1 : 0;
    for (int pass = 1; pass >= 0 && r->n < max; pass--)
        for (uint32_t i = start; i < s->n_blocks && r->n < max; i++)
            if (scan(s, i, q.p, q.n, fuzzy, pass, word, max, r)) break;
    mb_free(&q);
    return r->n;
}

const md_format_t md_fmt_sdic = {"sdic", sdic_probe, sdic_needs_index, sdic_open, sdic_close, sdic_lookup};
