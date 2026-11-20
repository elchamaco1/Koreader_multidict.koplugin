/* QuickDic versión 6 (.quickdic, .quickdic.v006.zip).
 *
 * Formato (Java DataOutputStream, big-endian; cadenas en "modified UTF-8"):
 *   int versión, long fecha, string nombre
 *   lista fuentes, lista pares, lista textos, lista html, lista índices
 *   "END OF DICTIONARY"
 * Cada lista: int n, (n+1) long offsets (tabla de contenidos), elementos.
 * Gracias a esas tablas cualquier elemento se lee directamente.
 *
 * Cada índice (uno por idioma) tiene sus entradas ordenadas por una forma
 * normalizada de la palabra (transliteración ICU "Any-Latin", minúsculas, sin
 * acentos ni espacios). Para buscar sin ICU se transliteran latín, cirílico y
 * griego con tablas equivalentes y se hace búsqueda binaria directa. Si el
 * diccionario contiene otras escrituras, o su orden no coincide con el
 * nuestro, se crea una sola vez un índice auxiliar (.mdidx).
 *
 * Estructura según pyglossary/plugins/quickdic6.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "md_internal.h"

typedef struct {
    char *name;
    int swap;
    uint32_t n_entries;
    uint64_t entries_toc;   /* posición de la tabla de offsets de las entradas */
    uint32_t n_rows, row_size;
    uint64_t rows_off;
    int direct;             /* 1 = búsqueda binaria directa posible */
} qindex;

typedef struct {
    FILE *fp;
    uint64_t base;          /* desplazamiento dentro de un ZIP sin comprimir */
    uint32_t n_pairs, n_texts, n_htmls;
    uint64_t pairs_toc, texts_toc, htmls_toc;
    qindex *ix;
    int n_ix;
    sidx *aux;              /* índice auxiliar si algún índice no es directo */
    char *extracted;        /* ruta del .quickdic extraído de un ZIP comprimido */
} quickdic;

/* ---------------------------------------------------------------------- */
/* lectura                                                                 */
/* ---------------------------------------------------------------------- */

