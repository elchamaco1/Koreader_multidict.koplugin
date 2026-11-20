/* Diccionarios de Apple (.dictionary, Diccionario de macOS).
 *
 * Un .dictionary es una carpeta: Contents/[Resources/]Body.data, KeyText.data,
 * KeyText.index, Info.plist... El índice nativo (KeyText.index, un árbol trie)
 * no está documentado, pero KeyText.data relaciona cada palabra con la
 * posición de su artículo en Body.data. Se lee una vez para crear el índice
 * auxiliar (.mdidx); luego cada artículo se lee directamente de Body.data
 * (secciones comprimidas con zlib).
 *
 * Formato según pyglossary/plugins/appledict_bin.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "md_internal.h"

#define APPLE_OFF 0x40

/* ---------------------------------------------------------------------- */
/* plist binario mínimo (bplist00)                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
    const uint8_t *d;
    size_t n;
    int osz, rsz;
    uint64_t nobj, top, otab;
} bplist;

static uint64_t bnum(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static int bp_init(bplist *b, const uint8_t *d, size_t n)
{
    if (n < 40 || memcmp(d, "bplist00", 8)) return -1;
    const uint8_t *t = d + n - 32;
    b->d = d; b->n = n;
    b->osz = t[6]; b->rsz = t[7];
    b->nobj = bnum(t + 8, 8); b->top = bnum(t + 16, 8); b->otab = bnum(t + 24, 8);
    if (b->osz < 1 || b->osz > 8 || b->rsz < 1 || b->rsz > 8 || b->otab + b->nobj * (uint64_t)b->osz > n) return -1;
    return 0;
}

static const uint8_t *bp_obj(bplist *b, uint64_t ref)
{
    if (ref >= b->nobj) return NULL;
    uint64_t off = bnum(b->d + b->otab + ref * (uint64_t)b->osz, b->osz);
    return off < b->n ? b->d + off : NULL;
}

/* Longitud de un objeto (con el entero extendido si el nibble es 0xF). */
static const uint8_t *bp_len(bplist *b, const uint8_t *o, uint64_t *len)
{
    uint64_t l = o[0] & 0x0F;
    const uint8_t *p = o + 1;
    if (l == 0x0F) {
        int w = 1 << (p[0] & 0x0F);
        l = bnum(p + 1, w);
        p += 1 + w;
    }
    *len = l;
    return p;
}

/* Valor de cadena (ASCII o UTF-16BE) como UTF-8. */
static int bp_str(bplist *b, uint64_t ref, mbuf *out)
{
    const uint8_t *o = bp_obj(b, ref);
    out->n = 0;
    if (!o) return -1;
    uint64_t len;
    const uint8_t *p = bp_len(b, o, &len);
    if ((o[0] >> 4) == 0x5) mb_put(out, p, (size_t)len);
    else if ((o[0] >> 4) == 0x6) utf16_to_utf8(p, (size_t)len * 2, 1, out);
    else return -1;
    mb_cstr(out);
    return 0;
}

static int64_t bp_int(bplist *b, uint64_t ref, int64_t def)
{
    const uint8_t *o = bp_obj(b, ref);
    if (!o) return def;
    if ((o[0] >> 4) == 0x1) return (int64_t)bnum(o + 1, 1 << (o[0] & 0x0F));
    if (o[0] == 0x09) return 1;
    if (o[0] == 0x08) return 0;
    return def;
}

/* Busca `key` en el diccionario `ref`; devuelve la referencia del valor o -1. */





/* ---------------------------------------------------------------------- */
/* Árbol común para plist binario y XML                                    */
/* ---------------------------------------------------------------------- */

enum { PN_NONE, PN_STR, PN_INT, PN_ARR, PN_DICT };
typedef struct pn {
    int t;
    int64_t i;
    char *s;
    int n;
    char **keys;        /* solo PN_DICT */
    struct pn **kids;
} pn;

static void pn_free(pn *x)
{
    if (!x) return;
    for (int i = 0; i < x->n; i++) { pn_free(x->kids[i]); if (x->keys) free(x->keys[i]); }
    free(x->kids); free(x->keys); free(x->s); free(x);
}

