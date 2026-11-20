/* Aard 2 (.slob).
 *
 *   "!-1SLOB\x1F", UUID (16), codificación, compresión (zlib|bz2|lzma2|""),
 *   etiquetas, tipos de contenido, nº de blobs, offset del almacén, tamaño
 *   lista de referencias: u32 n, n x u64 posiciones, refs
 *       ref = clave (u16 + texto), u32 bin, u16 elemento, fragmento (u8 + texto)
 *   almacén: u32 n, n x u64 posiciones, bins
 *       bin = u32 n, n x u8 tipo, u32 longitud, datos comprimidos
 *             (descomprimidos: n x u32 offsets, elementos u32 + bytes)
 *
 * Las referencias están ordenadas con la colación ICU raíz (signos y espacios
 * ignorables), así que se busca directamente con búsqueda binaria; no se crea
 * nada. Si el orden del archivo no encaja con el nuestro, índice auxiliar.
 * Estructura según pyglossary/plugins/aard2_slob.
 */
#include <stdlib.h>
#include <string.h>

#include "md_internal.h"

enum { SC_NONE, SC_ZLIB, SC_BZ2, SC_LZMA2 };

typedef struct {
    FILE *fp;
    int comp;
    char **ctypes;
    int n_ctypes;
    uint32_t n_refs;
    uint64_t refs_pos, refs_data;
    uint32_t n_bins;
    uint64_t bins_pos, bins_data;
    /* caché de un bin */
    long cbin;
    uint8_t *bin_data;
    size_t bin_len;
    uint32_t bin_count;
    uint8_t *bin_types;
    int direct;
    sidx *aux;
} slob;