static int q_read(quickdic *q, uint64_t off, void *dst, size_t n)
{
    if (fseeko(q->fp, (off_t)(q->base + off), SEEK_SET)) return -1;
    return fread(dst, 1, n, q->fp) == n ? 0 : -1;
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static int r_int(quickdic *q, uint64_t *pos, uint32_t *v)
{
    uint8_t b[4];
    if (q_read(q, *pos, b, 4)) return -1;
    *v = (uint32_t)be(b, 4);
    *pos += 4;
    return 0;
}

static int r_long(quickdic *q, uint64_t *pos, uint64_t *v)
{
    uint8_t b[8];
    if (q_read(q, *pos, b, 8)) return -1;
    *v = be(b, 8);
    *pos += 8;
    return 0;
}

/* Modified UTF-8 de Java -> UTF-8 estándar (pares suplentes y NUL 0xC0 0x80). */
static void mutf8_to_utf8(const uint8_t *s, size_t n, mbuf *out)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c == 0xC0 && i + 1 < n && s[i + 1] == 0x80) { mb_putc(out, 0); i += 2; continue; }
        if (c == 0xED && i + 5 < n && (s[i + 1] & 0xF0) == 0xA0 && s[i + 3] == 0xED && (s[i + 4] & 0xF0) == 0xB0) {
            uint32_t hi = 0xD000 | ((uint32_t)(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            uint32_t lo = 0xD000 | ((uint32_t)(s[i + 4] & 0x3F) << 6) | (s[i + 5] & 0x3F);
            utf8_put(out, 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
            i += 6;
            continue;
        }
        mb_putc(out, (char)c);
        i++;
    }
    mb_cstr(out);
}

static int r_str(quickdic *q, uint64_t *pos, mbuf *out)
{
    uint8_t b[2];
    out->n = 0;
    if (q_read(q, *pos, b, 2)) return -1;
    size_t len = (size_t)be(b, 2);
    *pos += 2;
    uint8_t *tmp = malloc(len + 1);
    if (!tmp || (len && q_read(q, *pos, tmp, len))) { free(tmp); return -1; }
    *pos += len;
    mutf8_to_utf8(tmp, len, out);
    free(tmp);
    return 0;
}

/* Lee la cabecera de una lista: n y posición de su tabla; devuelve el final. */
static int r_list(quickdic *q, uint64_t *pos, uint32_t *n, uint64_t *toc)
{
    if (r_int(q, pos, n)) return -1;
    *toc = *pos;
    uint64_t end;
    uint64_t p = *toc + 8ull * *n;
    if (r_long(q, &p, &end)) return -1;
    *pos = end;
    return 0;
}

static int list_item(quickdic *q, uint64_t toc, uint32_t i, uint64_t *off)
{
    uint64_t p = toc + 8ull * i;
    return r_long(q, &p, off);
}

/* ---------------------------------------------------------------------- */
/* normalización (equivalente a ":: Any-Latin; ' ' > ; :: Lower; :: NFD;   */
/* :: [:Nonspacing Mark:] Remove;" para latín, cirílico y griego)          */
/* ---------------------------------------------------------------------- */

static const char *cyr_latin(uint32_t c)
{
    static const char *const ru[32] = {"a", "b", "v", "g", "d", "e", "z", "z", "i", "j", "k", "l", "m", "n", "o", "p",
                                       "r", "s", "t", "u", "f", "h", "c", "c", "s", "s", "", "y", "", "e", "u", "a"};
    if (c >= 0x430 && c <= 0x44F) return ru[c - 0x430];
    switch (c) {
    case 0x451: return "e";  /* ё */
    case 0x454: return "e";  /* є */
    case 0x456: return "i";  /* і */
    case 0x457: return "i";  /* ї */
    case 0x491: return "g";  /* ґ */
    case 0x452: return "d";  /* ђ */
    case 0x458: return "j";  /* ј */
    case 0x459: return "l";  /* љ */
    case 0x45A: return "n";  /* њ */
    case 0x45B: return "c";  /* ћ */
    case 0x45F: return "d";  /* џ */
    case 0x455: return "z";  /* ѕ */
    case 0x453: return "g";  /* ѓ */
    case 0x45C: return "k";  /* ќ */
    case 0x45E: return "u";  /* ў */
    }
    return NULL;
}

static const char *greek_latin(uint32_t c)
{
    static const char *const gr[25] = {"a", "b", "g", "d", "e", "z", "e", "th", "i", "k", "l", "m", "n", "x", "o", "p",
                                       "r", "s", "s", "t", "y", "ph", "ch", "ps", "o"};
    if (c >= 0x3B1 && c <= 0x3C9) return gr[c - 0x3B1];
    switch (c) {
    case 0x3AC: return "a"; case 0x3AD: return "e"; case 0x3AE: return "e"; case 0x3AF: return "i";
    case 0x3CC: return "o"; case 0x3CD: return "y"; case 0x3CE: return "o"; case 0x3CA: return "i";
    case 0x3CB: return "y"; case 0x390: return "i"; case 0x3B0: return "y";
    }
    return NULL;
}

static int is_other_script(uint32_t c)
{
    if (c < 0x370) return 0;                         /* latín y símbolos */
    if (c >= 0x370 && c <= 0x3FF) return 0;          /* griego */
    if (c >= 0x400 && c <= 0x52F) return 0;          /* cirílico */
    if (c >= 0x1E00 && c <= 0x1FFF) return 0;        /* latín extendido, griego extendido */
    if (c >= 0x2000 && c <= 0x2BFF) return 0;        /* puntuación y símbolos */
    return 1;
}

/* Orden de los signos en la colación ICU (no ignorables: van antes que los
 * dígitos y las letras). Obtenido de ICU 74 con las reglas de QuickDic. */
static const char *const ICU_PUNCT[] = {
    "_", "-", "\xE2\x80\x93", "\xE2\x80\x94", ",", ";", ":", "!", "\xC2\xA1", "?", "\xC2\xBF", ".", "\xC2\xB7",
    "'", "\xE2\x80\x99", "\"", "\xC2\xAB", "\xC2\xBB", "(", ")", "[", "]", "{", "}", "@", "*", "/", "\\", "&",
    "#", "%", "`", "^", "\xCA\xB9", "\xCA\xBA", "+", "<", "=", ">", "|", "~", "$", NULL};

static int punct_rank(uint32_t c)
{
    mbuf t = {0};
    utf8_put(&t, c);
    mb_cstr(&t);
    int r = 0;
    for (int i = 0; ICU_PUNCT[i]; i++)
        if (!strcmp(ICU_PUNCT[i], t.p)) { r = i + 1; break; }
    mb_free(&t);
    return r;
}

/* Clave de comparación: transliterada, minúsculas, sin acentos ni espacios;
 * los signos se convierten en bytes bajos con el orden de ICU. Devuelve 0 si
 * la palabra contiene escrituras que no sabemos transliterar. */
static int q_key(const char *s, mbuf *out)
{
    mbuf lat = {0};
    const char *p = s, *end = s + strlen(s);
    int ok = 1;
    while (p < end) {
        uint32_t c = md_lower(utf8_next(&p, end));
        if (c >= 0x400 && c <= 0x40F) c += 80;
        if (c >= 0x410 && c <= 0x42F) c += 32;
        const char *t = cyr_latin(c);
        if (!t) t = greek_latin(c);
        if (t) {
            if (*t) mb_puts(&lat, t);
            else utf8_put(&lat, c == 0x44C ? 0x2B9 : 0x2BA); /* ь ъ -> ʹ ʺ */
            continue;
        }
        if (is_other_script(c)) ok = 0;
        utf8_put(&lat, c);
    }
    out->n = 0;
    p = lat.p ? lat.p : "";
    end = p + lat.n;
    while (p < end) {
        const char *cs = p;
        uint32_t c = utf8_next(&p, end);
        if (c == ' ' || c == '\t' || c == 0xA0) continue;
        int r = (c < 0x80 && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) ? 0 : punct_rank(c);
        if (r) { mb_putc(out, (char)r); continue; }
        md_fold(cs, (size_t)(p - cs), FOLD_LOOSE, 0, out);
    }
    mb_cstr(out);
    mb_free(&lat);
    return ok;
}

/* ---------------------------------------------------------------------- */

typedef struct {
    mbuf token, norm;
    uint32_t start, count;
    uint64_t html_toc;
    uint32_t n_html;
} qentry;

static int read_entry(quickdic *q, qindex *x, uint32_t i, qentry *e, int full)
{
    uint64_t off;
    if (list_item(q, x->entries_toc, i, &off)) return -1;
    uint64_t p = off;
    if (r_str(q, &p, &e->token)) return -1;
    if (r_int(q, &p, &e->start) || r_int(q, &p, &e->count)) return -1;
    uint8_t hn;
    if (q_read(q, p, &hn, 1)) return -1;
    p++;
    e->norm.n = 0;
    if (hn && r_str(q, &p, &e->norm)) return -1;
    mb_cstr(&e->norm);
    if (full) {
        if (r_int(q, &p, &e->n_html)) return -1;
        e->html_toc = p;
    }
    return 0;
}

static void entry_key(qentry *e, mbuf *k)
{
    q_key(e->norm.n ? e->norm.p : e->token.p, k);
}

/* Comprueba con una muestra que el orden del índice coincide con q_key. */
static int check_direct(quickdic *q, qindex *x)
{
    if (x->n_entries < 2) return 1;
    qentry e = {0};
    mbuf prev = {0}, k = {0};
    int ok = 1, inversions = 0, samples = 0;
    uint32_t step = x->n_entries / 256 + 1;
    for (uint32_t i = 0; i < x->n_entries && ok; i += step) {
        if (read_entry(q, x, i, &e, 0)) { ok = 0; break; }
        if (!q_key(e.token.p, &k) && !e.norm.n) ok = 0;   /* escritura sin transliterar */
        entry_key(&e, &k);
        if (prev.n && md_keycmp(prev.p, prev.n, k.p, k.n) > 0) inversions++;
        prev.n = 0;
        mb_put(&prev, k.p, k.n);
        mb_cstr(&prev);
        samples++;
    }
    if (inversions * 20 > samples) ok = 0; /* más de un 5 % de desorden */
    mb_free(&e.token); mb_free(&e.norm); mb_free(&prev); mb_free(&k);
    return ok;
}

/* ---------------------------------------------------------------------- */

static int qd_probe(const char *p) { return ends_with_ci(p, ".quickdic") || ends_with_ci(p, ".quickdic.v006.zip"); }

static void qd_free(quickdic *q)
{
    if (!q) return;
    if (q->fp) fclose(q->fp);
    for (int i = 0; i < q->n_ix; i++) free(q->ix[i].name);
    free(q->ix);
    sidx_close(q->aux);
    free(q->extracted);
    free(q);
}

/* ZIP: localiza el .quickdic. Si está guardado sin comprimir devuelve su
 * offset; si está comprimido lo extrae (una sola vez) a `dst`. */
static int zip_locate(const char *path, const char *dst, uint64_t *base, int *extract_needed, int do_extract)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseeko(f, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftello(f);
    size_t tail = fsize < 70000 ? (size_t)fsize : 70000;
    uint8_t *buf = malloc(tail);
    fseeko(f, (off_t)(fsize - tail), SEEK_SET);
    int rc = -1;
    if (fread(buf, 1, tail, f) != tail) goto out;
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6) { eocd = i; break; }
    if (eocd < 0) goto out;
    uint32_t count = buf[eocd + 10] | (buf[eocd + 11] << 8);
    uint32_t cdsize = buf[eocd + 12] | (buf[eocd + 13] << 8) | (buf[eocd + 14] << 16) | ((uint32_t)buf[eocd + 15] << 24);
    uint32_t cdoff = buf[eocd + 16] | (buf[eocd + 17] << 8) | (buf[eocd + 18] << 16) | ((uint32_t)buf[eocd + 19] << 24);
    uint8_t *cd = malloc(cdsize ? cdsize : 1);
    fseeko(f, cdoff, SEEK_SET);
    if (fread(cd, 1, cdsize, f) != cdsize) { free(cd); goto out; }
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cdsize; i++) {
        uint32_t method = cd[p + 10] | (cd[p + 11] << 8);
        uint32_t csize = cd[p + 20] | (cd[p + 21] << 8) | (cd[p + 22] << 16) | ((uint32_t)cd[p + 23] << 24);
        uint32_t usize = cd[p + 24] | (cd[p + 25] << 8) | (cd[p + 26] << 16) | ((uint32_t)cd[p + 27] << 24);
        uint32_t nl = cd[p + 28] | (cd[p + 29] << 8), xl = cd[p + 30] | (cd[p + 31] << 8), cl = cd[p + 32] | (cd[p + 33] << 8);
        uint32_t lho = cd[p + 42] | (cd[p + 43] << 8) | (cd[p + 44] << 16) | ((uint32_t)cd[p + 45] << 24);
        char *name = md_strndup((const char *)cd + p + 46, nl);
        int is_qd = ends_with_ci(name, ".quickdic");
        free(name);
        p += 46 + nl + xl + cl;
        if (!is_qd) continue;
        uint8_t lh[30];
        fseeko(f, lho, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30) break;
        uint64_t data = (uint64_t)lho + 30 + (lh[26] | (lh[27] << 8)) + (lh[28] | (lh[29] << 8));
        if (method == 0) { *base = data; *extract_needed = 0; rc = 0; break; }
        if (method != 8) break;
        *extract_needed = 1;
        if (!do_extract) { rc = 0; break; }
        /* descompresión en memoria y escritura atómica */
        uint8_t *c = malloc(csize ? csize : 1);
        uint8_t *u = malloc(usize ? usize : 1);
        fseeko(f, (off_t)data, SEEK_SET);
        if (c && u && fread(c, 1, csize, f) == csize && md_inflate(c, csize, u, usize, 0) == (int)usize) {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s.tmp", dst);
            FILE *o = fopen(tmp, "wb");
            if (o && fwrite(u, 1, usize, o) == usize && !fclose(o) && !rename(tmp, dst)) rc = 0;
            else { if (o) fclose(o); unlink(tmp); }
        }
        free(c);
        free(u);
        break;
    }
    free(cd);
