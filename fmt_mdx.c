/* MDict (.mdx): versiones 1.2, 2.0 y 3.0.
 *
 * Todas las versiones traen su propio índice: una tabla pequeña con la primera
 * y la última clave de cada bloque de claves, y otra con el tamaño de cada
 * bloque de registros. Al abrir solo se leen esas dos tablas; en cada búsqueda
 * se descomprime un bloque de claves y el bloque de registros que toca. No se
 * genera ningún archivo auxiliar.
 *
 * 1.2 / 2.0: lógica basada en pyglossary/plugin_lib/readmdict.py y mdict-utils.
 * 3.0: estructura en "unidades" (contenido, índice de contenido, claves e
 *   índice de claves), cifrado Salsa20/8 con clave derivada del UUID por
 *   xxHash64, y compresiones LZO, zlib, LZMA, bzip2 y LZ4. El formato está
 *   documentado a partir de mdict-utils y de libmdx (raymanzhang/mdx).
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "md_internal.h"

enum { MENC_UTF8, MENC_UTF16, MENC_OTHER };

typedef struct {
    uint64_t comp_off;            /* posición de la cabecera de 8 bytes del bloque */
    uint64_t comp_size, decomp_size;
    char *first, *last;           /* claves normalizadas (FOLD_MDX) */
    char *lfirst, *llast;         /* claves laxas ignorando puntuación (FOLD_LOOSE) */
    char *tfirst, *tlast;         /* claves laxas cortadas en la puntuación (FOLD_LOOSE_T) */
} keyblk;

typedef struct {
    uint64_t comp_off, comp_size, decomp_size, decomp_start;
} recblk;

typedef struct {
    FILE *fp;
    double version;
    int v3;
    int nw;
    int enc;
    char encname[32];
    int encrypt;
    int strip_key, case_sens;
    uint8_t key[16];
    int has_key;
    keyblk *kb;
    size_t n_kb;
    recblk *rb;
    size_t n_rb;
    /* cachés de un bloque */
    long kcache_idx;
    mbuf kcache;
    long rcache_idx;
    mbuf rcache;
    mbuf tmp;
} mdx;

/* ---------------------------------------------------------------------- */

static uint64_t be_num(const uint8_t *p, int w)
{
    uint64_t v = 0;
    for (int i = 0; i < w; i++) v = (v << 8) | p[i];
    return v;
}

static int read_at(mdx *m, uint64_t off, void *dst, size_t n)
{
    if (fseeko(m->fp, (off_t)off, SEEK_SET)) return -1;
    return fread(dst, 1, n, m->fp) == n ? 0 : -1;
}

static void fast_decrypt(uint8_t *b, size_t n, const uint8_t *key, size_t keylen)
{
    uint8_t prev = 0x36;
    for (size_t i = 0; i < n; i++) {
        uint8_t bi = b[i];
        uint8_t t = (uint8_t)((bi >> 4) | (bi << 4));
        t = t ^ prev ^ (uint8_t)(i & 0xFF) ^ key[i % keylen];
        prev = bi;
        b[i] = t;
    }
}

/* Decodifica un bloque comprimido/cifrado de MDict a out (que se vacía).
 * Cabecera de 8 bytes: [compresión | cifrado<<4] [bytes cifrados] [2 reservados] [adler32] */
static int decode_block(mdx *m, uint8_t *blk, size_t n, size_t decomp_size, mbuf *out)
{
    if (n < 8) return -1;
    int comp = blk[0] & 0xF, encm = (blk[0] >> 4) & 0xF;
    size_t encsz = blk[1];
    uint8_t *data = blk + 8;
    size_t dn = n - 8;
    if (encm) {
        uint8_t key[16];
        if (m->has_key) memcpy(key, m->key, 16);
        else ripemd128(blk + 4, 4, key);
        size_t en = encsz < dn ? encsz : dn;
        if (encm == 1) fast_decrypt(data, en, key, 16);
        else if (encm == 2) md_salsa20_8(data, en, key);
        else return -2;
    }
    out->n = 0;
    if (comp == 0) decomp_size = dn;
    if (mb_reserve(out, decomp_size)) return -1;
    size_t ol = decomp_size;
    if (md_decompress(comp, data, dn, (uint8_t *)out->p, &ol)) return -3;
    out->n = ol;
    return 0;
}