static void pn_add(pn *x, char *key, pn *kid)
{
    x->kids = realloc(x->kids, sizeof *x->kids * (x->n + 1));
    if (x->t == PN_DICT) { x->keys = realloc(x->keys, sizeof *x->keys * (x->n + 1)); x->keys[x->n] = key; }
    else free(key);
    x->kids[x->n++] = kid;
}

static pn *pn_get(pn *x, const char *k)
{
    if (!x || x->t != PN_DICT) return NULL;
    for (int i = 0; i < x->n; i++) if (x->keys[i] && !strcmp(x->keys[i], k)) return x->kids[i];
    return NULL;
}
static pn *pn_at(pn *x, int i) { return x && x->t == PN_ARR && i < x->n ? x->kids[i] : NULL; }
static int pn_count(pn *x) { return x && (x->t == PN_ARR || x->t == PN_DICT) ? x->n : 0; }
static int64_t pn_int(pn *x, int64_t def) { return x && x->t == PN_INT ? x->i : def; }
static const char *pn_str(pn *x) { return x && x->t == PN_STR ? x->s : NULL; }

static pn *pn_from_bp(bplist *b, uint64_t ref, int depth)
{
    pn *x = calloc(1, sizeof *x);
    const uint8_t *o = bp_obj(b, ref);
    if (!o || depth > 32) return x;
    int ty = o[0] >> 4;
    if (ty == 0x5 || ty == 0x6) {
        mbuf s = {0};
        if (!bp_str(b, ref, &s)) { x->t = PN_STR; x->s = md_strndup(s.p, s.n); }
        mb_free(&s);
    } else if (ty == 0x1 || o[0] == 0x08 || o[0] == 0x09) {
        x->t = PN_INT; x->i = bp_int(b, ref, 0);
    } else if (ty == 0xA || ty == 0xD) {
        uint64_t n;
        const uint8_t *p = bp_len(b, o, &n);
        x->t = ty == 0xA ? PN_ARR : PN_DICT;
        if (n > 100000) n = 0;
        for (uint64_t i = 0; i < n; i++) {
            char *key = NULL;
            uint64_t vr;
            if (ty == 0xD) {
                mbuf k = {0};
                if (!bp_str(b, bnum(p + i * b->rsz, b->rsz), &k)) key = md_strndup(k.p, k.n);
                mb_free(&k);
                vr = bnum(p + (n + i) * b->rsz, b->rsz);
            } else vr = bnum(p + i * b->rsz, b->rsz);
            pn_add(x, key, pn_from_bp(b, vr, depth + 1));
        }
    }
    return x;
}

/* texto de XML con entidades básicas */
static char *xml_unesc(const char *s, size_t n)
{
    mbuf o = {0};
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            static const struct { const char *e; char c; } E[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
            int hit = 0;
            for (int k = 0; k < 5; k++) {
                size_t l = strlen(E[k].e);
                if (i + l <= n && !memcmp(s + i, E[k].e, l)) { mb_put(&o, &E[k].c, 1); i += l - 1; hit = 1; break; }
            }
            if (hit) continue;
        }
        mb_put(&o, s + i, 1);
    }
    mb_cstr(&o);
    return mb_steal(&o);
}

