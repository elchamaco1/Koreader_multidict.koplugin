/* ZIM (Kiwix: Wikipedia, Wiktionary...).
 *
 * Cabecera (little-endian): magic 0x044D495A, versión, UUID, nº de entradas,
 * nº de clusters, posiciones de las listas de URL, títulos y clusters, MIME...
 * La lista de títulos está ordenada por (espacio de nombres, título), así que
 * se busca directamente con búsqueda binaria; no se crea nada.
 *
 * Entrada: u16 mime (0xFFFF = redirección), u8, char espacio, u32 revisión,
 *   artículo: u32 cluster, u32 blob | redirección: u32 destino; url\0 título\0
 * Cluster: u8 (compresión: 1 ninguna, 4 xz, 5 zstd; 0x10 = offsets de 64
 *   bits), tabla de offsets de blobs, datos.
 */
#include <stdlib.h>
#include <string.h>

#include "md_internal.h"

#define ZSTDLIB_VISIBLE
#define ZSTDLIB_HIDDEN
#define ZDICTLIB_VISIBLE
#define ZSTDERRORLIB_VISIBLE
#define ZSTDERRORLIB_HIDDEN
#include "zstd/zstd.h"

typedef struct {
    FILE *fp;
    uint64_t fsize;
    uint32_t n_entries, n_clusters;
    uint64_t url_ptr, title_ptr, cluster_ptr, mime_pos, checksum_pos;
    char **mimes;
    int n_mimes;
    /* caché de un cluster */
    long ccl;
    uint8_t *cdata;
    size_t clen;
    int cext;
    /* lista de títulos: en la cabecera (ZIM antiguos) o en X/listing/titleOrdered */
    uint32_t n_titles;
    uint64_t titles_file_off;   /* si la lista está sin comprimir en el archivo */
    uint8_t *titles_mem;        /* si está en un cluster comprimido */
    int titles_v1;              /* v1: solo artículos de 'C', ordenados por título */
} zim;

typedef struct {
    uint16_t mime;
    char ns;
    uint32_t a, b;        /* cluster y blob, o destino de la redirección */
    mbuf url, title;
} zdirent;

static int z_read(zim *z, uint64_t off, void *dst, size_t n)
{
    if (fseeko(z->fp, (off_t)off, SEEK_SET)) return -1;
    return fread(dst, 1, n, z->fp) == n ? 0 : -1;
}

static uint64_t le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static int read_cstr(zim *z, uint64_t *pos, mbuf *out)
{
    out->n = 0;
    char buf[256];
    for (;;) {
        size_t want = sizeof buf;
        if (*pos >= z->fsize) return -1;
        if (*pos + want > z->fsize) want = (size_t)(z->fsize - *pos);
        if (z_read(z, *pos, buf, want)) return -1;
        char *nul = memchr(buf, 0, want);
        size_t take = nul ? (size_t)(nul - buf) : want;
        mb_put(out, buf, take);
        *pos += take + (nul ? 1 : 0);
        if (nul) break;
        if (out->n > 65536) return -1;
    }
    mb_cstr(out);
    return 0;
}

static int read_dirent(zim *z, uint64_t pos, zdirent *d)
{
    uint8_t h[16];
    if (z_read(z, pos, h, 12)) return -1;
    d->mime = (uint16_t)le(h, 2);
    d->ns = (char)h[3];
    d->a = (uint32_t)le(h + 8, 4);
    pos += 12;
    if (d->mime != 0xFFFF) {
        if (z_read(z, pos, h, 4)) return -1;
        d->b = (uint32_t)le(h, 4);
        pos += 4;
    }
    if (read_cstr(z, &pos, &d->url) || read_cstr(z, &pos, &d->title)) return -1;
    if (!d->title.n) { mb_put(&d->title, d->url.p, d->url.n); mb_cstr(&d->title); }
    return 0;
}

static int dirent_by_url_index(zim *z, uint32_t i, zdirent *d)
{
    uint8_t b[8];
    if (i >= z->n_entries || z_read(z, z->url_ptr + 8ull * i, b, 8)) return -1;
    return read_dirent(z, le(b, 8), d);
}

static int dirent_by_title_index(zim *z, uint32_t i, zdirent *d, uint32_t *url_index)
{
    uint8_t b[4];
    if (i >= z->n_titles) return -1;
    if (z->titles_mem) memcpy(b, z->titles_mem + 4ull * i, 4);
    else if (z_read(z, z->titles_file_off + 4ull * i, b, 4)) return -1;
    *url_index = (uint32_t)le(b, 4);
    return dirent_by_url_index(z, *url_index, d);
}

/* ---------------------------------------------------------------------- */
/* clusters                                                                */
/* ---------------------------------------------------------------------- */