/* Convierte texto en la codificación del diccionario a UTF-8. */
static void to_utf8(mdx *m, const char *s, size_t n, mbuf *out)
{
    if (m->enc == MENC_UTF8) mb_put(out, s, n);
    else if (m->enc == MENC_UTF16) utf16_to_utf8((const uint8_t *)s, n, 0, out);
    else iconv_to_utf8(m->encname, s, n, out);
    mb_cstr(out);
}

static void fold_key(mdx *m, const char *s, size_t n, mbuf *out)
{
    md_fold(s, n, m->strip_key ? FOLD_MDX : FOLD_PLAIN, m->case_sens, out);
}

/* Guarda las claves extremas de un bloque (texto crudo en la codificación del diccionario). */
static void set_block_keys(mdx *m, keyblk *k, const char *head, size_t hn, const char *tail, size_t tn)
{
    mbuf t = {0}, f = {0};
    to_utf8(m, head, hn, &t);
    fold_key(m, t.p, t.n, &f);
    k->first = md_strndup(f.p, f.n);
    f.n = 0;
    md_fold(t.p, t.n, FOLD_LOOSE, 0, &f);
    k->lfirst = md_strndup(f.p, f.n);
    f.n = 0;
    md_fold(t.p, t.n, FOLD_LOOSE_T, 0, &f);
    k->tfirst = md_strndup(f.p, f.n);
    t.n = f.n = 0;
    to_utf8(m, tail, tn, &t);
    fold_key(m, t.p, t.n, &f);
    k->last = md_strndup(f.p, f.n);
    f.n = 0;
    md_fold(t.p, t.n, FOLD_LOOSE, 0, &f);
    k->llast = md_strndup(f.p, f.n);
    f.n = 0;
    md_fold(t.p, t.n, FOLD_LOOSE_T, 0, &f);
    k->tlast = md_strndup(f.p, f.n);
    mb_free(&t);
    mb_free(&f);
}

/* ---------------------------------------------------------------------- */
/* cabecera                                                                */
/* ---------------------------------------------------------------------- */

static char *hdr_attr(const char *h, const char *name)
{
    char pat[64];
    snprintf(pat, sizeof pat, " %s=\"", name);
    const char *a = strstr(h, pat);
    if (!a) {
        snprintf(pat, sizeof pat, "%s=\"", name);
        a = strstr(h, pat);
        if (!a) return NULL;
    }
    a += strlen(pat);
    const char *b = strchr(a, '"');
    if (!b) return NULL;
    /* desescapa &lt; &gt; &quot; &amp; */
    mbuf o = {0};
    for (const char *p = a; p < b; p++) {
        if (*p == '&') {
            if (!strncmp(p, "&lt;", 4)) { mb_putc(&o, '<'); p += 3; continue; }
            if (!strncmp(p, "&gt;", 4)) { mb_putc(&o, '>'); p += 3; continue; }
            if (!strncmp(p, "&quot;", 6)) { mb_putc(&o, '"'); p += 5; continue; }
            if (!strncmp(p, "&amp;", 5)) { mb_putc(&o, '&'); p += 4; continue; }
        }
        mb_putc(&o, *p);
    }
    return mb_steal(&o);
}

static int yes(const char *v, int def)
{
    if (!v) return def;
    return !strcasecmp(v, "yes") || !strcmp(v, "1") || !strcasecmp(v, "true");
}

/* ---------------------------------------------------------------------- */

static int mdx_probe(const char *path) { return ends_with_ci(path, ".mdx"); }
static int mdx_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

static void mdx_free(mdx *m)
{
    if (!m) return;
    if (m->fp) fclose(m->fp);
    for (size_t i = 0; i < m->n_kb; i++) {
        free(m->kb[i].first); free(m->kb[i].last);
        free(m->kb[i].lfirst); free(m->kb[i].llast);
        free(m->kb[i].tfirst); free(m->kb[i].tlast);
    }
    free(m->kb);
    free(m->rb);
    mb_free(&m->kcache);
    mb_free(&m->rcache);
    mb_free(&m->tmp);
    free(m);
}

/* ---------------------------------------------------------------------- */
/* MDict 1.2 / 2.0                                                         */
/* ---------------------------------------------------------------------- */