static int s_read(slob *s, uint64_t off, void *dst, size_t n)
{
    if (fseeko(s->fp, (off_t)off, SEEK_SET)) return -1;
    return fread(dst, 1, n, s->fp) == n ? 0 : -1;
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

/* texto con longitud de 1 o 2 bytes; los de longitud máxima acaban en NUL */
static int s_text(slob *s, uint64_t *pos, int lw, mbuf *out)
{
    uint8_t b[2];
    out->n = 0;
    if (s_read(s, *pos, b, (size_t)lw)) return -1;
    size_t n = (size_t)be(b, lw);
    *pos += (uint64_t)lw;
    if (mb_reserve(out, n) || (n && s_read(s, *pos, out->p, n))) return -1;
    *pos += n;
    out->n = n;
    if (n == (lw == 1 ? 255u : 65535u)) {
        char *z = memchr(out->p, 0, n);
        if (z) out->n = (size_t)(z - out->p);
    }
    mb_cstr(out);
    return 0;
}

typedef struct {
    mbuf key, frag;
    uint32_t bin;
    uint16_t item;
} sref;

static int read_ref(slob *s, uint32_t i, sref *r)
{
    uint8_t b[8];
    if (s_read(s, s->refs_pos + 8ull * i, b, 8)) return -1;
    uint64_t pos = s->refs_data + be(b, 8);
    if (s_text(s, &pos, 2, &r->key)) return -1;
    uint8_t bi[6];
    if (s_read(s, pos, bi, 6)) return -1;
    r->bin = (uint32_t)be(bi, 4);
    r->item = (uint16_t)be(bi + 4, 2);
    pos += 6;
    return s_text(s, &pos, 1, &r->frag);
}

static void key_of(const char *w, size_t n, mbuf *k)
{
    k->n = 0;
    md_fold(w, n, FOLD_ROOT, 0, k);
}

static int load_bin(slob *s, uint32_t bi)
{
    if ((long)bi == s->cbin) return 0;
    if (bi >= s->n_bins) return -1;
    uint8_t b[8];
    if (s_read(s, s->bins_pos + 8ull * bi, b, 8)) return -1;
    uint64_t pos = s->bins_data + be(b, 8);
    uint8_t c4[4];
    if (s_read(s, pos, c4, 4)) return -1;
    uint32_t count = (uint32_t)be(c4, 4);
    pos += 4;
    if (count > 65536) return -1;
    uint8_t *types = malloc(count ? count : 1);
    if (s_read(s, pos, types, count)) { free(types); return -1; }
    pos += count;
    if (s_read(s, pos, c4, 4)) { free(types); return -1; }
    uint32_t clen = (uint32_t)be(c4, 4);
    pos += 4;
    uint8_t *c = malloc(clen ? clen : 1);
    if (!c || s_read(s, pos, c, clen)) { free(c); free(types); return -1; }
    size_t ulen = 0;
    uint8_t *u = NULL;
    switch (s->comp) {
    case SC_NONE: u = c; ulen = clen; c = NULL; break;
    case SC_ZLIB: u = md_inflate_heap(c, clen, &ulen, 1); break;
    case SC_BZ2: u = md_bzip2_heap(c, clen, &ulen); break;
    case SC_LZMA2: u = md_lzma2_raw_heap(c, clen, &ulen); break;
    }
    free(c);
    if (!u) { free(types); return -1; }
    free(s->bin_data);
    free(s->bin_types);
    s->bin_data = u;
    s->bin_len = ulen;
    s->bin_count = count;
    s->bin_types = types;
    s->cbin = (long)bi;
    return 0;
}

/* Contenido del elemento `item` del bin cargado; devuelve el tipo o NULL. */
static const char *bin_item(slob *s, uint32_t item, const uint8_t **data, size_t *len)
{
    if (item >= s->bin_count || 4ull * s->bin_count > s->bin_len) return NULL;
    uint64_t off = 4ull * s->bin_count + be(s->bin_data + 4ull * item, 4);
    if (off + 4 > s->bin_len) return NULL;
    uint32_t n = (uint32_t)be(s->bin_data + off, 4);
    if (off + 4 + n > s->bin_len) return NULL;
    *data = s->bin_data + off + 4;
    *len = n;
    int t = s->bin_types[item];
    return t < s->n_ctypes ? s->ctypes[t] : "";
}

/* ---------------------------------------------------------------------- */

static int slob_probe(const char *p) { return ends_with_ci(p, ".slob"); }

static void slob_free(slob *s)
{
    if (!s) return;
    if (s->fp) fclose(s->fp);
    for (int i = 0; i < s->n_ctypes; i++) free(s->ctypes[i]);
    free(s->ctypes);
    free(s->bin_data);
    free(s->bin_types);
    sidx_close(s->aux);
    free(s);
}

static int check_order(slob *s)
{
    if (s->n_refs < 2) return 1;
    sref r = {0};
    mbuf prev = {0}, k = {0};
    int inv = 0, samples = 0;
    uint32_t step = s->n_refs / 256 + 1;
    for (uint32_t i = 0; i < s->n_refs; i += step) {
        if (read_ref(s, i, &r)) { inv = 1000; break; }
        key_of(r.key.p, r.key.n, &k);
        if (prev.n && md_keycmp(prev.p, prev.n, k.p, k.n) > 0) inv++;
        prev.n = 0;
        mb_put(&prev, k.p, k.n);
        samples++;
    }
    mb_free(&r.key); mb_free(&r.frag); mb_free(&prev); mb_free(&k);
    return inv * 20 <= samples;
}

static int slob_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

static int slob_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    slob *s = calloc(1, sizeof *s);
    s->cbin = -1;
    s->fp = fopen(d->path, "rb");
    mbuf t = {0};
    if (!s->fp) { md_seterr(err, errlen, "no se puede abrir %s", d->path); slob_free(s); return -1; }
    uint8_t h[24];
    if (s_read(s, 0, h, 24) || memcmp(h, "!-1SLOB\x1F", 8)) goto bad;
    uint64_t pos = 24;
    if (s_text(s, &pos, 1, &t)) goto bad;                    /* codificación */
    if (t.n && strcasecmp(t.p, "utf-8") && strcasecmp(t.p, "utf8")) {
        md_seterr(err, errlen, "Slob con codificación %s no soportada", t.p);
        goto fail;
    }
    if (s_text(s, &pos, 1, &t)) goto bad;                    /* compresión */
    if (!t.n) s->comp = SC_NONE;
    else if (!strcmp(t.p, "zlib")) s->comp = SC_ZLIB;
    else if (!strcmp(t.p, "bz2")) s->comp = SC_BZ2;
    else if (!strcmp(t.p, "lzma2")) s->comp = SC_LZMA2;
    else { md_seterr(err, errlen, "compresión Slob desconocida: %s", t.p); goto fail; }
    uint8_t cnt;
    if (s_read(s, pos, &cnt, 1)) goto bad;
    pos++;
    mbuf v = {0};
    for (int i = 0; i < cnt; i++) {
        if (s_text(s, &pos, 1, &t) || s_text(s, &pos, 1, &v)) { mb_free(&v); goto bad; }
        if (!strcmp(t.p, "label") && v.n) { free(d->name); d->name = md_strndup(v.p, v.n); }
        else if (!strcmp(t.p, "source.lang") && v.n) d->lang_from = md_strndup(v.p, v.n);
        else if (!strcmp(t.p, "target.lang") && v.n) d->lang_to = md_strndup(v.p, v.n);
    }
    mb_free(&v);
    if (s_read(s, pos, &cnt, 1)) goto bad;
    pos++;
    s->ctypes = calloc(cnt ? cnt : 1, sizeof(char *));
    for (int i = 0; i < cnt; i++) {
        if (s_text(s, &pos, 2, &t)) goto bad;
        s->ctypes[s->n_ctypes++] = md_strndup(t.p, t.n);
    }
    uint8_t b[20];
    if (s_read(s, pos, b, 20)) goto bad;
    uint64_t store_off = be(b + 4, 8);
    pos += 20;
    uint8_t c4[4];
    if (s_read(s, pos, c4, 4)) goto bad;
    s->n_refs = (uint32_t)be(c4, 4);
    s->refs_pos = pos + 4;
    s->refs_data = s->refs_pos + 8ull * s->n_refs;
    if (s_read(s, store_off, c4, 4)) goto bad;
    s->n_bins = (uint32_t)be(c4, 4);
    s->bins_pos = store_off + 4;
    s->bins_data = s->bins_pos + 8ull * s->n_bins;
    mb_free(&t);
    d->word_count = s->n_refs;
    d->is_html = 1;
    s->direct = check_order(s);
    if (!s->direct) {
        /* índice auxiliar: clave -> nº de referencia */
        char *ip = sidx_path_for(d->path, cache_dir, 0);
        if (!sidx_is_fresh(ip, d->path)) {
            free(ip);
            ip = sidx_path_for(d->path, cache_dir, 1);
            if (!sidx_is_fresh(ip, d->path)) {
                sidx_builder *bld = sidx_new();
                sref r = {0};
                for (uint32_t i = 0; i < s->n_refs; i++)
                    if (!read_ref(s, i, &r)) sidx_add(bld, r.key.p, r.key.n, i, 0);
                mb_free(&r.key); mb_free(&r.frag);
                int rc = sidx_write(bld, ip, d->path, d->name, NULL, NULL, err, errlen);
                sidx_free(bld);
                if (rc) { free(ip); slob_free(s); return -1; }
            }
        }
        s->aux = sidx_open(ip, NULL, NULL, NULL, NULL, err, errlen);
        free(ip);
        if (!s->aux) { slob_free(s); return -1; }
    }
    d->priv = s;
    return 0;
bad:
    md_seterr(err, errlen, "archivo Slob dañado: %s", d->path);
fail:
    mb_free(&t);
    slob_free(s);
    return -1;
}