/* lee el siguiente valor XML a partir de *pp */
static pn *pn_xml(const char **pp, const char *end, int depth)
{
    const char *p = *pp;
    for (;;) {
        while (p < end && *p != '<') p++;
        if (p + 1 >= end) { *pp = end; return NULL; }
        if (p[1] == '?' || p[1] == '!') { const char *g = memchr(p, '>', (size_t)(end - p)); p = g ? g + 1 : end; continue; }
        break;
    }
    const char *g = memchr(p, '>', (size_t)(end - p));
    if (!g) { *pp = end; return NULL; }
    char tag[16] = {0};
    size_t tl = 0;
    for (const char *q = p + 1; q < g && tl < 15 && *q != ' ' && (*q != '/' || q == p + 1) && *q != '>'; q++) tag[tl++] = *q;
    int selfclose = g[-1] == '/';
    p = g + 1;
    *pp = p;
    if (tag[0] == '/' || depth > 32) return NULL;
    if (!strcmp(tag, "plist")) return pn_xml(pp, end, depth + 1);
    pn *x = calloc(1, sizeof *x);
    if (!strcmp(tag, "true") || !strcmp(tag, "false")) { x->t = PN_INT; x->i = tag[0] == 't'; return x; }
    if (selfclose) { if (!strcmp(tag, "dict")) x->t = PN_DICT; else if (!strcmp(tag, "array")) x->t = PN_ARR; return x; }
    if (!strcmp(tag, "dict") || !strcmp(tag, "array")) {
        x->t = tag[0] == 'd' ? PN_DICT : PN_ARR;
        for (;;) {
            char *key = NULL;
            if (x->t == PN_DICT) {
                const char *k = strstr(*pp, "<key>");
                const char *c = strstr(*pp, "</dict>");
                if (!k || (c && c < k)) { *pp = c ? c + 7 : end; break; }
                const char *ke = strstr(k + 5, "</key>");
                if (!ke) { *pp = end; break; }
                key = xml_unesc(k + 5, (size_t)(ke - k - 5));
                *pp = ke + 6;
            }
            pn *v = pn_xml(pp, end, depth + 1);
            if (!v) { free(key); break; }   /* </array> o </dict> */
            pn_add(x, key, v);
        }
        return x;
    }
    char close[24];
    snprintf(close, sizeof close, "</%s>", tag);
    const char *e = strstr(p, close);
    if (!e) e = end;
    if (!strcmp(tag, "string")) { x->t = PN_STR; x->s = xml_unesc(p, (size_t)(e - p)); }
    else if (!strcmp(tag, "integer")) { x->t = PN_INT; x->i = strtoll(p, NULL, 10); }
    *pp = e < end ? e + strlen(close) : end;
    return x;
}

static pn *plist_parse(mbuf *raw)
{
    bplist b;
    if (!bp_init(&b, (uint8_t *)raw->p, raw->n)) return pn_from_bp(&b, b.top, 0);
    mb_cstr(raw);
    if (!strstr(raw->p, "<plist")) return NULL;
    const char *p = raw->p;
    return pn_xml(&p, raw->p + raw->n, 0);
}

/* ---------------------------------------------------------------------- */

typedef struct {
    FILE *body;
    int body_comp;          /* HeapDataCompressionType del cuerpo */
    int has_sections;
    int kt_comp, kt_fixed;
    sidx *idx;
    char *css;
    /* caché de una sección */
    long csec;
    mbuf sec;
} apple;

static int read_file(const char *path, mbuf *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseeko(f, 0, SEEK_END);
    off_t n = ftello(f);
    fseeko(f, 0, SEEK_SET);
    out->n = 0;
    int rc = (n >= 0 && !mb_reserve(out, (size_t)n) && fread(out->p, 1, (size_t)n, f) == (size_t)n) ? 0 : -1;
    if (!rc) out->n = (size_t)n;
    fclose(f);
    return rc;
}

static char *sibling(const char *path, const char *name)
{
    const char *sl = strrchr(path, '/');
    mbuf b = {0};
    if (sl) mb_put(&b, path, (size_t)(sl - path + 1));
    mb_puts(&b, name);
    return mb_steal(&b);
}

/* Carpeta "Contents" (Info.plist vive ahí, aunque Body.data esté en Resources/) */
static char *contents_dir(const char *path)
{
    char *p = md_strdup(path);
    char *sl = strrchr(p, '/');
    if (sl) *sl = 0;
    size_t n = strlen(p);
    if (n >= 10 && !strcmp(p + n - 10, "/Resources")) p[n - 10] = 0;
    return p;
}

static int apple_probe(const char *p)
{
    if (!ends_with_ci(p, "/Body.data")) return 0;
    return strstr(p, ".dictionary/") != NULL;
}

static int le_int(const uint8_t *p) { return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }
static uint32_t le16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }

/* desplazamiento inicial y límite del archivo (guessFileOffsetLimit) */
static void off_limit(const uint8_t *d, size_t n, uint64_t *start, uint64_t *limit)
{
    *limit = n >= APPLE_OFF + 4 ? (uint64_t)(uint32_t)le_int(d + APPLE_OFF) : 0;
    *start = (n >= APPLE_OFF + 12 && le_int(d + APPLE_OFF + 4) == 0 && le_int(d + APPLE_OFF + 8) == -1) ? 0x20 : 0x4;
}