static int open_v12(mdx *m, md_dict *d, uint64_t pos, char *err, int errlen)
{
    uint8_t *buf = NULL;
    mbuf kbi = {0};
    if (m->encrypt & 1) {
        md_seterr(err, errlen, "diccionario cifrado con registro (necesita clave de usuario)");
        return -1;
    }
    uint8_t kh[44];
    size_t khn = m->version >= 2.0 ? 44 : 16;
    if (read_at(m, pos, kh, khn)) goto bad;
    int w = m->nw;
    uint64_t num_kb = be_num(kh, w);
    uint64_t num_entries = be_num(kh + w, w);
    uint64_t kbi_decomp = 0, kbi_size, kb_size;
    if (m->version >= 2.0) {
        kbi_decomp = be_num(kh + 2 * w, w);
        kbi_size = be_num(kh + 3 * w, w);
        kb_size = be_num(kh + 4 * w, w);
    } else {
        kbi_size = be_num(kh + 2 * w, w);
        kb_size = be_num(kh + 3 * w, w);
    }
    d->word_count = (long)num_entries;
    pos += khn;
    if (kbi_size > 256u * 1024 * 1024 || num_kb > 10000000) goto bad;
    buf = malloc((size_t)kbi_size);
    if (read_at(m, pos, buf, (size_t)kbi_size)) goto bad;
    pos += kbi_size;
    if (m->version >= 2.0) {
        if (kbi_size < 8 || buf[0] > 5) goto bad;
        if (m->encrypt & 2) {
            uint8_t kin[8], key[16];
            memcpy(kin, buf + 4, 4);
            kin[4] = 0x95; kin[5] = 0x36; kin[6] = 0; kin[7] = 0;
            ripemd128(kin, 8, key);
            fast_decrypt(buf + 8, (size_t)kbi_size - 8, key, 16);
        }
        if (mb_reserve(&kbi, (size_t)kbi_decomp)) goto bad;
        /* casi siempre zlib (2), pero algunos generadores usan LZO (1) o nada (0) */
        size_t ol = (size_t)kbi_decomp;
        if (buf[0] == 0) ol = (size_t)kbi_size - 8;
        if (md_decompress(buf[0], buf + 8, (size_t)kbi_size - 8, (uint8_t *)kbi.p, &ol)) goto bad;
        kbi.n = ol;
    } else {
        mb_put(&kbi, buf, (size_t)kbi_size);
    }
    free(buf);
    buf = NULL;

    /* --- tabla de bloques de claves --- */
    m->kb = calloc((size_t)num_kb ? (size_t)num_kb : 1, sizeof(keyblk));
    {
        const uint8_t *p = (const uint8_t *)kbi.p, *end = p + kbi.n;
        int sw = m->version >= 2.0 ? 2 : 1, term = m->version >= 2.0 ? 1 : 0;
        int unit = m->enc == MENC_UTF16 ? 2 : 1;
        uint64_t off = pos;
        for (uint64_t i = 0; i < num_kb; i++) {
            if (p + w + sw > end) goto bad;
            p += w;
            size_t hl = (size_t)be_num(p, sw);
            p += sw;
            const uint8_t *head = p;
            p += (hl + term) * unit;
            if (p + sw > end) goto bad;
            size_t tl = (size_t)be_num(p, sw);
            p += sw;
            const uint8_t *tail = p;
            p += (tl + term) * unit;
            if (p + 2 * w > end) goto bad;
            keyblk *k = &m->kb[m->n_kb++];
            k->comp_size = be_num(p, w);
            k->decomp_size = be_num(p + w, w);
            p += 2 * w;
            k->comp_off = off;
            off += k->comp_size;
            set_block_keys(m, k, (const char *)head, hl * unit, (const char *)tail, tl * unit);
        }
    }
    pos += kb_size;

    /* --- tabla de bloques de registros --- */
    {
        uint8_t rh[32];
        if (read_at(m, pos, rh, 4 * (size_t)w)) goto bad;
        uint64_t num_rb = be_num(rh, w);
        uint64_t rbi_size = be_num(rh + 2 * w, w);
        pos += 4 * (uint64_t)w;
        if (rbi_size != num_rb * 2 * (uint64_t)w || rbi_size > 256u * 1024 * 1024) goto bad;
        buf = malloc((size_t)rbi_size + 1);
        if (read_at(m, pos, buf, (size_t)rbi_size)) goto bad;
        pos += rbi_size;
        m->rb = calloc((size_t)num_rb ? (size_t)num_rb : 1, sizeof(recblk));
        uint64_t coff = pos, dstart = 0;
        for (uint64_t i = 0; i < num_rb; i++) {
            recblk *r = &m->rb[m->n_rb++];
            r->comp_size = be_num(buf + i * 2 * w, w);
            r->decomp_size = be_num(buf + i * 2 * w + w, w);
            r->comp_off = coff;
            r->decomp_start = dstart;
            coff += r->comp_size;
            dstart += r->decomp_size;
        }
        free(buf);
        buf = NULL;
    }
    mb_free(&kbi);
    return 0;
bad:
    free(buf);
    mb_free(&kbi);
    md_seterr(err, errlen, "archivo MDict dañado o no soportado: %s", d->path);
    return -1;
}