out:
    free(buf);
    fclose(f);
    return rc;
}

static char *extract_path(const char *path, const char *cache_dir)
{
    /* se guarda como un "índice": junto al ZIP o en la caché */
    char *ip = sidx_path_for(path, cache_dir, 1);
    size_t n = strlen(ip);
    if (n > 6 && !strcmp(ip + n - 6, ".mdidx")) ip[n - 6] = 0;
    mbuf b = {0};
    mb_puts(&b, ip);
    mb_puts(&b, ".quickdic");
    free(ip);
    return mb_steal(&b);
}

static int qd_needs_index(const char *path, const char *cache_dir)
{
    if (ends_with_ci(path, ".zip")) {
        uint64_t base = 0;
        int need = 0;
        if (zip_locate(path, NULL, &base, &need, 0)) return 0;
        if (need) {
            char *ep = extract_path(path, cache_dir);
            struct stat a, b;
            int fresh = !stat(ep, &a) && !stat(path, &b) && a.st_mtime >= b.st_mtime;
            free(ep);
            if (!fresh) return 1;
        }
    }
    /* el índice auxiliar solo se conoce tras abrir; si existe ya está creado */
    return 0;
}

/* Construye el índice auxiliar: clave = palabra, offset = (índice << 32) | entrada. */
static int build_aux(quickdic *q, md_dict *d, const char *cache_dir, char *err, int errlen)
{
    char *ip = sidx_path_for(d->path, cache_dir, 0);
    if (!sidx_is_fresh(ip, d->path)) {
        free(ip);
        ip = sidx_path_for(d->path, cache_dir, 1);
        if (!sidx_is_fresh(ip, d->path)) {
            sidx_builder *b = sidx_new();
            qentry e = {0};
            for (int k = 0; k < q->n_ix; k++) {
                qindex *x = &q->ix[k];
                if (x->direct) continue;
                for (uint32_t i = 0; i < x->n_entries; i++) {
                    if (read_entry(q, x, i, &e, 0)) break;
                    uint64_t ref = ((uint64_t)k << 32) | i;
                    sidx_add(b, e.token.p, e.token.n, ref, 0);
                    if (e.norm.n) sidx_add(b, e.norm.p, e.norm.n, ref, 0);
                }
            }
            mb_free(&e.token);
            mb_free(&e.norm);
            int rc = sidx_write(b, ip, d->path, d->name, NULL, NULL, err, errlen);
            sidx_free(b);
            if (rc) { free(ip); return -1; }
        }
    }
    q->aux = sidx_open(ip, NULL, NULL, NULL, NULL, err, errlen);
    free(ip);
    return q->aux ? 0 : -1;
}