static uint8_t *zstd_heap(const uint8_t *in, size_t n, size_t *out_len)
{
    ZSTD_DCtx *ctx = ZSTD_createDCtx();
    if (!ctx) return NULL;
    size_t cap = n * 4 + 65536, len = 0;
    uint8_t *out = malloc(cap);
    ZSTD_inBuffer ib = {in, n, 0};
    int ok = 0;
    while (out) {
        ZSTD_outBuffer ob = {out + len, cap - len, 0};
        size_t r = ZSTD_decompressStream(ctx, &ob, &ib);
        len += ob.pos;
        if (ZSTD_isError(r)) break;
        if (r == 0 || ib.pos >= ib.size) { ok = 1; if (r == 0 || ob.pos < ob.size) break; }
        if (len == cap) {
            if (cap > 512u * 1024 * 1024) break;
            uint8_t *nb = realloc(out, cap * 2);
            if (!nb) break;
            out = nb;
            cap *= 2;
            ok = 0;
        } else if (ib.pos >= ib.size) {
            break;
        }
    }
    ZSTD_freeDCtx(ctx);
    if (!ok) { free(out); return NULL; }
    *out_len = len;
    return out;
}

static int load_cluster(zim *z, uint32_t ci)
{
    if ((long)ci == z->ccl) return 0;
    if (ci >= z->n_clusters) return -1;
    uint8_t b[16];
    if (z_read(z, z->cluster_ptr + 8ull * ci, b, 8)) return -1;
    uint64_t start = le(b, 8), end;
    if (ci + 1 < z->n_clusters) {
        if (z_read(z, z->cluster_ptr + 8ull * (ci + 1), b, 8)) return -1;
        end = le(b, 8);
    } else {
        end = z->checksum_pos ? z->checksum_pos : z->fsize;
    }
    if (end <= start || end - start > 512u * 1024 * 1024) return -1;
    size_t n = (size_t)(end - start);
    uint8_t *raw = malloc(n);
    if (!raw || z_read(z, start, raw, n)) { free(raw); return -1; }
    int comp = raw[0] & 0x0F, ext = (raw[0] & 0x10) != 0;
    uint8_t *data = NULL;
    size_t len = 0;
    if (comp <= 1) {
        data = malloc(n - 1);
        if (data) { memcpy(data, raw + 1, n - 1); len = n - 1; }
    } else if (comp == 4) {
        data = md_xz_heap(raw + 1, n - 1, &len);
    } else if (comp == 5) {
        data = zstd_heap(raw + 1, n - 1, &len);
    } else if (comp == 2) {
        data = md_inflate_heap(raw + 1, n - 1, &len, 1);
    } else if (comp == 3) {
        data = md_bzip2_heap(raw + 1, n - 1, &len);
    }
    free(raw);
    if (!data) return -1;
    free(z->cdata);
    z->cdata = data;
    z->clen = len;
    z->cext = ext;
    z->ccl = (long)ci;
    return 0;
}

static int blob(zim *z, uint32_t ci, uint32_t bi, const uint8_t **p, size_t *n)
{
    if (load_cluster(z, ci)) return -1;
    int w = z->cext ? 8 : 4;
    if ((size_t)w > z->clen) return -1;
    uint64_t first = le(z->cdata, w);
    uint64_t count = first / (uint64_t)w;
    if (count == 0 || bi + 1 >= count || first > z->clen) return -1;
    uint64_t a = le(z->cdata + (uint64_t)w * bi, w), b = le(z->cdata + (uint64_t)w * (bi + 1), w);
    if (a > b || b > z->clen) return -1;
    *p = z->cdata + a;
    *n = (size_t)(b - a);
    return 0;
}

/* ---------------------------------------------------------------------- */

static int zim_probe(const char *p) { return ends_with_ci(p, ".zim"); }
static int zim_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

static void zim_free(zim *z)
{
    if (!z) return;
    if (z->fp) fclose(z->fp);
    for (int i = 0; i < z->n_mimes; i++) free(z->mimes[i]);
    free(z->mimes);
    free(z->cdata);
    free(z->titles_mem);
    free(z);
}

/* Busca por (espacio, url) en la lista de URL; devuelve el índice o -1. */
static long find_url(zim *z, char ns, const char *url)
{
    zdirent d = {0};
    uint32_t lo = 0, hi = z->n_entries;
    long found = -1;
    size_t ul = strlen(url);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (dirent_by_url_index(z, mid, &d)) break;
        int c = (unsigned char)d.ns - (unsigned char)ns;
        if (!c) c = md_keycmp(d.url.p, d.url.n, url, ul);
        if (c < 0) lo = mid + 1;
        else { if (!c) found = mid; hi = mid; }
    }
    mb_free(&d.url); mb_free(&d.title);
    return found;
}