/* Recorre KeyText.data y añade cada palabra al índice. */
static int parse_keytext(apple *a, const char *kt_path, sidx_builder *bld)
{
    mbuf raw = {0}, buf = {0};
    if (read_file(kt_path, &raw)) return -1;
    uint64_t start, limit, off, lim;
    off_limit((uint8_t *)raw.p, raw.n, &start, &limit);
    if (a->kt_comp > 0) {
        /* secciones comprimidas: se descomprimen todas en un búfer */
        uint64_t pos = start + APPLE_OFF;
        if (pos + 4 > raw.n) { mb_free(&raw); return -1; }
        uint32_t sec_len = (uint32_t)le_int((uint8_t *)raw.p + pos);
        pos += 4;
        uint64_t sec_off = pos;
        while (pos + 8 <= raw.n && pos < limit + APPLE_OFF) {
            uint32_t cl = (uint32_t)le_int((uint8_t *)raw.p + pos), dl = (uint32_t)le_int((uint8_t *)raw.p + pos + 4);
            if (!cl && !dl) break;
            if (cl < 4 || pos + 8 + cl - 4 > raw.n) break;
            size_t ol = 0;
            uint8_t *u = md_inflate_heap((uint8_t *)raw.p + pos + 8, cl - 4, &ol, 1);
            if (u) { mb_put(&buf, u, ol); free(u); }
            sec_off += (sec_len > cl + 4 ? sec_len : cl + 4);
            pos = sec_off;
        }
        off = 0;
        lim = buf.n;
    } else {
        if (raw.n > APPLE_OFF) mb_put(&buf, raw.p + APPLE_OFF, raw.n - APPLE_OFF);
        off = start;
        lim = limit < buf.n ? limit : buf.n;
    }
    mb_free(&raw);
    const uint8_t *d = (const uint8_t *)buf.p;
    mbuf w = {0};
    while (off + 6 < lim) {
        uint64_t p = off;
        int jump = le_int(d + p);
        p += 4;
        if (a->kt_comp == 0) p += 4;
        uint32_t forms = le16(d + p);
        p += 2;
        uint64_t next_lex = 0;
        for (uint32_t f = 0; f < forms && p + 2 <= lim; f++) {
            p += 2;
            if (next_lex) p = next_lex;
            uint32_t small = 0;
            while (!small && p + 2 <= lim) { small = le16(d + p); p += 2; }
            next_lex = p + small;
            uint32_t sec, chunk = 0;
            if (a->has_sections) { chunk = (uint32_t)le_int(d + p); sec = (uint32_t)le_int(d + p + 4); p += 8; }
            else { sec = (uint32_t)le_int(d + p); p += 4; }
            if (a->kt_fixed) p += 2;
            int field = 0;
            while (p + 2 <= next_lex && next_lex <= lim) {
                uint32_t l = le16(d + p);
                p += 2;
                if (l && p + l <= next_lex) {
                    /* campos: palabra clave, entrada, título, ancla, lectura */
                    if (field <= 2) {
                        w.n = 0;
                        utf16_to_utf8(d + p, l, 0, &w);
                        sidx_add(bld, w.p, w.n, ((uint64_t)sec << 32) | chunk, 0);
                    }
                    p += l;
                }
                field++;
            }
        }
        if (jump <= 0) break;
        off += (uint64_t)jump + 4;
    }
    mb_free(&w);
    mb_free(&buf);
    return 0;
}

static int apple_needs_index(const char *path, const char *cache_dir)
{
    char *ip = sidx_path_for(path, cache_dir, 0);
    int fresh = sidx_is_fresh(ip, path);
    free(ip);
    if (fresh) return 0;
    ip = sidx_path_for(path, cache_dir, 1);
    fresh = sidx_is_fresh(ip, path);
    free(ip);
    return !fresh;
}

static void apple_free(apple *a)
{
    if (!a) return;
    if (a->body) fclose(a->body);
    sidx_close(a->idx);
    free(a->css);
    mb_free(&a->sec);
    free(a);
}