static int qd_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    quickdic *q = calloc(1, sizeof *q);
    const char *file = d->path;
    if (ends_with_ci(d->path, ".zip")) {
        uint64_t base = 0;
        int need = 0;
        if (zip_locate(d->path, NULL, &base, &need, 0)) {
            md_seterr(err, errlen, "el ZIP no contiene un .quickdic: %s", d->path);
            qd_free(q);
            return -1;
        }
        if (need) {
            q->extracted = extract_path(d->path, cache_dir);
            struct stat a, b;
            int fresh = !stat(q->extracted, &a) && !stat(d->path, &b) && a.st_mtime >= b.st_mtime;
            if (!fresh && zip_locate(d->path, q->extracted, &base, &need, 1)) {
                md_seterr(err, errlen, "no se pudo descomprimir %s", d->path);
                qd_free(q);
                return -1;
            }
            file = q->extracted;
        } else {
            q->base = base;
        }
    }
    q->fp = fopen(file, "rb");
    if (!q->fp) { md_seterr(err, errlen, "no se puede abrir %s", file); qd_free(q); return -1; }

    mbuf s = {0};
    uint64_t pos = 0, created, toc;
    uint32_t ver, n;
    if (r_int(q, &pos, &ver) || ver < 6 || ver > 7 || r_long(q, &pos, &created) || r_str(q, &pos, &s)) goto bad;
    if (s.n) {
        /* pyglossary pone a veces un título genérico ("slob (EN->EN)"): entonces
         * se deja el nombre del archivo */
        static const char *const generic[] = {"slob", "mdx", "mdict", "dict", "dictionary", "untitled", "title", "tabfile", "stardict", NULL};
        size_t bn = s.n;
        const char *par = memchr(s.p, '(', s.n);
        if (par) bn = (size_t)(par - s.p);
        while (bn && s.p[bn - 1] == ' ') bn--;
        int gen = 0;
        for (int g = 0; generic[g]; g++) if (strlen(generic[g]) == bn && !strncasecmp(s.p, generic[g], bn)) gen = 1;
        if (!gen) { free(d->name); d->name = md_strndup(s.p, s.n); }
    }
    if (r_list(q, &pos, &n, &toc)) goto bad;                       /* fuentes */
    if (r_list(q, &pos, &q->n_pairs, &q->pairs_toc)) goto bad;
    if (r_list(q, &pos, &q->n_texts, &q->texts_toc)) goto bad;
    if (r_list(q, &pos, &q->n_htmls, &q->htmls_toc)) goto bad;
    uint32_t nix;
    uint64_t ix_toc;
    if (r_list(q, &pos, &nix, &ix_toc) || nix > 64) goto bad;
    q->ix = calloc(nix ? nix : 1, sizeof(qindex));
    long words = 0;
    int need_aux = 0;
    for (uint32_t k = 0; k < nix; k++) {
        qindex *x = &q->ix[q->n_ix++];
        uint64_t p;
        if (list_item(q, ix_toc, k, &p)) goto bad;
        if (r_str(q, &p, &s)) goto bad;                            /* nombre corto */
        x->name = md_strndup(s.p, s.n);
        if (r_str(q, &p, &s) || r_str(q, &p, &s)) goto bad;        /* nombre largo, iso */
        if (k == 0 && s.n) d->lang_from = md_strndup(s.p, s.n);
        if (k == 1 && s.n) d->lang_to = md_strndup(s.p, s.n);
        if (r_str(q, &p, &s)) goto bad;                            /* reglas del normalizador */
        uint8_t sw;
        if (q_read(q, p, &sw, 1)) goto bad;
        p++;
        x->swap = sw != 0;
        uint32_t mtc, stop_size;
        if (r_int(q, &p, &mtc)) goto bad;
        if (r_list(q, &p, &x->n_entries, &x->entries_toc)) goto bad;
        if (r_int(q, &p, &stop_size)) goto bad;
        p += stop_size;
        if (r_int(q, &p, &x->n_rows) || r_int(q, &p, &x->row_size)) goto bad;
        x->rows_off = p;
        if (!x->swap) words += x->n_entries;
        x->direct = check_direct(q, x);
        if (!x->direct) need_aux = 1;
    }
    d->word_count = words;
    mb_free(&s);
    d->is_html = 1;
    d->priv = q;
    if (need_aux && build_aux(q, d, cache_dir, err, errlen)) { d->priv = NULL; qd_free(q); return -1; }
    return 0;
