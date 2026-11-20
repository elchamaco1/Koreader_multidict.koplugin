/* Diccionarios de Kobo (dicthtml-XX.zip).
 *
 * Un dictzip de Kobo es un ZIP con:
 *   words          trie MARISA con las palabras (solo para autocompletar)
 *   PREFIJO.html   HTML comprimido con gzip, uno por prefijo de 2 letras
 *   imágenes       opcionales
 * Cada entrada es <w><a name="PALABRA"/>...<var><variant name="forma"/></var>...</w>.
 *
 * Como hace el propio Kobo: se calcula el prefijo de la palabra, se abre solo
 * ese archivo y se buscan las entradas cuya palabra o variante coincide. El
 * índice es el propio directorio del ZIP; no se genera nada aparte.
 *
 * Algoritmo de prefijos y formato según dictutil (pgaskin/dictutil, docs/dicthtml).
 * Los diccionarios oficiales de la tienda Kobo están cifrados (AES) con claves
 * privadas de Kobo y no se pueden leer.
 */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "md_internal.h"

typedef struct {
    char *name;
    uint32_t method, csize, usize;
    uint64_t lho; /* posición de la cabecera local */
} zentry;

typedef struct {
    FILE *fp;
    zentry *e;
    size_t n;
    /* caché del último archivo de prefijo descomprimido */
    long cached;
    mbuf html;
    int encrypted_seen;
} kobo;

static uint32_t le16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void kobo_free(kobo *k)
{
    if (!k) return;
    if (k->fp) fclose(k->fp);
    for (size_t i = 0; i < k->n; i++) free(k->e[i].name);
    free(k->e);
    mb_free(&k->html);
    free(k);
}