/* ---------------------------------------------------------------------- */
/* MDict 3.0                                                               */
/* ---------------------------------------------------------------------- */

typedef struct {
    int found;
    uint64_t data_start, data_len;
} unit_t;

/* Lee un "storage block" v3 completo: u32 tamaño original, u32 longitud, datos. */
static int read_storage_block(mdx *m, uint64_t off, mbuf *out, uint64_t *next)
{
    uint8_t h[8];
    if (read_at(m, off, h, 8)) return -1;
    uint32_t orig = (uint32_t)be_num(h, 4), blen = (uint32_t)be_num(h + 4, 4);
    if (blen < 8 || blen > 512u * 1024 * 1024 || orig > 512u * 1024 * 1024) return -1;
    m->tmp.n = 0;
    if (mb_reserve(&m->tmp, blen) || read_at(m, off + 8, m->tmp.p, blen)) return -1;
    if (decode_block(m, (uint8_t *)m->tmp.p, blen, orig, out)) return -1;
    if (next) *next = off + 8 + blen;
    return 0;
}

static int open_v3(mdx *m, md_dict *d, uint64_t pos, char *err, int errlen)
{
    unit_t units[5] = {{0}};
    mbuf blk = {0};
    fseeko(m->fp, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftello(m->fp);

    /* Recorre las unidades. Cada una: u8 tipo, 3 reservados, u64 (longitud-12),
     * u32 nº de bloques, u64 longitud de datos; datos; bloque de información XML. */
    while (pos + 24 <= fsize) {
        uint8_t h[24];
        if (read_at(m, pos, h, 24)) break;
        int type = h[0];
        uint64_t data_len = be_num(h + 16, 8);
        uint64_t data_start = pos + 24;
        if (type < 1 || type > 4 || data_start + data_len > fsize) break;
        units[type].found = 1;
        units[type].data_start = data_start;
        units[type].data_len = data_len;
        /* tras los datos va el bloque de información; así se llega a la siguiente unidad
         * (el campo de longitud total no siempre se rellena) */
        uint8_t ih[8];
        if (read_at(m, data_start + data_len, ih, 8)) break;
        pos = data_start + data_len + 8 + be_num(ih + 4, 4);
    }
    if (!units[1].found || !units[2].found || !units[3].found || !units[4].found) {
        md_seterr(err, errlen, "MDict 3.0: falta alguna unidad (contenido o índices) en %s", d->path);
        goto fail;
    }

    /* índice de contenido: entradas (u64 longitud del bloque, u64 tamaño original) */
    if (read_storage_block(m, units[2].data_start, &blk, NULL) || blk.n % 16) goto bad;
    {
        size_t n = blk.n / 16;
        m->rb = calloc(n ? n : 1, sizeof(recblk));
        uint64_t off = units[1].data_start, dstart = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *e = (const uint8_t *)blk.p + 16 * i;
            uint64_t blen = be_num(e, 8), orig = be_num(e + 8, 8);
            if (blen < 16) goto bad;
            recblk *r = &m->rb[m->n_rb++];
            r->comp_off = off + 8;      /* tras u32 original + u32 longitud */
            r->comp_size = blen - 8;
            r->decomp_size = orig;
            r->decomp_start = dstart;
            off += blen;
            dstart += orig;
        }
    }

    /* índice de claves: u32 nº entradas, primera clave, última clave,
     * u32 longitud del bloque, u32 tamaño original */
    if (read_storage_block(m, units[4].data_start, &blk, NULL)) goto bad;
    {
        const uint8_t *p = (const uint8_t *)blk.p, *end = p + blk.n;
        int unit = m->enc == MENC_UTF16 ? 2 : 1;
        size_t cap = 64;
        m->kb = calloc(cap, sizeof(keyblk));
        uint64_t off = units[3].data_start;
        long total = 0;
        while (p + 4 + 2 <= end) {
            uint32_t cnt = (uint32_t)be_num(p, 4);
            p += 4;
            size_t hl = (size_t)be_num(p, 2) * unit;
            p += 2;
            const uint8_t *head = p;
            p += hl + unit;
            if (p + 2 > end) goto bad;
            size_t tl = (size_t)be_num(p, 2) * unit;
            p += 2;
            const uint8_t *tail = p;
            p += tl + unit;
            if (p + 8 > end) goto bad;
            uint64_t blen = be_num(p, 4), orig = be_num(p + 4, 4);
            p += 8;
            if (blen < 16) goto bad;
            if (m->n_kb == cap) {
                cap *= 2;
                keyblk *nk = realloc(m->kb, cap * sizeof(keyblk));
                if (!nk) goto bad;
                m->kb = nk;
            }
            keyblk *k = &m->kb[m->n_kb++];
            memset(k, 0, sizeof *k);
            k->comp_off = off + 8;
            k->comp_size = blen - 8;
            k->decomp_size = orig;
            off += blen;
            total += cnt;
            set_block_keys(m, k, (const char *)head, hl, (const char *)tail, tl);
        }
        d->word_count = total;
    }
    mb_free(&blk);
    return 0;
bad:
    md_seterr(err, errlen, "MDict 3.0 dañado o con un formato no soportado: %s", d->path);
fail:
    mb_free(&blk);
    return -1;
}