bad:
    mb_free(&s);
    md_seterr(err, errlen, "archivo QuickDic dañado o no soportado: %s", d->path);
    qd_free(q);
    return -1;
}

static void qd_close(md_dict *d) { qd_free(d->priv); }

/* ---------------------------------------------------------------------- */
/* contenido                                                               */
/* ---------------------------------------------------------------------- */

static void put_text(mbuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') mb_puts(out, "<br/>");
        else mb_html_escape(out, s + i, 1);
    }
}

static void render_pairs(quickdic *q, uint32_t idx, int swap, mbuf *out)
{
    uint64_t p;
    if (idx >= q->n_pairs || list_item(q, q->pairs_toc, idx, &p)) return;
    uint32_t cnt;
    p += 2; /* src_idx */
    if (r_int(q, &p, &cnt)) return;
    mbuf a = {0}, b = {0};
    for (uint32_t i = 0; i < cnt && i < 4096; i++) {
        if (r_str(q, &p, &a) || r_str(q, &p, &b)) break;
        mbuf *l = swap ? &b : &a, *r = swap ? &a : &b;
        mb_puts(out, "<div><b>");
        put_text(out, l->p, l->n);
        mb_puts(out, "</b> — ");
        put_text(out, r->p, r->n);
        mb_puts(out, "</div>");
    }
    mb_free(&a);
    mb_free(&b);
}