static int apple_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    apple *a = calloc(1, sizeof *a);
    a->csec = -1;
    char *cdir = contents_dir(d->path);
    mbuf pl = {0}, s = {0};
    mbuf ipath = {0};
    mb_printf(&ipath, "%s/Info.plist", cdir);
    pn *top = NULL;
    if (read_file(mb_cstr(&ipath), &pl) || !(top = plist_parse(&pl))) {
        md_seterr(err, errlen, "no se puede leer Info.plist de %s", d->path);
        goto fail;
    }
    const char *nm = pn_str(pn_get(top, "CFBundleDisplayName"));
    if (!nm || !*nm) nm = pn_str(pn_get(top, "CFBundleName"));
    if (nm && *nm) { free(d->name); d->name = md_strdup(nm); }
    const char *css = pn_str(pn_get(top, "DCSDictionaryCSS"));
    if (css && *css) a->css = md_strdup(css);
    pn *idxs = pn_get(top, "IDXDictionaryIndexes");
    pn *kt = pn_at(idxs, 0), *bd = pn_at(idxs, 2);
    pn *fields = pn_get(kt, "IDXIndexDataFields");
    a->kt_fixed = pn_count(pn_get(fields, "IDXFixedDataFields"));
    int ext_size = (int)pn_int(pn_get(pn_at(pn_get(fields, "IDXExternalDataFields"), 0), "IDXDataSize"), 4);
    a->body_comp = (int)pn_int(pn_get(bd, "HeapDataCompressionType"), 0);
    a->has_sections = a->body_comp == 2 && ext_size == 8;
    a->kt_comp = (int)pn_int(pn_get(pn_get(kt, "TrieAuxiliaryDataOptions"), "HeapDataCompressionType"), 0);
    pn_free(top);
    top = NULL;
    if (a->kt_fixed > 1) { md_seterr(err, errlen, "AppleDict con campos desconocidos: %s", d->path); goto fail; }

    a->body = fopen(d->path, "rb");
    if (!a->body) { md_seterr(err, errlen, "no se puede abrir %s", d->path); goto fail; }

    /* índice auxiliar desde KeyText.data */
    char *ip = sidx_path_for(d->path, cache_dir, 0);
    if (!sidx_is_fresh(ip, d->path)) {
        free(ip);
        ip = sidx_path_for(d->path, cache_dir, 1);
        if (!sidx_is_fresh(ip, d->path)) {
            char *ktp = sibling(d->path, "KeyText.data");
            sidx_builder *bld = sidx_new();
            int rc = parse_keytext(a, ktp, bld);
            free(ktp);
            if (!rc) rc = sidx_write(bld, ip, d->path, d->name, NULL, NULL, err, errlen);
            else md_seterr(err, errlen, "no se puede leer KeyText.data de %s", d->path);
            sidx_free(bld);
            if (rc) { free(ip); goto fail; }
        }
    }
    a->idx = sidx_open(ip, NULL, NULL, NULL, &d->word_count, err, errlen);
    free(ip);
    if (!a->idx) goto fail;
    free(cdir);
    mb_free(&pl); mb_free(&s); mb_free(&ipath);
    d->is_html = 1;
    d->priv = a;
    return 0;
fail:
    free(cdir);
    mb_free(&pl); mb_free(&s); mb_free(&ipath);
    apple_free(a);
    return -1;
}

static void apple_close(md_dict *d) { apple_free(d->priv); }

/* Carga la sección que empieza en `sec` (relativa a APPLE_OFF). */
static int load_section(apple *a, uint32_t sec)
{
    if ((long)sec == a->csec) return 0;
    uint8_t h[12];
    if (fseeko(a->body, (off_t)sec + APPLE_OFF, SEEK_SET) || fread(h, 1, 12, a->body) < 8) return -1;
    uint32_t dlen = (uint32_t)le_int(h + 4);
    if (dlen > 256u << 20) return -1;
    a->sec.n = 0;
    if (a->body_comp > 0) {
        if (dlen < 4) return -1;
        uint8_t *c = malloc(dlen - 4);
        if (!c || fseeko(a->body, (off_t)sec + APPLE_OFF + 12, SEEK_SET) || fread(c, 1, dlen - 4, a->body) != dlen - 4) { free(c); return -1; }
        size_t ol = 0;
        uint8_t *u = md_inflate_heap(c, dlen - 4, &ol, 1);
        free(c);
        if (!u) return -1;
        mb_put(&a->sec, u, ol);
        free(u);
    } else {
        if (mb_reserve(&a->sec, dlen) || fseeko(a->body, (off_t)sec + APPLE_OFF + 8, SEEK_SET) ||
            fread(a->sec.p, 1, dlen, a->body) != dlen) return -1;
        a->sec.n = dlen;
    }
    a->csec = (long)sec;
    return 0;
}