static char *metadata(zim *z, const char *key)
{
    long i = find_url(z, 'M', key);
    if (i < 0) return NULL;
    zdirent d = {0};
    char *r = NULL;
    const uint8_t *p;
    size_t n;
    if (!dirent_by_url_index(z, (uint32_t)i, &d) && d.mime != 0xFFFF && !blob(z, d.a, d.b, &p, &n))
        r = md_strndup((const char *)p, n);
    mb_free(&d.url); mb_free(&d.title);
    return r;
}

static int zim_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    (void)cache_dir;
    zim *z = calloc(1, sizeof *z);
    z->ccl = -1;
    z->fp = fopen(d->path, "rb");
    if (!z->fp) { md_seterr(err, errlen, "no se puede abrir %s", d->path); zim_free(z); return -1; }
    fseeko(z->fp, 0, SEEK_END);
    z->fsize = (uint64_t)ftello(z->fp);
    uint8_t h[80];
    if (z_read(z, 0, h, 80) || le(h, 4) != 0x044D495A) {
        md_seterr(err, errlen, "no es un archivo ZIM: %s", d->path);
        zim_free(z);
        return -1;
    }
    z->n_entries = (uint32_t)le(h + 24, 4);
    z->n_clusters = (uint32_t)le(h + 28, 4);
    z->url_ptr = le(h + 32, 8);
    z->title_ptr = le(h + 40, 8);
    z->cluster_ptr = le(h + 48, 8);
    z->mime_pos = le(h + 56, 8);
    z->checksum_pos = le(h + 72, 8);
    /* lista de tipos MIME */
    uint64_t pos = z->mime_pos;
    mbuf m = {0};
    int cap = 0;
    while (z->n_mimes < 1024 && !read_cstr(z, &pos, &m) && m.n) {
        if (z->n_mimes == cap) {
            cap = cap ? cap * 2 : 16;
            z->mimes = realloc(z->mimes, sizeof(char *) * (size_t)cap);
        }
        z->mimes[z->n_mimes++] = md_strndup(m.p, m.n);
    }
    mb_free(&m);
    char *t = metadata(z, "Title");
    if (t && *t) { free(d->name); d->name = t; } else free(t);
    char *lang = metadata(z, "Language");
    if (lang && *lang) d->lang_from = lang; else free(lang);
    /* lista de títulos */
    if (z->title_ptr != UINT64_MAX && z->title_ptr < z->fsize) {
        z->n_titles = z->n_entries;
        z->titles_file_off = z->title_ptr;
    } else {
        static const char *const lists[] = {"listing/titleOrdered/v0", "listing/titleOrdered/v1"};
        for (int k = 0; k < 2 && !z->n_titles; k++) {
            long ui = find_url(z, 'X', lists[k]);
            if (ui < 0) continue;
            zdirent e = {0};
            if (!dirent_by_url_index(z, (uint32_t)ui, &e) && e.mime != 0xFFFF) {
                /* si el cluster no está comprimido se lee directamente del archivo */
                uint8_t b[8];
                if (!z_read(z, z->cluster_ptr + 8ull * e.a, b, 8)) {
                    uint64_t cstart = le(b, 8);
                    uint8_t info;
                    if (!z_read(z, cstart, &info, 1) && (info & 0x0F) <= 1) {
                        int w = (info & 0x10) ? 8 : 4;
                        uint8_t o[16];
                        if (!z_read(z, cstart + 1 + (uint64_t)w * e.b, o, (size_t)(2 * w))) {
                            uint64_t a = le(o, w), bb = le(o + w, w);
                            z->titles_file_off = cstart + 1 + a;
                            z->n_titles = (uint32_t)((bb - a) / 4);
                        }
                    } else {
                        const uint8_t *p;
                        size_t n;
                        if (!blob(z, e.a, e.b, &p, &n)) {
                            z->titles_mem = malloc(n ? n : 1);
                            if (z->titles_mem) { memcpy(z->titles_mem, p, n); z->n_titles = (uint32_t)(n / 4); }
                        }
                    }
                }
                z->titles_v1 = k == 1;
            }
            mb_free(&e.url); mb_free(&e.title);
        }
    }
    if (!z->n_titles) {
        md_seterr(err, errlen, "el ZIM no tiene lista de títulos: %s", d->path);
        zim_free(z);
        return -1;
    }
    d->word_count = z->titles_v1 ? (long)z->n_titles : (long)z->n_entries;
    d->is_html = 1;
    d->priv = z;
    return 0;
}

static void zim_close(md_dict *d) { zim_free(d->priv); }