static void render_text(quickdic *q, uint32_t idx, mbuf *out)
{
    uint64_t p;
    if (idx >= q->n_texts || list_item(q, q->texts_toc, idx, &p)) return;
    p += 2;
    mbuf t = {0};
    if (!r_str(q, &p, &t)) { mb_puts(out, "<div>"); put_text(out, t.p, t.n); mb_puts(out, "</div>"); }
    mb_free(&t);
}

/* Las definiciones HTML van en gzip; pyglossary además escapa \uXXXX. */
static void render_html(quickdic *q, uint32_t idx, mbuf *out)
{
    uint64_t p;
    if (idx >= q->n_htmls || list_item(q, q->htmls_toc, idx, &p)) return;
    p += 2;
    mbuf title = {0};
    uint32_t raw_len, clen;
    if (r_str(q, &p, &title) || r_int(q, &p, &raw_len) || r_int(q, &p, &clen) || clen > 64u * 1024 * 1024) { mb_free(&title); return; }
    uint8_t *c = malloc(clen ? clen : 1);
    if (c && !q_read(q, p, c, clen) && clen > 18 && c[0] == 0x1F && c[1] == 0x8B) {
        size_t hp = 10;
        if (c[3] & 4) hp += 2 + (c[10] | (c[11] << 8));
        if (c[3] & 8) { while (hp < clen && c[hp]) hp++; hp++; }
        if (c[3] & 16) { while (hp < clen && c[hp]) hp++; hp++; }
        if (c[3] & 2) hp += 2;
        uint32_t isize = c[clen - 4] | (c[clen - 3] << 8) | (c[clen - 2] << 16) | ((uint32_t)c[clen - 1] << 24);
        uint8_t *u = malloc(isize + 1);
        int r = u && hp < clen ? md_inflate(c + hp, clen - hp, u, isize, 0) : -1;
        if (r >= 0) {
            /* \uXXXX -> carácter */
            for (int i = 0; i < r; i++) {
                if (u[i] == '\\' && i + 5 < r && u[i + 1] == 'u') {
                    char hex[5] = {(char)u[i + 2], (char)u[i + 3], (char)u[i + 4], (char)u[i + 5], 0};
                    char *e;
                    unsigned long cp = strtoul(hex, &e, 16);
                    if (*e == 0) { utf8_put(out, (uint32_t)cp); i += 5; continue; }
                }
                mb_putc(out, (char)u[i]);
            }
        }
        free(u);
    }
    free(c);
    mb_free(&title);
}