/* Lee el directorio central del ZIP. */
static int zip_read_dir(kobo *k, char *err, int errlen)
{
    fseeko(k->fp, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftello(k->fp);
    size_t tail = fsize < 70000 ? (size_t)fsize : 70000;
    uint8_t *buf = malloc(tail);
    fseeko(k->fp, (off_t)(fsize - tail), SEEK_SET);
    if (fread(buf, 1, tail, k->fp) != tail) { free(buf); return -1; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (le32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(buf); md_seterr(err, errlen, "no es un ZIP válido"); return -1; }
    uint32_t count = le16(buf + eocd + 10);
    uint32_t cdsize = le32(buf + eocd + 12);
    uint64_t cdoff = le32(buf + eocd + 16);
    free(buf);
    if (cdoff + cdsize > fsize || cdsize > 64u * 1024 * 1024) { md_seterr(err, errlen, "ZIP dañado"); return -1; }
    uint8_t *cd = malloc(cdsize ? cdsize : 1);
    fseeko(k->fp, (off_t)cdoff, SEEK_SET);
    if (fread(cd, 1, cdsize, k->fp) != cdsize) { free(cd); return -1; }
    k->e = calloc(count ? count : 1, sizeof(zentry));
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cdsize; i++) {
        if (le32(cd + p) != 0x02014b50) break;
        uint32_t nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (p + 46 + nl > cdsize) break;
        zentry *z = &k->e[k->n++];
        z->method = le16(cd + p + 10);
        z->csize = le32(cd + p + 20);
        z->usize = le32(cd + p + 24);
        z->lho = le32(cd + p + 42);
        z->name = md_strndup((const char *)cd + p + 46, nl);
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return 0;
}

static long zip_find(kobo *k, const char *name)
{
    for (size_t i = 0; i < k->n; i++)
        if (!strcmp(k->e[i].name, name)) return (long)i;
    return -1;
}

/* Extrae una entrada del ZIP (stored o deflate). */
static int zip_extract(kobo *k, size_t i, mbuf *out)
{
    zentry *z = &k->e[i];
    uint8_t lh[30];
    fseeko(k->fp, (off_t)z->lho, SEEK_SET);
    if (fread(lh, 1, 30, k->fp) != 30 || le32(lh) != 0x04034b50) return -1;
    uint64_t data = z->lho + 30 + le16(lh + 26) + le16(lh + 28);
    uint8_t *c = malloc(z->csize ? z->csize : 1);
    fseeko(k->fp, (off_t)data, SEEK_SET);
    if (fread(c, 1, z->csize, k->fp) != z->csize) { free(c); return -1; }
    out->n = 0;
    int rc = 0;
    if (z->method == 0) {
        mb_put(out, c, z->csize);
    } else if (z->method == 8) {
        if (mb_reserve(out, z->usize)) rc = -1;
        else {
            int r = md_inflate(c, z->csize, (uint8_t *)out->p, z->usize, 0);
            if (r < 0) rc = -1; else out->n = (size_t)r;
        }
    } else {
        rc = -1;
    }
    free(c);
    return rc;
}

/* Descomprime un gzip completo (cabecera + deflate). */
static int gunzip(const uint8_t *in, size_t n, mbuf *out)
{
    if (n < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return -1;
    int fl = in[3];
    size_t p = 10;
    if (fl & 4) { if (p + 2 > n) return -1; p += 2 + le16(in + p); }
    if (fl & 8) { while (p < n && in[p]) p++; p++; }
    if (fl & 16) { while (p < n && in[p]) p++; p++; }
    if (fl & 2) p += 2;
    if (p >= n) return -1;
    size_t isize = le32(in + n - 4);
    if (isize > 256u * 1024 * 1024) return -1;
    out->n = 0;
    if (mb_reserve(out, isize)) return -1;
    int r = md_inflate(in + p, n - p, (uint8_t *)out->p, isize, 0);
    if (r < 0) return -1;
    out->n = (size_t)r;
    mb_cstr(out);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* prefijo de Kobo                                                         */
/* ---------------------------------------------------------------------- */

static int is_space_u(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A); }
static int is_cyrillic(uint32_t c) { return (c >= 0x400 && c <= 0x52F) || (c >= 0x1C80 && c <= 0x1C8F) || (c >= 0x2DE0 && c <= 0x2DFF) || (c >= 0xA640 && c <= 0xA69F); }
static int is_han(uint32_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2FFFF) || (c >= 0xF900 && c <= 0xFAFF); }
static int is_kana(uint32_t c) { return c >= 0x3040 && c <= 0x30FF; }

/* Aproximación de unicode.IsLetter suficiente para prefijos. */
static int is_letter_u(uint32_t c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA;
    if (c == 0xD7 || c == 0xF7) return 0;
    if (c >= 0x2000 && c <= 0x2BFF) return 0;   /* puntuación, símbolos, flechas... */
    if (c >= 0x3000 && c <= 0x303F) return 0;   /* puntuación CJK */
    if (c >= 0xFF00 && c <= 0xFF20) return 0;   /* signos de ancho completo */
    if (c >= 0x300 && c <= 0x36F) return 0;     /* diacríticos combinantes */
    if (c >= 0x660 && c <= 0x669) return 0;     /* dígitos árabes */
    return 1;
}

/* Prefijo según el algoritmo de Kobo (DictionaryParser::htmlForWord). */
static void kobo_prefix(const char *word, mbuf *out)
{
    uint32_t c[2];
    int n = 0;
    const char *s = word, *end = word + strlen(word);
    while (s < end && n < 2) {
        uint32_t cp = utf8_next(&s, end);
        if (cp == 0) break;
        c[n++] = md_lower(cp);
    }
    int a = 0, b = n;
    while (a < b && is_space_u(c[a])) a++;
    while (b > a && is_space_u(c[b - 1])) b--;
    out->n = 0;
    if (a == b) { mb_puts(out, "11"); return; }
    uint32_t r[2] = {0, 0};
    int m = 0;
    for (int i = a; i < b; i++) r[m++] = c[i];
    if (!is_cyrillic(r[0])) {
        while (m < 2) r[m++] = 'a';
        if (!is_letter_u(r[0]) || !is_letter_u(r[1])) { mb_puts(out, "11"); return; }
    }
    for (int i = 0; i < m; i++) utf8_put(out, r[i]);
    mb_cstr(out);
}

/* ---------------------------------------------------------------------- */

static int kobo_probe(const char *path)
{
    if (!ends_with_ci(path, ".zip")) return 0;
    /* los .dsl.files.zip de Lingvo y otros ZIP de recursos no son diccionarios */
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (strncmp(b, "dicthtml", 8)) {
        /* sin el nombre típico, lo aceptamos solo si tiene "words" y algún .html */
        kobo k = {0};
        k.fp = fopen(path, "rb");
        if (!k.fp) return 0;
        int ok = 0;
        if (!zip_read_dir(&k, NULL, 0) && zip_find(&k, "words") >= 0)
            for (size_t i = 0; i < k.n && !ok; i++) ok = ends_with_ci(k.e[i].name, ".html");
        fclose(k.fp);
        for (size_t i = 0; i < k.n; i++) free(k.e[i].name);
        free(k.e);
        return ok;
    }
    return 1;
}

static int kobo_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

static int kobo_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    (void)cache_dir;
    kobo *k = calloc(1, sizeof *k);
    k->cached = -1;
    k->fp = fopen(d->path, "rb");
    if (!k->fp) { md_seterr(err, errlen, "no se puede abrir %s", d->path); kobo_free(k); return -1; }
    if (zip_read_dir(k, err, errlen)) { kobo_free(k); return -1; }
    size_t nhtml = 0;
    long first = -1;
    for (size_t i = 0; i < k->n; i++)
        if (ends_with_ci(k->e[i].name, ".html")) { nhtml++; if (first < 0) first = (long)i; }
    if (!nhtml) { md_seterr(err, errlen, "%s no es un diccionario de Kobo (no contiene archivos .html)", d->path); kobo_free(k); return -1; }

    /* comprobación de cifrado: el contenido descomprimido debe ser un gzip */
    mbuf raw = {0};
    if (zip_extract(k, (size_t)first, &raw) == 0 && raw.n >= 2 &&
        !((uint8_t)raw.p[0] == 0x1F && (uint8_t)raw.p[1] == 0x8B) && raw.p[0] != '<') {
        mb_free(&raw);
        md_seterr(err, errlen, "diccionario de Kobo cifrado (los de la tienda de Kobo no se pueden leer)");
        kobo_free(k);
        return -1;
    }
    mb_free(&raw);

    /* nombre: dicthtml-es-en.zip -> "Kobo es-en"; idiomas desde el nombre */
    const char *b = strrchr(d->path, '/');
    b = b ? b + 1 : d->path;
    if (!strncmp(b, "dicthtml-", 9)) {
        char loc[64];
        snprintf(loc, sizeof loc, "%s", b + 9);
        char *dot = strstr(loc, ".zip");
        if (dot) *dot = 0;
        mbuf nm = {0};
        mb_printf(&nm, "Kobo %s", loc);
        free(d->name);
        d->name = mb_steal(&nm);
        /* idiomas solo si son códigos ISO (2-3 letras): dicthtml-es-en.zip;
         * dicthtml-es-wiktionary.zip no da idioma de destino */
        char *dash = strchr(loc, '-');
        if (dash) *dash = 0;
        size_t l1 = strlen(loc), l2 = dash ? strlen(dash + 1) : 0;
        if (l1 >= 2 && l1 <= 3) {
            d->lang_from = md_strdup(loc);
            if (dash && l2 >= 2 && l2 <= 3) d->lang_to = md_strdup(dash + 1);
        }
    }
    d->word_count = -1;
    d->is_html = 1;
    d->priv = k;
    return 0;
}

static void kobo_close(md_dict *d) { kobo_free(d->priv); }

static int load_prefix(kobo *k, const char *prefix)
{
    char fn[64];
    snprintf(fn, sizeof fn, "%s.html", prefix);
    long i = zip_find(k, fn);
    if (i < 0) return -1;
    if (i == k->cached) return 0;
    mbuf raw = {0};
    k->cached = -1;
    int rc = zip_extract(k, (size_t)i, &raw);
    if (rc == 0) {
        if (raw.n >= 2 && (uint8_t)raw.p[0] == 0x1F && (uint8_t)raw.p[1] == 0x8B) rc = gunzip((uint8_t *)raw.p, raw.n, &k->html);
        else if (raw.n && raw.p[0] == '<') { k->html.n = 0; mb_put(&k->html, raw.p, raw.n); mb_cstr(&k->html); }
        else rc = -1;
    }
    mb_free(&raw);
    if (rc == 0) k->cached = i;
    return rc;
}

/* Desescapa entidades básicas de un atributo HTML. */
static void attr_text(const char *s, size_t n, mbuf *out)
{
    out->n = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            if (!strncmp(s + i, "&amp;", 5)) { mb_putc(out, '&'); i += 4; continue; }
            if (!strncmp(s + i, "&lt;", 4)) { mb_putc(out, '<'); i += 3; continue; }
            if (!strncmp(s + i, "&gt;", 4)) { mb_putc(out, '>'); i += 3; continue; }
            if (!strncmp(s + i, "&quot;", 6)) { mb_putc(out, '"'); i += 5; continue; }
            if (!strncmp(s + i, "&apos;", 6) || !strncmp(s + i, "&#39;", 5)) { mb_putc(out, '\''); i += s[i + 1] == 'a' ? 5 : 4; continue; }
        }
        mb_putc(out, s[i]);
    }
    mb_cstr(out);
}