/* Muestra el contenido de una entrada (siguiendo redirecciones). */
static void emit(zim *z, zdirent *e, md_results *r)
{
    zdirent d = {0};
    mb_put(&d.url, e->url.p, e->url.n);
    d.mime = e->mime; d.a = e->a; d.b = e->b; d.ns = e->ns;
    for (int hop = 0; d.mime == 0xFFFF && hop < 5; hop++)
        if (dirent_by_url_index(z, d.a, &d)) { mb_free(&d.url); mb_free(&d.title); return; }
    const uint8_t *p;
    size_t n;
    const char *mime = d.mime < z->n_mimes ? z->mimes[d.mime] : "";
    if (d.mime == 0xFFFF || strncmp(mime, "text/", 5) || blob(z, d.a, d.b, &p, &n)) {
        mb_free(&d.url); mb_free(&d.title);
        return;
    }
    mbuf out = {0};
    if (!strncmp(mime, "text/html", 9)) {
        /* solo el cuerpo de la página */
        const char *s = (const char *)p, *end = s + n;
        mbuf tmp = {0};
        mb_put(&tmp, s, n);
        mb_cstr(&tmp);
        const char *b = strstr(tmp.p, "<body");
        if (b) {
            const char *gt = strchr(b, '>');
            const char *be = strstr(b, "</body>");
            if (gt && (!be || gt < be)) mb_put(&out, gt + 1, (size_t)((be ? be : tmp.p + tmp.n) - gt - 1));
        } else {
            mb_put(&out, s, (size_t)(end - s));
        }
        mb_free(&tmp);
    } else {
        for (size_t i = 0; i < n; i++) {
            if (p[i] == '\n') mb_puts(&out, "<br/>");
            else mb_html_escape(&out, (const char *)p + i, 1);
        }
    }
    mb_cstr(&out);
    if (!md_results_has(r, e->title.p, out.p)) md_results_add(r, e->title.p, e->title.n, out.p, out.n);
    mb_free(&out);
    mb_free(&d.url); mb_free(&d.title);
}

/* Primer índice de título >= (ns, t). */
static uint32_t lower_bound(zim *z, char ns, const char *t, size_t tn)
{
    zdirent d = {0};
    uint32_t lo = 0, hi = z->n_titles, ui;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (dirent_by_title_index(z, mid, &d, &ui)) break;
        int c = z->titles_v1 ? 0 : (unsigned char)d.ns - (unsigned char)ns;
        if (!c) c = md_keycmp(d.title.p, d.title.n, t, tn);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    mb_free(&d.url); mb_free(&d.title);
    return lo;
}

static void search(zim *z, char ns, const char *t, int fuzzy, int max, md_results *r)
{
    size_t tn = strlen(t);
    if (!tn) return;
    zdirent d = {0};
    uint32_t ui;
    for (uint32_t i = lower_bound(z, ns, t, tn); i < z->n_titles && r->n < max; i++) {
        if (dirent_by_title_index(z, i, &d, &ui) || (!z->titles_v1 && d.ns != ns)) break;
        int m = fuzzy ? md_has_prefix(d.title.p, d.title.n, t, tn) : md_keycmp(d.title.p, d.title.n, t, tn) == 0;
        if (!m) break;
        emit(z, &d, r);
    }
    mb_free(&d.url); mb_free(&d.title);
}

/* Mayúsculas: inversa de md_lower para los casos habituales. */
static uint32_t upper(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x100 && c <= 0x17F && (c & 1)) return c - 1;
    return c;
}

static int zim_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    zim *z = d->priv;
    /* variantes: tal cual, minúsculas, mayúscula inicial, todo mayúsculas */
    mbuf v[4] = {{0}};
    const char *end = word + strlen(word);
    mb_puts(&v[0], word);
    for (const char *p = word; p < end;) {
        uint32_t c = utf8_next(&p, end);
        utf8_put(&v[1], md_lower(c));
        utf8_put(&v[2], v[2].n ? md_lower(c) : upper(md_lower(c)));
        utf8_put(&v[3], upper(md_lower(c)));
    }
    for (int i = 0; i < 4; i++) mb_cstr(&v[i]);
    static const char spaces[] = {'C', 'A'};
    for (int s = 0; s < (z->titles_v1 ? 1 : 2) && r->n < max; s++)
        for (int i = 0; i < 4 && r->n < max; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++) if (!strcmp(v[i].p, v[j].p)) dup = 1;
            if (!dup) search(z, spaces[s], v[i].p, fuzzy, max, r);
        }
    for (int i = 0; i < 4; i++) mb_free(&v[i]);
    return r->n;
}

const md_format_t md_fmt_zim = {"zim", zim_probe, zim_needs_index, zim_open, zim_close, zim_lookup};