/* Extrae el artículo que empieza en `pos` de la sección cargada. */
static int article_at(apple *a, size_t pos, const char **s, size_t *n)
{
    const char *buf = a->sec.p;
    size_t len = a->sec.n;
    if (pos >= len) return -1;
    size_t look = len - pos < 12 ? len - pos : 12;
    const char *e = NULL;
    for (size_t i = 0; i + 8 <= look; i++)
        if (!memcmp(buf + pos + i, "<d:entry", 8)) { e = buf + pos + i; break; }
    if (!e) return -1;
    size_t off = (size_t)(e - (buf + pos));
    size_t chunk;
    if (off == 0) {
        mbuf t = {0};
        mb_put(&t, buf + pos, len - pos);
        mb_cstr(&t);
        const char *end = strstr(t.p, "</d:entry>");
        chunk = end ? (size_t)(end - t.p) + 10 : len - pos;
        mb_free(&t);
    } else {
        uint8_t b4[4] = {0, 0, 0, 0};
        memcpy(b4 + (4 - (off < 4 ? off : 4)), buf + pos, off < 4 ? off : 4);
        if (off >= 4) memcpy(b4, buf + pos, 4);
        chunk = (uint32_t)le_int(b4);
    }
    if (pos + off + chunk > len) chunk = len - pos - off;
    *s = buf + pos + off;
    *n = chunk;
    return 0;
}

/* <d:entry> -> <div>, enlaces x-dictionary:d:palabra -> bword://palabra */
static void to_html(const char *s, size_t n, mbuf *out)
{
    mbuf t = {0};
    mb_put(&t, s, n);
    mb_cstr(&t);
    const char *p = t.p, *end = t.p + t.n;
    while (p < end) {
        const char *a = strstr(p, "<d:entry"), *b = strstr(p, "</d:entry>"), *c = strstr(p, "x-dictionary:d:");
        const char *nx = NULL;
        int kind = 0;
        if (a && (!nx || a < nx)) { nx = a; kind = 1; }
        if (b && (!nx || b < nx)) { nx = b; kind = 2; }
        if (c && (!nx || c < nx)) { nx = c; kind = 3; }
        if (!nx) { mb_put(out, p, (size_t)(end - p)); break; }
        mb_put(out, p, (size_t)(nx - p));
        if (kind == 1) {
            const char *gt = strchr(nx, '>');
            mb_puts(out, "<div>");
            p = gt ? gt + 1 : end;
        } else if (kind == 2) {
            mb_puts(out, "</div>");
            p = nx + 10;
        } else {
            mb_puts(out, "bword://");
            p = nx + 15;
        }
    }
    mb_cstr(out);
    mb_free(&t);
}

static int apple_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    apple *a = d->priv;
    sidx_hit hits[64];
    int n = sidx_find(a->idx, word, fuzzy, hits, max < 64 ? max : 64);
    mbuf html = {0};
    for (int i = 0; i < n; i++) {
        uint32_t sec = (uint32_t)(hits[i].off >> 32), chunk = (uint32_t)hits[i].off;
        const char *s;
        size_t len;
        if (!load_section(a, sec) && !article_at(a, chunk, &s, &len)) {
            html.n = 0;
            to_html(s, len, &html);
            if (!md_results_has(r, hits[i].word, html.p)) md_results_add(r, hits[i].word, strlen(hits[i].word), html.p, html.n);
        }
        free(hits[i].word);
    }
    mb_free(&html);
    return r->n;
}

const md_format_t md_fmt_apple = {"appledict", apple_probe, apple_needs_index, apple_open, apple_close, apple_lookup};