/* Valor del atributo name="..." a partir de p (que apunta tras `name=`). */
static int read_name_attr(const char *p, const char *end, const char **vs, size_t *vn)
{
    while (p < end && (*p == ' ' || *p == '=')) p++;
    if (p >= end || (*p != '"' && *p != '\'')) return -1;
    char q = *p++;
    const char *e = memchr(p, q, (size_t)(end - p));
    if (!e) return -1;
    *vs = p;
    *vn = (size_t)(e - p);
    return 0;
}

/* strstr limitado a [p, end) */
static const char *bfind(const char *p, const char *end, const char *needle)
{
    size_t nl = strlen(needle);
    if (!p || p >= end) return NULL;
    for (const char *q = p; (size_t)(end - q) >= nl && (q = memchr(q, needle[0], (size_t)(end - q - (ptrdiff_t)nl + 1))); q++)
        if (!memcmp(q, needle, nl)) return q;
    return NULL;
}

/* Busca en el HTML del prefijo las entradas de `word`. fuzzy: las que empiezan por word. */
static void scan_prefix(kobo *k, const char *word, int fuzzy, int max, md_results *r)
{
    mbuf q = {0}, key = {0}, name = {0}, body = {0};
    md_fold(word, strlen(word), FOLD_PLAIN, 0, &q);
    const char *p = k->html.p, *end = k->html.p + k->html.n;
    while (p < end && r->n < max) {
        const char *w = bfind(p, end, "<w");
        if (!w) break;
        if (w[2] != '>' && w[2] != ' ') { p = w + 2; continue; }
        const char *we = bfind(w, end, "</w>");
        if (!we) we = end;
        const char *inner = strchr(w, '>');
        if (!inner || inner > we) { p = we; continue; }
        inner++;

        /* palabra principal y variantes */
        int match = 0;
        const char *headword_s = NULL;
        size_t headword_n = 0;
        for (const char *a = inner; a < we && !match;) {
            const char *an = bfind(a, we, "<a name");
            const char *vn = bfind(a, we, "<variant name");
            const char *t = NULL;
            int is_var = 0;
            if (an && an < we && (!vn || an < vn)) t = an + 7;
            else if (vn && vn < we) { t = vn + 13; is_var = 1; }
            if (!t) break;
            const char *vs;
            size_t vlen;
            if (read_name_attr(t, we, &vs, &vlen) == 0) {
                attr_text(vs, vlen, &name);
                if (!is_var && !headword_s) { headword_s = vs; headword_n = vlen; }
                key.n = 0;
                md_fold(name.p, name.n, FOLD_PLAIN, 0, &key);
                match = fuzzy ? md_has_prefix(key.p, key.n, q.p, q.n) : md_keycmp(key.p, key.n, q.p, q.n) == 0;
                a = vs + vlen;
            } else {
                a = t;
            }
        }
        if (match) {
            /* definición: el contenido del <w> sin el ancla ni la lista de variantes */
            body.n = 0;
            const char *s = inner;
            while (s < we) {
                const char *v = bfind(s, we, "<var>");
                const char *a = bfind(s, we, "<a name");
                const char *nx = NULL;
                int kind = 0;
                if (v && v < we && (!a || v < a)) { nx = v; kind = 1; }
                else if (a && a < we) { nx = a; kind = 2; }
                if (!nx) { mb_put(&body, s, (size_t)(we - s)); break; }
                mb_put(&body, s, (size_t)(nx - s));
                if (kind == 1) {
                    const char *ve = bfind(nx, we, "</var>");
                    s = ve && ve < we ? ve + 6 : we;
                } else {
                    /* el valor de name="" puede contener <sub>...</sub>: se salta entre comillas */
                    const char *vs;
                    size_t vlen;
                    const char *after = nx + 7;
                    if (read_name_attr(after, we, &vs, &vlen) == 0) after = vs + vlen + 1;
                    const char *gt = strchr(after, '>');
                    s = gt && gt < we ? gt + 1 : we;
                    if (s + 4 <= we && !strncmp(s, "</a>", 4)) s += 4;
                }
            }
            mb_cstr(&body);
            mbuf hw = {0};
            if (headword_s) attr_text(headword_s, headword_n, &hw);
            else mb_puts(&hw, word);
            if (!md_results_has(r, hw.p, body.p)) md_results_add(r, hw.p, hw.n, body.p, body.n);
            mb_free(&hw);
        }
        p = we + 4;
    }
    mb_free(&q);
    mb_free(&key);
    mb_free(&name);
    mb_free(&body);
}