/* ---------------------------------------------------------------------- */

static int mdx_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    (void)cache_dir;
    mdx *m = calloc(1, sizeof *m);
    m->kcache_idx = m->rcache_idx = -1;
    m->fp = fopen(d->path, "rb");
    uint8_t *buf = NULL;
    mbuf hdr = {0};
    char *v = NULL;
    if (!m->fp) { md_seterr(err, errlen, "no se puede abrir %s", d->path); goto fail; }

    /* --- cabecera XML (UTF-16LE en 1.2/2.0, normalmente UTF-8 en 3.0) --- */
    uint8_t b4[4];
    if (read_at(m, 0, b4, 4)) goto bad;
    uint32_t hsize = (uint32_t)be_num(b4, 4);
    if (hsize < 2 || hsize > 16 * 1024 * 1024) goto bad;
    buf = malloc(hsize);
    if (read_at(m, 4, buf, hsize)) goto bad;
    if (buf[1] == 0) utf16_to_utf8(buf, hsize, 0, &hdr);
    else mb_put(&hdr, buf, hsize);
    mb_cstr(&hdr);
    while (hdr.n && hdr.p[hdr.n - 1] == 0) hdr.n--;
    mb_cstr(&hdr);
    free(buf);
    buf = NULL;
    uint64_t pos = 4 + (uint64_t)hsize + 4;

    v = hdr_attr(hdr.p, "GeneratedByEngineVersion");
    if (!v) v = hdr_attr(hdr.p, "RequiredEngineVersion");
    m->version = v ? atof(v) : 2.0;
    free(v);
    m->v3 = m->version >= 3.0;
    m->nw = m->version < 2.0 ? 4 : 8;

    v = hdr_attr(hdr.p, "Encoding");
    if (!v || !*v || !strcasecmp(v, "UTF-8") || !strcasecmp(v, "UTF8")) m->enc = MENC_UTF8;
    else if (!strncasecmp(v, "UTF-16", 6) || !strcasecmp(v, "UTF16")) m->enc = MENC_UTF16;
    else {
        m->enc = MENC_OTHER;
        snprintf(m->encname, sizeof m->encname, "%s",
                 (!strcasecmp(v, "GBK") || !strcasecmp(v, "GB2312")) ? "GB18030" : v);
    }
    free(v);

    v = hdr_attr(hdr.p, "Encrypted");
    m->encrypt = (!v || !strcasecmp(v, "No")) ? 0 : (!strcasecmp(v, "Yes") ? 1 : atoi(v));
    free(v);
    v = hdr_attr(hdr.p, "StripKey");
    m->strip_key = yes(v, 1);
    free(v);
    v = hdr_attr(hdr.p, "KeyCaseSensitive");
    m->case_sens = yes(v, 0);
    free(v);

    if (m->v3) {
        /* clave de cifrado: xxHash64 de cada mitad del UUID, en big-endian */
        v = hdr_attr(hdr.p, "UUID");
        if (v && *v) {
            size_t n = strlen(v), mid = (n + 1) / 2;
            uint64_t h1 = md_xxh64(v, mid), h2 = n > 1 ? md_xxh64(v + mid, n - mid) : 0;
            for (int i = 0; i < 8; i++) {
                m->key[i] = (uint8_t)(h1 >> (56 - 8 * i));
                m->key[8 + i] = (uint8_t)(h2 >> (56 - 8 * i));
            }
            m->has_key = 1;
        }
        free(v);
        v = hdr_attr(hdr.p, "RegCode");
        if (v && *v) {
            free(v);
            md_seterr(err, errlen, "diccionario MDict 3.0 con registro (necesita clave de usuario)");
            goto fail;
        }
        free(v);
    }

    v = hdr_attr(hdr.p, "Title");
    if (v && *v && !strstr(v, "No HTML code allowed")) {
        mbuf t = {0};
        for (const char *p = v; *p; p++) {
            if (*p == '<') { const char *q = strchr(p, '>'); if (!q) break; p = q; continue; }
            mb_putc(&t, *p);
        }
        free(d->name);
        d->name = mb_steal(&t);
    }
    free(v);
    v = NULL;
    d->is_html = 1;

    if ((m->v3 ? open_v3(m, d, pos, err, errlen) : open_v12(m, d, pos, err, errlen))) goto fail;

    mb_free(&hdr);
    d->priv = m;
    return 0;