/* Muestra lo que cuelga de una entrada del índice: filas y html asociados. */
static void render_entry(quickdic *q, qindex *x, uint32_t ei, mbuf *out, int depth)
{
    qentry e = {0};
    if (depth > 3 || read_entry(q, x, ei, &e, 1)) { mb_free(&e.token); mb_free(&e.norm); return; }
    for (uint32_t h = 0; h < e.n_html && h < 64; h++) {
        uint64_t p = e.html_toc + 8ull * h;
        uint64_t off;
        uint32_t hidx;
        if (r_long(q, &p, &off)) break;
        p = off;
        if (r_int(q, &p, &hidx)) break;
        render_html(q, hidx, out);
    }
    for (uint32_t r = e.start + 1; r <= e.start + e.count && r < x->n_rows; r++) {
        uint8_t row[16];
        if (x->row_size < 5 || x->row_size > 16 || q_read(q, x->rows_off + (uint64_t)r * x->row_size, row, x->row_size)) break;
        int type = (int8_t)row[0];
        uint32_t idx = (uint32_t)be(row + 1, 4);
        if (type == 0) render_pairs(q, idx, x->swap, out);
        else if (type == 2) render_text(q, idx, out);
        else if (type == 4) render_html(q, idx, out);
        else if ((type == 1 || type == 3) && idx != ei) render_entry(q, x, idx, out, depth + 1);
    }
    mb_free(&e.token);
    mb_free(&e.norm);
}