static void slob_close(md_dict *d) { slob_free(d->priv); }

static void emit(slob *s, sref *r, md_results *res)
{
    const uint8_t *data;
    size_t len;
    if (load_bin(s, r->bin)) return;
    const char *ct = bin_item(s, r->item, &data, &len);
    if (!ct) return;
    mbuf out = {0};
    if (!strncmp(ct, "text/html", 9)) {
        mb_put(&out, data, len);
    } else if (!strncmp(ct, "text/", 5)) {
        for (size_t i = 0; i < len; i++) {
            if (data[i] == '\n') mb_puts(&out, "<br/>");
            else mb_html_escape(&out, (const char *)data + i, 1);
        }
    } else {
        return; /* imágenes, css, js: recursos, no definiciones */
    }
    mb_cstr(&out);
    if (!md_results_has(res, r->key.p, out.p)) md_results_add(res, r->key.p, r->key.n, out.p, out.n);
    mb_free(&out);
}

static int slob_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *res)
{
    slob *s = d->priv;
    sref r = {0};
    if (s->aux) {
        sidx_hit hits[64];
        int n = sidx_find(s->aux, word, fuzzy, hits, max < 64 ? max : 64);
        for (int i = 0; i < n; i++) {
            if (!read_ref(s, (uint32_t)hits[i].off, &r)) emit(s, &r, res);
            free(hits[i].word);
        }
        mb_free(&r.key); mb_free(&r.frag);
        return res->n;
    }
    mbuf qk = {0}, k = {0}, qp = {0}, kp = {0};
    key_of(word, strlen(word), &qk);
    md_fold(word, strlen(word), FOLD_PLAIN, 0, &qp);
    if (!qk.n) { mb_free(&qk); mb_free(&qp); return 0; }
    uint32_t lo = 0, hi = s->n_refs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (read_ref(s, mid, &r)) break;
        key_of(r.key.p, r.key.n, &k);
        if (md_keycmp(k.p, k.n, qk.p, qk.n) < 0) lo = mid + 1;
        else hi = mid;
    }
    /* Coincidencias por clave laxa (como Aard 2: sin mayúsculas ni acentos);
     * primero las que coinciden exactamente, luego el resto. */
    uint32_t from = lo > 8 ? lo - 8 : 0;
    uint32_t cand[256];
    int nc = 0, misses = 0;
    for (uint32_t i = from; i < s->n_refs && nc < 256; i++) {
        if (read_ref(s, i, &r)) break;
        key_of(r.key.p, r.key.n, &k);
        int m = fuzzy ? md_has_prefix(k.p, k.n, qk.p, qk.n) : md_keycmp(k.p, k.n, qk.p, qk.n) == 0;
        if (m) { cand[nc++] = i; misses = 0; }
        else if (i >= lo && ++misses > 8) break;
    }
    for (int pass = 0; pass < 2 && res->n < max; pass++) {
        for (int c = 0; c < nc && res->n < max; c++) {
            if (read_ref(s, cand[c], &r)) continue;
            kp.n = 0;
            md_fold(r.key.p, r.key.n, FOLD_PLAIN, 0, &kp);
            int exact = md_keycmp(kp.p, kp.n, qp.p, qp.n) == 0;
            if (exact == (pass == 0)) emit(s, &r, res);
        }
    }
    mb_free(&qk); mb_free(&k); mb_free(&qp); mb_free(&kp);
    mb_free(&r.key); mb_free(&r.frag);
    return res->n;
}

const md_format_t md_fmt_slob = {"slob", slob_probe, slob_needs_index, slob_open, slob_close, slob_lookup};