bad:
    md_seterr(err, errlen, "archivo MDict dañado o no soportado: %s", d->path);
fail:
    free(buf);
    mb_free(&hdr);
    mdx_free(m);
    return -1;
}

static void mdx_close(md_dict *d) { mdx_free(d->priv); }

/* ---------------------------------------------------------------------- */

static int load_keyblock(mdx *m, size_t i)
{
    if ((long)i == m->kcache_idx) return 0;
    keyblk *k = &m->kb[i];
    m->tmp.n = 0;
    if (mb_reserve(&m->tmp, (size_t)k->comp_size)) return -1;
    if (read_at(m, k->comp_off, m->tmp.p, (size_t)k->comp_size)) return -1;
    m->kcache_idx = -1;
    if (decode_block(m, (uint8_t *)m->tmp.p, (size_t)k->comp_size, (size_t)k->decomp_size, &m->kcache)) return -1;
    m->kcache_idx = (long)i;
    return 0;
}

static int load_recblock(mdx *m, size_t i)
{
    if ((long)i == m->rcache_idx) return 0;
    recblk *r = &m->rb[i];
    m->tmp.n = 0;
    if (mb_reserve(&m->tmp, (size_t)r->comp_size)) return -1;
    if (read_at(m, r->comp_off, m->tmp.p, (size_t)r->comp_size)) return -1;
    m->rcache_idx = -1;
    if (decode_block(m, (uint8_t *)m->tmp.p, (size_t)r->comp_size, (size_t)r->decomp_size, &m->rcache)) return -1;
    m->rcache_idx = (long)i;
    return 0;
}

static size_t find_recblock(mdx *m, uint64_t off)
{
    size_t lo = 0, hi = m->n_rb;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->rb[mid].decomp_start + m->rb[mid].decomp_size <= off) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Obtiene el registro [rec_off, rec_end). rec_end = 0 si se desconoce: entonces
 * se corta en el terminador NUL o al final del bloque. Un registro puede ocupar
 * varios bloques. */
static int get_record(mdx *m, uint64_t rec_off, uint64_t rec_end, mbuf *out)
{
    mbuf raw = {0};
    size_t bi = find_recblock(m, rec_off);
    uint64_t cur = rec_off;
    int ok = -1;
    while (bi < m->n_rb) {
        if (load_recblock(m, bi)) break;
        recblk *r = &m->rb[bi];
        size_t a = (size_t)(cur - r->decomp_start);
        size_t b = m->rcache.n;
        if (rec_end > cur && rec_end - r->decomp_start < b) b = (size_t)(rec_end - r->decomp_start);
        if (a > b) break;
        mb_put(&raw, m->rcache.p + a, b - a);
        ok = 0;
        cur = r->decomp_start + b;
        /* continúa en el siguiente bloque solo si conocemos el final y no hemos llegado */
        if (!rec_end || cur >= rec_end || raw.n > 16 * 1024 * 1024) break;
        bi++;
    }
    if (ok) { mb_free(&raw); return -1; }
    const char *p = raw.p ? raw.p : "";
    size_t n = raw.n;
    if (m->enc == MENC_UTF16) {
        for (size_t i = 0; i + 1 < n; i += 2) if (!p[i] && !p[i + 1]) { n = i; break; }
    } else {
        const char *z = memchr(p, 0, n);
        if (z) n = (size_t)(z - p);
    }
    out->n = 0;
    to_utf8(m, p, n, out);
    while (out->n && (out->p[out->n - 1] == '\n' || out->p[out->n - 1] == '\r' || out->p[out->n - 1] == ' '))
        out->n--;
    mb_cstr(out);
    mb_free(&raw);
    return 0;
}