static void emit(quickdic *q, int k, uint32_t ei, const char *word, md_results *r)
{
    qindex *x = &q->ix[k];
    mbuf html = {0};
    render_entry(q, x, ei, &html, 0);
    mb_cstr(&html);
    if (html.n && !md_results_has(r, word, html.p)) md_results_add(r, word, strlen(word), html.p, html.n);
    mb_free(&html);
}

static int matches(const char *a, size_t an, const char *b, size_t bn, int fuzzy)
{
    return fuzzy ? md_has_prefix(a, an, b, bn) : md_keycmp(a, an, b, bn) == 0;
}

static void lookup_direct(quickdic *q, int k, const char *word, int fuzzy, int max, md_results *r)
{
    qindex *x = &q->ix[k];
    mbuf qk = {0}, ek = {0};
    q_key(word, &qk);
    if (!qk.n || !x->n_entries) { mb_free(&qk); return; }
    qentry e = {0};
    uint32_t lo = 0, hi = x->n_entries;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (read_entry(q, x, mid, &e, 0)) break;
        entry_key(&e, &ek);
        if (md_keycmp(ek.p, ek.n, qk.p, qk.n) < 0) lo = mid + 1;
        else hi = mid;
    }
    /* ventana: un poco hacia atrás (desempates de ICU) y hacia delante mientras coincida */
    uint32_t from = lo > 8 ? lo - 8 : 0;
    int misses = 0;
    for (uint32_t i = from; i < x->n_entries && r->n < max; i++) {
        if (read_entry(q, x, i, &e, 0)) break;
        entry_key(&e, &ek);
        if (matches(ek.p, ek.n, qk.p, qk.n, fuzzy)) {
            misses = 0;
            emit(q, k, i, e.token.p, r);
        } else if (i >= lo && ++misses > 8) {
            break;
        }
    }
    mb_free(&e.token); mb_free(&e.norm); mb_free(&qk); mb_free(&ek);
}

static int qd_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    quickdic *q = d->priv;
    mbuf tmp = {0};
    int direct_ok = q_key(word, &tmp);
    mb_free(&tmp);
    for (int k = 0; k < q->n_ix && r->n < max; k++)
        if (q->ix[k].direct && direct_ok) lookup_direct(q, k, word, fuzzy, max, r);
    if (q->aux && r->n < max) {
        sidx_hit hits[64];
        int n = sidx_find(q->aux, word, fuzzy, hits, max - r->n < 64 ? max - r->n : 64);
        for (int i = 0; i < n; i++) {
            int k = (int)(hits[i].off >> 32);
            uint32_t ei = (uint32_t)hits[i].off;
            if (k < q->n_ix) emit(q, k, ei, hits[i].word, r);
            free(hits[i].word);
        }
    }
    /* índices directos con una palabra en otra escritura: no se encontró arriba */
    if (!direct_ok && !q->aux && r->n == 0) {
        /* último recurso: recorrido lineal (diccionario pequeño o escritura rara) */
        qentry e = {0};
        mbuf a = {0}, b = {0};
        md_fold(word, strlen(word), FOLD_PLAIN, 0, &b);
        for (int k = 0; k < q->n_ix && r->n < max; k++) {
            qindex *x = &q->ix[k];
            for (uint32_t i = 0; i < x->n_entries && i < 200000 && r->n < max; i++) {
                if (read_entry(q, x, i, &e, 0)) break;
                a.n = 0;
                md_fold(e.token.p, e.token.n, FOLD_PLAIN, 0, &a);
                if (matches(a.p, a.n, b.p, b.n, fuzzy)) emit(q, k, i, e.token.p, r);
            }
        }
        mb_free(&e.token); mb_free(&e.norm); mb_free(&a); mb_free(&b);
    }
    return r->n;
}

const md_format_t md_fmt_quickdic = {"quickdic", qd_probe, qd_needs_index, qd_open, qd_close, qd_lookup};