static int kobo_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    kobo *k = d->priv;
    /* prefijo de Kobo; alternativas para diccionarios CJK hechos con otras
     * herramientas (un solo carácter han, o kana pasado a katakana) y "11" */
    mbuf pf = {0};
    kobo_prefix(word, &pf);
    char cands[4][32];
    int nc = 0;
    snprintf(cands[nc++], 32, "%s", pf.p);
    const char *s = word, *end = word + strlen(word);
    uint32_t c0 = s < end ? utf8_next(&s, end) : 0;
    if (is_han(c0)) {
        mbuf one = {0};
        utf8_put(&one, c0);
        snprintf(cands[nc++], 32, "%s", mb_cstr(&one));
        mb_free(&one);
    } else if (is_kana(c0)) {
        mbuf kk = {0};
        const char *t = word;
        for (int i = 0; i < 2 && t < end; i++) {
            uint32_t c = utf8_next(&t, end);
            if (c >= 0x3041 && c <= 0x3096) c += 0x60; /* hiragana -> katakana */
            utf8_put(&kk, c);
        }
        snprintf(cands[nc++], 32, "%s", mb_cstr(&kk));
        mb_free(&kk);
    }
    if (strcmp(pf.p, "11")) snprintf(cands[nc++], 32, "11");
    mb_free(&pf);

    for (int i = 0; i < nc && r->n < max; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (!strcmp(cands[i], cands[j])) dup = 1;
        if (dup || load_prefix(k, cands[i])) continue;
        scan_prefix(k, word, fuzzy, max, r);
    }
    return r->n;
}

const md_format_t md_fmt_kobo = {"kobo", kobo_probe, kobo_needs_index, kobo_open, kobo_close, kobo_lookup};