/* Offset de registro de la primera entrada del bloque de claves i (0 si no hay). */
static uint64_t first_record_offset(mdx *m, size_t i)
{
    if (i >= m->n_kb || load_keyblock(m, i) || m->kcache.n < (size_t)m->nw) return 0;
    return be_num((const uint8_t *)m->kcache.p, m->nw);
}

/* Reescribe enlaces internos de MDict para KOReader. */
static void fix_links(const char *s, size_t n, mbuf *out)
{
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *e = strstr(p, "entry://");
        if (!e || e >= end) { mb_put(out, p, (size_t)(end - p)); break; }
        mb_put(out, p, (size_t)(e - p));
        mb_puts(out, "bword://");
        p = e + 8;
    }
    mb_cstr(out);
}

static int mdx_find(mdx *m, const char *word, int fuzzy, int max, md_results *r, int depth);

static void emit(mdx *m, const char *word, size_t wn, mbuf *rec, md_results *r, int depth)
{
    if (rec->n > 8 && !strncmp(rec->p, "@@@LINK=", 8)) {
        if (depth >= 5) return;
        char *target = md_strndup(rec->p + 8, rec->n - 8);
        char *nl = strpbrk(target, "\r\n");
        if (nl) *nl = 0;
        md_results tmp = {0};
        mdx_find(m, target, 0, 4, &tmp, depth + 1);
        for (int i = 0; i < tmp.n; i++) {
            if (!md_results_has(r, tmp.words[i], tmp.defs[i]))
                md_results_add(r, word, wn, tmp.defs[i], strlen(tmp.defs[i]));
            free(tmp.words[i]);
            free(tmp.defs[i]);
        }
        free(tmp.words);
        free(tmp.defs);
        free(target);
        return;
    }
    mbuf fixed = {0};
    fix_links(rec->p, rec->n, &fixed);
    char *w = md_strndup(word, wn);
    if (!md_results_has(r, w, fixed.p)) md_results_add(r, word, wn, fixed.p, fixed.n);
    free(w);
    mb_free(&fixed);
}

/* Recorre un bloque de claves buscando q. Devuelve nº de aciertos añadidos. */
static int scan_block(mdx *m, size_t bi, const char *q, size_t qn, int fuzzy, int max, md_results *r, int depth)
{
    if (load_keyblock(m, bi)) return 0;
    /* copiamos el bloque: get_record y la búsqueda del siguiente bloque reutilizan cachés */
    mbuf kb = {0};
    mb_put(&kb, m->kcache.p, m->kcache.n);
    const uint8_t *p = (const uint8_t *)kb.p, *end = p + kb.n;
    int w = m->nw, unit = m->enc == MENC_UTF16 ? 2 : 1, added = 0;
    mbuf t = {0}, f = {0}, rec = {0};
    while (p + w < end && r->n < max) {
        uint64_t roff = be_num(p, w);
        const uint8_t *ks = p + w, *ke = ks;
        if (unit == 1) {
            while (ke < end && *ke) ke++;
        } else {
            while (ke + 1 < end && (ke[0] || ke[1])) ke += 2;
        }
        size_t kn = (size_t)(ke - ks);
        const uint8_t *next = ke + unit;
        t.n = f.n = 0;
        to_utf8(m, (const char *)ks, kn, &t);
        fold_key(m, t.p, t.n, &f);
        int match = fuzzy ? md_has_prefix(f.p, f.n, q, qn) : md_keycmp(f.p, f.n, q, qn) == 0;
        if (match) {
            uint64_t rend = 0;
            if (next + w <= end) rend = be_num(next, w);
            else rend = first_record_offset(m, bi + 1); /* última clave del bloque */
            if (get_record(m, roff, rend, &rec) == 0) {
                int before = r->n;
                emit(m, t.p, t.n, &rec, r, depth);
                added += r->n - before;
            }
        }
        p = next;
    }
    mb_free(&kb);
    mb_free(&t);
    mb_free(&f);
    mb_free(&rec);
    return added;
}

/* ¿Puede el bloque [first, last] contener q? Comparación con claves laxas.
 * Si el rango sale invertido (p. ej. una colación que pone el cirílico antes
 * que el latín), el bloque es frontera y se acepta q por cualquiera de los lados. */
static int range_may_contain(const char *first, const char *last, const char *q, size_t qn, int fuzzy)
{
    size_t fl = strlen(first), ll = strlen(last);
    int ge_first = md_keycmp(first, fl, q, qn) <= 0 || (fuzzy && md_has_prefix(first, fl, q, qn));
    int le_last = md_keycmp(q, qn, last, ll) <= 0 || md_has_prefix(last, ll, q, qn);
    if (md_keycmp(first, fl, last, ll) <= 0) return ge_first && le_last;
    return ge_first || le_last;
}

static int mdx_find(mdx *m, const char *word, int fuzzy, int max, md_results *r, int depth)
{
    if (!m->n_kb) return 0;
    mbuf q = {0}, lq = {0}, tq = {0};
    fold_key(m, word, strlen(word), &q);
    md_fold(word, strlen(word), FOLD_LOOSE, 0, &lq);
    md_fold(word, strlen(word), FOLD_LOOSE_T, 0, &tq);
    if (!q.n) { mb_free(&q); mb_free(&lq); mb_free(&tq); return 0; }

    /* Bloques candidatos:
     *  1) búsqueda binaria con nuestra normalización (lo-1 .. lo+1)
     *  2) bloques cuyo rango, comparado sin acentos ni mayúsculas, puede
     *     contener la palabra, tratando la puntuación como ignorada y como
     *     significativa. Cubre los diccionarios ordenados con otra colación
     *     (MDict 3.0 usa ICU según el idioma). */
    size_t cand[24];
    int nc = 0;
    size_t lo = 0, hi = m->n_kb;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (md_keycmp(m->kb[mid].last, strlen(m->kb[mid].last), q.p, q.n) < 0) lo = mid + 1;
        else hi = mid;
    }
    for (size_t i = lo > 0 ? lo - 1 : 0; i <= lo + 1 && i < m->n_kb && nc < 3; i++) cand[nc++] = i;
    for (size_t i = 0; i < m->n_kb && nc < 24; i++) {
        keyblk *k = &m->kb[i];
        int ok = (lq.n && range_may_contain(k->lfirst, k->llast, lq.p, lq.n, fuzzy)) ||
                 range_may_contain(k->tfirst, k->tlast, tq.p, tq.n, fuzzy);
        if (!ok) continue;
        int dup = 0;
        for (int j = 0; j < nc; j++) if (cand[j] == i) dup = 1;
        if (!dup) cand[nc++] = i;
    }
    /* en orden, para que los resultados salgan ordenados */
    for (int a = 1; a < nc; a++)
        for (int b = a; b > 0 && cand[b - 1] > cand[b]; b--) { size_t t = cand[b]; cand[b] = cand[b - 1]; cand[b - 1] = t; }

    int found = 0;
    for (int c = 0; c < nc && r->n < max; c++) found += scan_block(m, cand[c], q.p, q.n, fuzzy, max, r, depth);

    /* Segunda pasada si no apareció: bloques vecinos de los candidatos. Cubre
     * letras que cada idioma ordena a su manera (ñ en español, å/ä/ö en sueco,
     * ch en checo...), que desplazan un poco las fronteras entre bloques. */
    if (!found) {
        int extra = 0;
        for (int c = 0; c < nc && extra < 12 && r->n < max; c++) {
            for (int dlt = -2; dlt <= 2 && extra < 12; dlt++) {
                if (dlt == 0 || (dlt < 0 && cand[c] < (size_t)-dlt)) continue;
                size_t i = cand[c] + (size_t)dlt;
                if (i >= m->n_kb) continue;
                int seen = 0;
                for (int j = 0; j < nc; j++) if (cand[j] == i) seen = 1;
                if (seen) continue;
                if (nc < 24) cand[nc++] = i; /* lo marcamos como visto */
                found += scan_block(m, i, q.p, q.n, fuzzy, max, r, depth);
                extra++;
            }
        }
    }
    mb_free(&q);
    mb_free(&lq);
    mb_free(&tq);
    return found;
}

static int mdx_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    return mdx_find(d->priv, word, fuzzy, max, r, 0);
}

const md_format_t md_fmt_mdx = {"mdx", mdx_probe, mdx_needs_index, mdx_open, mdx_close, mdx_lookup};
