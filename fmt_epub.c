/* Diccionarios en EPUB.
 *
 * Se aceptan tres formas de marcar las entradas:
 *   1. EPUB 3 Dictionaries (https://idpf.org/epub/dict/epub-dict.html): un
 *      "search key map" (application/vnd.epub.search-key-map+xml) que asocia
 *      cada palabra (match/value) con un fragmento (archivo#id).
 *   2. Entradas de Kindle/Mobipocket dentro del XHTML: <idx:entry> con
 *      <idx:orth value="..."> e <idx:iform value="...">.
 *   3. EPUB 2 de pyglossary: <div class="groupEntry"> con
 *      <h2 class="groupHeadword">palabra|alternativa</h2>.
 *   Como último recurso, <article> con <dfn> (contenido de EPUB 3 sin mapa).
 *
 * El EPUB es un ZIP sin índice ordenado: la primera vez se recorre y se crea
 * el índice auxiliar (.mdidx) con la posición de cada artículo dentro de su
 * archivo XHTML; después cada artículo se lee directamente del ZIP.
 */
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "md_internal.h"
#include "miniz/miniz.h"
#include "lz4/lz4.h"
#include <sys/stat.h>

typedef struct {
    char *name;
    uint32_t method, csize, usize;
    uint64_t lho;
} zent;

/* Punto de reanudación del descompresor (como zran de zlib): estado de
 * tinfl + ventana de 32 KB, guardados comprimidos con LZ4 en la caché .mdzck */
typedef struct {
    uint64_t out, in;       /* posición en el archivo descomprimido y en el comprimido */
    uint64_t cofs;          /* posición del estado guardado en la caché */
    uint32_t clen;
} ckpt;

typedef struct {
    FILE *fp;
    zent *e;
    size_t n;
    sidx *idx;
    /* puntos de reanudación por entrada del ZIP */
    ckpt **ck;
    uint32_t *nck;
    uint8_t *ck_done;       /* durante el índice: entrada ya recorrida */
    FILE *ckf;              /* caché .mdzck (lectura o escritura) */
} epub;

#define CK_EVERY (2u << 20) /* un punto cada 2 MB descomprimidos */

static uint32_t le16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void epub_free(epub *z)
{
    if (!z) return;
    if (z->fp) fclose(z->fp);
    for (size_t i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    sidx_close(z->idx);
    if (z->ck) for (size_t i = 0; i < z->n; i++) free(z->ck[i]);
    free(z->ck); free(z->nck); free(z->ck_done);
    if (z->ckf) fclose(z->ckf);
    free(z);
}

static int zip_dir(epub *z, char *err, int errlen)
{
    fseeko(z->fp, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftello(z->fp);
    size_t tail = fsize < 70000 ? (size_t)fsize : 70000;
    uint8_t *buf = malloc(tail ? tail : 1);
    fseeko(z->fp, (off_t)(fsize - tail), SEEK_SET);
    if (fread(buf, 1, tail, z->fp) != tail) { free(buf); return -1; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (le32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(buf); md_seterr(err, errlen, "no es un ZIP válido"); return -1; }
    uint32_t count = le16(buf + eocd + 10), cdsize = le32(buf + eocd + 12);
    uint64_t cdoff = le32(buf + eocd + 16);
    free(buf);
    if (cdoff + cdsize > fsize || cdsize > 64u * 1024 * 1024) { md_seterr(err, errlen, "ZIP dañado"); return -1; }
    uint8_t *cd = malloc(cdsize ? cdsize : 1);
    fseeko(z->fp, (off_t)cdoff, SEEK_SET);
    if (fread(cd, 1, cdsize, z->fp) != cdsize) { free(cd); return -1; }
    z->e = calloc(count ? count : 1, sizeof(zent));
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cdsize; i++) {
        if (le32(cd + p) != 0x02014b50) break;
        uint32_t nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (p + 46 + nl > cdsize) break;
        zent *e = &z->e[z->n++];
        e->method = le16(cd + p + 10);
        e->csize = le32(cd + p + 20);
        e->usize = le32(cd + p + 24);
        e->lho = le32(cd + p + 42);
        e->name = md_strndup((const char *)cd + p + 46, nl);
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return 0;
}

static long zip_find(epub *z, const char *name)
{
    for (size_t i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name)) return (long)i;
    return -1;
}

static int zip_get(epub *z, size_t i, mbuf *out)
{
    zent *e = &z->e[i];
    uint8_t lh[30];
    out->n = 0;
    if (e->usize > 256u * 1024 * 1024) return -1;
    if (fseeko(z->fp, (off_t)e->lho, SEEK_SET) || fread(lh, 1, 30, z->fp) != 30 || le32(lh) != 0x04034b50) return -1;
    uint64_t data = e->lho + 30 + le16(lh + 26) + le16(lh + 28);
    uint8_t *c = malloc(e->csize ? e->csize : 1);
    if (!c || fseeko(z->fp, (off_t)data, SEEK_SET) || fread(c, 1, e->csize, z->fp) != e->csize) { free(c); return -1; }
    int rc = 0;
    if (e->method == 0) mb_put(out, c, e->csize);
    else if (e->method == 8 && !mb_reserve(out, e->usize)) {
        int r = md_inflate(c, e->csize, (uint8_t *)out->p, e->usize, 0);
        if (r < 0) rc = -1; else out->n = (size_t)r;
    } else rc = -1;
    free(c);
    mb_cstr(out);
    return rc;
}

/* ---------------------------------------------------------------------- */
/* lectura parcial de entradas deflate con puntos de reanudación           */
/* ---------------------------------------------------------------------- */

static uint64_t data_offset(epub *z, size_t i)
{
    uint8_t lh[30];
    zent *e = &z->e[i];
    if (fseeko(z->fp, (off_t)e->lho, SEEK_SET) || fread(lh, 1, 30, z->fp) != 30 || le32(lh) != 0x04034b50) return UINT64_MAX;
    return e->lho + 30 + le16(lh + 26) + le16(lh + 28);
}

#define CK_SNAP (sizeof(tinfl_decompressor) + TINFL_LZ_DICT_SIZE)

/* Descomprime la entrada i desde el punto `c` (o desde el principio) y llama a
 * fn con cada trozo de salida hasta que devuelva distinto de 0. Si `rec` no es
 * NULL, guarda puntos de reanudación nuevos cada CK_EVERY bytes. */
typedef int (*chunk_fn)(void *ud, uint64_t at, const uint8_t *p, size_t n);

static int inflate_from(epub *z, size_t i, const ckpt *c, chunk_fn fn, void *ud, int rec)
{
    zent *e = &z->e[i];
    uint64_t base = data_offset(z, i);
    if (base == UINT64_MAX) return -1;
    tinfl_decompressor *r = malloc(sizeof *r);
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE), *in = malloc(65536);
    uint8_t *snap = rec ? malloc(CK_SNAP) : NULL, *cs = rec ? malloc((size_t)LZ4_compressBound((int)CK_SNAP)) : NULL;
    int rc = -1;
    if (!r || !dict || !in || (rec && (!snap || !cs))) goto out;
    uint64_t ipos = 0, opos = 0, last_ck = 0;
    if (c) {
        uint8_t *buf = malloc(c->clen), *st = malloc(CK_SNAP);
        int ok = buf && st && !fseeko(z->ckf, (off_t)c->cofs, SEEK_SET) && fread(buf, 1, c->clen, z->ckf) == c->clen &&
                 LZ4_decompress_safe((char *)buf, (char *)st, (int)c->clen, (int)CK_SNAP) == (int)CK_SNAP;
        if (ok) { memcpy(r, st, sizeof *r); memcpy(dict, st + sizeof *r, TINFL_LZ_DICT_SIZE); }
        free(buf); free(st);
        if (!ok) goto out;
        ipos = c->in; opos = last_ck = c->out;
    } else tinfl_init(r);
    size_t dofs = (size_t)(opos & (TINFL_LZ_DICT_SIZE - 1)), avail = 0, ip = 0;
    for (;;) {
        if (ip == avail && ipos < e->csize) {
            size_t want = e->csize - ipos < 65536 ? (size_t)(e->csize - ipos) : 65536;
            if (fseeko(z->fp, (off_t)(base + ipos), SEEK_SET) || fread(in, 1, want, z->fp) != want) goto out;
            avail = want; ip = 0;
        }
        size_t isz = avail - ip, osz = TINFL_LZ_DICT_SIZE - dofs;
        int more = ipos + isz < e->csize;
        tinfl_status st = tinfl_decompress(r, in + ip, &isz, dict, dict + dofs, &osz, more ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        ip += isz; ipos += isz;
        if (osz) {
            if (fn(ud, opos, dict + dofs, osz)) { rc = 0; break; }
            opos += osz;
            dofs = (dofs + osz) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (st == TINFL_STATUS_DONE) { rc = 0; break; }
        if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && ipos >= e->csize)) break;
        /* ventana recién completada: estado limpio para un punto de reanudación */
        if (rec && dofs == 0 && opos >= last_ck + CK_EVERY && z->ckf) {
            memcpy(snap, r, sizeof *r);
            memcpy(snap + sizeof *r, dict, TINFL_LZ_DICT_SIZE);
            int cl = LZ4_compress_default((char *)snap, (char *)cs, (int)CK_SNAP, LZ4_compressBound((int)CK_SNAP));
            off_t at = ftello(z->ckf);
            if (cl > 0 && at >= 0 && fwrite(cs, 1, (size_t)cl, z->ckf) == (size_t)cl) {
                z->ck[i] = realloc(z->ck[i], sizeof(ckpt) * (z->nck[i] + 1));
                z->ck[i][z->nck[i]++] = (ckpt){opos, ipos, (uint64_t)at, (uint32_t)cl};
            }
            last_ck = opos;
        }
    }
out:
    free(r); free(dict); free(in); free(snap); free(cs);
    return rc;
}

static int collect_all(void *ud, uint64_t at, const uint8_t *p, size_t n) { (void)at; mb_put((mbuf *)ud, p, n); return 0; }

/* Entrada completa (al crear el índice); en las grandes, guarda los puntos. */
static int zip_get_rec(epub *z, size_t i, mbuf *out)
{
    zent *e = &z->e[i];
    if (e->method != 8 || e->usize < 2 * CK_EVERY || !z->ckf || !z->ck || z->ck_done[i]) return zip_get(z, i, out);
    out->n = 0;
    if (mb_reserve(out, e->usize)) return -1;
    z->ck_done[i] = 1;
    int rc = inflate_from(z, i, NULL, collect_all, out, 1);
    mb_cstr(out);
    return rc;
}

typedef struct { uint64_t pos; size_t len; mbuf *out; } range_ud;

static int collect_range(void *ud, uint64_t at, const uint8_t *p, size_t n)
{
    range_ud *u = ud;
    uint64_t a = at > u->pos ? at : u->pos, b = at + n < u->pos + u->len ? at + n : u->pos + u->len;
    if (b > a) mb_put(u->out, p + (a - at), (size_t)(b - a));
    return at + n >= u->pos + u->len;
}

/* Lee [pos, pos+len) de la entrada i sin descomprimirla entera. */
static int zip_read_range(epub *z, size_t i, uint64_t pos, size_t len, mbuf *out)
{
    zent *e = &z->e[i];
    out->n = 0;
    if (pos + len > e->usize) return -1;
    if (e->method == 0) {
        uint64_t base = data_offset(z, i);
        if (base == UINT64_MAX || mb_reserve(out, len) || fseeko(z->fp, (off_t)(base + pos), SEEK_SET) ||
            fread(out->p, 1, len, z->fp) != len) return -1;
        out->n = len;
        mb_cstr(out);
        return 0;
    }
    if (e->method != 8) return -1;
    const ckpt *c = NULL;
    if (z->ck && z->ckf)
        for (uint32_t k = 0; k < z->nck[i]; k++) if (z->ck[i][k].out <= pos) c = &z->ck[i][k];
    range_ud u = {pos, len, out};
    int rc = inflate_from(z, i, c, collect_range, &u, 0);
    mb_cstr(out);
    return rc || out->n != len ? -1 : 0;
}

/* Caché .mdzck: "MDZCK1\\0\\0" sizeof(tinfl) tamaño mtime nentradas tabla_ofs,
 * estados comprimidos y, al final, la tabla (por entrada: n, n×{out,in,cofs,clen}). */
static char *ck_path(const char *ip)
{
    mbuf b = {0};
    size_t n = strlen(ip);
    if (n > 6 && !strcmp(ip + n - 6, ".mdidx")) n -= 6;
    mb_put(&b, ip, n);
    mb_puts(&b, ".mdzck");
    return mb_steal(&b);
}

static void p64(FILE *f, uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); fwrite(b, 1, 8, f); }
static uint64_t g64(const uint8_t *b) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | b[i]; return v; }

static int ck_begin(epub *z, const char *cp)
{
    z->ckf = fopen(cp, "w+b");
    if (!z->ckf) return -1;
    z->ck = calloc(z->n, sizeof *z->ck);
    z->nck = calloc(z->n, sizeof *z->nck);
    z->ck_done = calloc(z->n, 1);
    uint8_t h[48] = {0};
    fwrite(h, 1, sizeof h, z->ckf);   /* cabecera provisional */
    return 0;
}

static int ck_finish(epub *z, const char *src)
{
    struct stat st;
    if (!z->ckf || stat(src, &st)) return -1;
    fseeko(z->ckf, 0, SEEK_END);
    uint64_t tab = (uint64_t)ftello(z->ckf);
    for (size_t i = 0; i < z->n; i++) {
        p64(z->ckf, z->nck[i]);
        for (uint32_t k = 0; k < z->nck[i]; k++) {
            p64(z->ckf, z->ck[i][k].out); p64(z->ckf, z->ck[i][k].in);
            p64(z->ckf, z->ck[i][k].cofs); p64(z->ckf, z->ck[i][k].clen);
        }
    }
    fseeko(z->ckf, 0, SEEK_SET);
    fwrite("MDZCK1\0\0", 1, 8, z->ckf);
    p64(z->ckf, sizeof(tinfl_decompressor) | ((uint64_t)sizeof(size_t) << 32));
    p64(z->ckf, (uint64_t)st.st_size); p64(z->ckf, (uint64_t)st.st_mtime);
    p64(z->ckf, z->n); p64(z->ckf, tab);
    free(z->ck_done); z->ck_done = NULL;
    return fflush(z->ckf) ? -1 : 0;
}

static int ck_load(epub *z, const char *cp, const char *src)
{
    struct stat st;
    if (stat(src, &st)) return -1;
    FILE *f = fopen(cp, "rb");
    if (!f) return -1;
    uint8_t h[48];
    if (fread(h, 1, 48, f) != 48 || memcmp(h, "MDZCK1\0\0", 8) ||
        g64(h + 8) != (sizeof(tinfl_decompressor) | ((uint64_t)sizeof(size_t) << 32)) ||
        g64(h + 16) != (uint64_t)st.st_size || g64(h + 24) != (uint64_t)st.st_mtime || g64(h + 32) != z->n ||
        fseeko(f, (off_t)g64(h + 40), SEEK_SET)) { fclose(f); return -1; }
    z->ck = calloc(z->n, sizeof *z->ck);
    z->nck = calloc(z->n, sizeof *z->nck);
    for (size_t i = 0; i < z->n; i++) {
        uint8_t b[32];
        if (fread(b, 1, 8, f) != 8) break;
        uint64_t n = g64(b);
        if (n > 1u << 20) break;
        z->ck[i] = n ? malloc(sizeof(ckpt) * n) : NULL;
        for (uint64_t k = 0; k < n; k++) {
            if (fread(b, 1, 32, f) != 32) { n = k; break; }
            z->ck[i][k] = (ckpt){g64(b), g64(b + 8), g64(b + 16), (uint32_t)g64(b + 24)};
        }
        z->nck[i] = (uint32_t)n;
    }
    z->ckf = f;
    return 0;
}

/* strstr limitado a [p, end): nunca recorre más allá del elemento actual */
static const char *bfind(const char *p, const char *end, const char *needle)
{
    size_t nl = strlen(needle);
    if (!p || p >= end) return NULL;
    for (const char *q = p; (size_t)(end - q) >= nl && (q = memchr(q, needle[0], (size_t)(end - q - (ptrdiff_t)nl + 1))); q++)
        if (!memcmp(q, needle, nl)) return q;
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* XML mínimo                                                              */
/* ---------------------------------------------------------------------- */

/* texto con entidades XML/HTML básicas y numéricas resueltas */
static void xml_text(const char *s, size_t n, int strip_tags, mbuf *out)
{
    static const struct { const char *e; const char *r; } E[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", "\xC2\xA0"},
    };
    for (size_t i = 0; i < n;) {
        if (strip_tags && s[i] == '<') {
            const char *g = memchr(s + i, '>', n - i);
            if (!g) break;
            i = (size_t)(g - s) + 1;
            continue;
        }
        if (s[i] == '&') {
            if (i + 2 < n && s[i + 1] == '#') {
                char *ep;
                int hex = s[i + 2] == 'x' || s[i + 2] == 'X';
                unsigned long v = strtoul(s + i + 2 + hex, &ep, hex ? 16 : 10);
                if (ep < s + n && *ep == ';' && v && v <= 0x10FFFF) { utf8_put(out, (uint32_t)v); i = (size_t)(ep - s) + 1; continue; }
            }
            int hit = 0;
            for (size_t k = 0; k < sizeof E / sizeof *E; k++) {
                size_t l = strlen(E[k].e);
                if (i + l <= n && !memcmp(s + i, E[k].e, l)) { mb_puts(out, E[k].r); i += l; hit = 1; break; }
            }
            if (hit) continue;
        }
        mb_putc(out, s[i++]);
    }
    mb_cstr(out);
}

/* valor del atributo `name` en la etiqueta que empieza en `tag` (hasta '>') */
static int attr(const char *tag, const char *end, const char *name, const char **v, size_t *vn)
{
    const char *gt = memchr(tag, '>', (size_t)(end - tag));
    if (!gt) gt = end;
    size_t nl = strlen(name);
    for (const char *p = tag + 1; p + nl + 2 < gt; p++) {
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') && !memcmp(p, name, nl)) {
            const char *q = p + nl;
            while (q < gt && (*q == ' ' || *q == '\t')) q++;
            if (q >= gt || *q != '=') continue;
            q++;
            while (q < gt && (*q == ' ' || *q == '\t')) q++;
            if (q >= gt || (*q != '"' && *q != '\'')) continue;
            char qc = *q++;
            const char *e = memchr(q, qc, (size_t)(gt - q));
            if (!e) return 0;
            *v = q;
            *vn = (size_t)(e - q);
            return 1;
        }
    }
    return 0;
}

static int attr_text(const char *tag, const char *end, const char *name, mbuf *out)
{
    const char *v;
    size_t vn;
    out->n = 0;
    if (!attr(tag, end, name, &v, &vn)) return 0;
    xml_text(v, vn, 0, out);
    return 1;
}

/* nombre de la etiqueta en `p` ('<' incluido) */
static size_t tag_name(const char *p, const char *end)
{
    size_t n = 0;
    p++;
    while (p + n < end && p[n] != ' ' && p[n] != '>' && p[n] != '/' && p[n] != '\t' && p[n] != '\n' && p[n] != '\r') n++;
    return n;
}

/* Fin (tras el cierre) del elemento que empieza en `p`, contando anidamiento. */
static const char *elem_end(const char *p, const char *end)
{
    size_t nl = tag_name(p, end);
    if (!nl) return NULL;
    const char *name = p + 1;
    const char *gt = memchr(p, '>', (size_t)(end - p));
    if (!gt) return NULL;
    if (gt[-1] == '/') return gt + 1;
    int depth = 1;
    const char *q = gt + 1;
    while (q < end) {
        const char *lt = memchr(q, '<', (size_t)(end - q));
        if (!lt) return NULL;
        if (lt + 1 < end && lt[1] == '/' && (size_t)(end - lt) > nl + 2 && !memcmp(lt + 2, name, nl) &&
            (lt[2 + nl] == '>' || lt[2 + nl] == ' ')) {
            const char *g = memchr(lt, '>', (size_t)(end - lt));
            if (!g) return NULL;
            if (--depth == 0) return g + 1;
            q = g + 1;
            continue;
        }
        if ((size_t)(end - lt) > nl + 1 && !memcmp(lt + 1, name, nl) && (lt[1 + nl] == '>' || lt[1 + nl] == ' ' || lt[1 + nl] == '/')) {
            const char *g = memchr(lt, '>', (size_t)(end - lt));
            if (!g) return NULL;
            if (g[-1] != '/') depth++;
            q = g + 1;
            continue;
        }
        q = lt + 1;
    }
    return NULL;
}

/* resuelve `href` relativo al directorio de `base` (ruta dentro del ZIP) */
static char *resolve(const char *base, const char *href, size_t hn)
{
    mbuf b = {0};
    const char *sl = strrchr(base, '/');
    if (sl) mb_put(&b, base, (size_t)(sl - base + 1));
    mbuf h = {0};
    xml_text(href, hn, 0, &h);
    /* %XX */
    for (size_t i = 0; i < h.n; i++) {
        if (h.p[i] == '%' && i + 2 < h.n) {
            char hx[3] = {h.p[i + 1], h.p[i + 2], 0};
            char *ep;
            long v = strtol(hx, &ep, 16);
            if (!*ep) { mb_putc(&b, (char)v); i += 2; continue; }
        }
        mb_putc(&b, h.p[i]);
    }
    mb_free(&h);
    mb_cstr(&b);
    /* normaliza "dir/../" y "./" */
    char *s = b.p;
    for (;;) {
        char *d = strstr(s, "/../");
        if (!d) break;
        char *prev = d;
        while (prev > s && prev[-1] != '/') prev--;
        memmove(prev, d + 4, strlen(d + 4) + 1);
    }
    while (!strncmp(s, "./", 2)) memmove(s, s + 2, strlen(s + 2) + 1);
    for (char *d; (d = strstr(s, "/./"));) memmove(d, d + 2, strlen(d + 2) + 1);
    return s;
}

/* ---------------------------------------------------------------------- */
/* índice                                                                  */
/* ---------------------------------------------------------------------- */

#define OFF(file, pos) (((uint64_t)(file) << 40) | (uint64_t)(pos))

typedef struct { const char *id; size_t idn; size_t start; } idpos;

static int idcmp(const void *a, const void *b)
{
    const idpos *x = a, *y = b;
    size_t n = x->idn < y->idn ? x->idn : y->idn;
    int c = memcmp(x->id, y->id, n);
    return c ? c : (x->idn > y->idn) - (x->idn < y->idn);
}

/* Archivo XHTML cargado con sus id="..." ordenados para buscar fragmentos. */
typedef struct {
    long file;
    mbuf data;
    idpos *ids;
    size_t nids;
} xfile;

static int xfile_load(epub *z, xfile *x, long file)
{
    if (x->file == file) return 0;
    free(x->ids);
    x->ids = NULL;
    x->nids = 0;
    x->file = -1;
    if (zip_get_rec(z, (size_t)file, &x->data)) return -1;
    x->file = file;
    size_t cap = 0;
    const char *s = x->data.p, *end = s + x->data.n;
    for (const char *p = s; (p = memchr(p, '<', (size_t)(end - p))); p++) {
        if (p[1] == '/' || p[1] == '!' || p[1] == '?') continue;
        const char *v;
        size_t vn;
        const char *gt = memchr(p, '>', (size_t)(end - p));
        if (!gt) break;
        if (attr(p, end, "id", &v, &vn)) {
            if (x->nids == cap) { cap = cap ? cap * 2 : 256; x->ids = realloc(x->ids, cap * sizeof *x->ids); }
            x->ids[x->nids++] = (idpos){v, vn, (size_t)(p - s)};
        }
        p = gt;
    }
    qsort(x->ids, x->nids, sizeof *x->ids, idcmp);
    return 0;
}

static long xfile_find(xfile *x, const char *id, size_t idn)
{
    idpos key = {id, idn, 0};
    idpos *r = bsearch(&key, x->ids, x->nids, sizeof *x->ids, idcmp);
    return r ? (long)r->start : -1;
}

/* Añade al índice el artículo `start..end` de `file`. */
static void add_key(sidx_builder *b, const char *w, size_t wn, long file, size_t start, size_t end)
{
    mbuf t = {0};
    mb_put(&t, w, wn);
    mb_cstr(&t);
    /* recorta espacios */
    size_t i = 0, n = t.n;
    while (i < n && (t.p[i] == ' ' || t.p[i] == '\n' || t.p[i] == '\t' || t.p[i] == '\r')) i++;
    while (n > i && (t.p[n - 1] == ' ' || t.p[n - 1] == '\n' || t.p[n - 1] == '\t' || t.p[n - 1] == '\r')) n--;
    if (n > i && end > start && start < (1ull << 40)) sidx_add(b, t.p + i, n - i, OFF(file, start), (uint32_t)(end - start));
    mb_free(&t);
}

/* Mapa de claves de búsqueda (EPUB 3 Dictionaries). */
static long index_skm(epub *z, const char *skm_path, sidx_builder *b)
{
    long fi = zip_find(z, skm_path);
    mbuf m = {0}, k = {0}, href = {0};
    xfile x = {.file = -1};
    long count = 0;
    if (fi < 0 || zip_get(z, (size_t)fi, &m)) { mb_free(&m); return 0; }
    const char *s = m.p, *end = s + m.n;
    for (const char *p = s; (p = bfind(p, end, "<search-key-group")); ) {
        const char *ge = bfind(p, end, "</search-key-group>");
        const char *gend = ge ? ge : end;
        const char *hv;
        size_t hn;
        if (!attr(p, end, "href", &hv, &hn)) { p = gend; continue; }
        const char *hash = memchr(hv, '#', hn);
        char *target = resolve(skm_path, hv, hash ? (size_t)(hash - hv) : hn);
        long tf = zip_find(z, target);
        free(target);
        size_t start = 0, stop = 0;
        if (tf >= 0 && !xfile_load(z, &x, tf)) {
            const char *xs = x.data.p, *xe = xs + x.data.n;
            if (hash) {
                href.n = 0;
                xml_text(hash + 1, hn - (size_t)(hash + 1 - hv), 0, &href);
                long st = xfile_find(&x, href.p, href.n);
                const char *ee = st >= 0 ? elem_end(xs + st, xe) : NULL;
                if (ee) { start = (size_t)st; stop = (size_t)(ee - xs); }
                /* si apunta dentro de una entrada, se muestra la entrada (<article>) entera */
                if (ee && !(xe - (xs + st) > 8 && !memcmp(xs + st, "<article", 8))) {
                    for (const char *a = xs + st; a > xs; a--) {
                        if (*a != '<' || (size_t)(xe - a) < 9 || memcmp(a, "<article", 8) || (a[8] != ' ' && a[8] != '>')) continue;
                        const char *ae = elem_end(a, xe);
                        if (ae && ae >= ee) { start = (size_t)(a - xs); stop = (size_t)(ae - xs); }
                        break;
                    }
                }
            } else {
                const char *bd = bfind(xs, xe, "<body");
                const char *ee = bd ? elem_end(bd, xe) : NULL;
                if (ee) { start = (size_t)(bd - xs); stop = (size_t)(ee - xs); }
            }
        }
        if (stop > start) {
            /* <match value="..."> y sus <value value="..."/> */
            for (const char *q = p; (q = bfind(q, gend, "<match")); q++) {
                if (attr_text(q, gend, "value", &k)) { add_key(b, k.p, k.n, tf, start, stop); count++; }
                /* forma canónica y lectura (pinyin, yomi...) también sirven para buscar */
                if (attr_text(q, gend, "title", &k)) add_key(b, k.p, k.n, tf, start, stop);
                if (attr_text(q, gend, "phonogram", &k)) add_key(b, k.p, k.n, tf, start, stop);
                const char *mgt = memchr(q, '>', (size_t)(gend - q));
                if (!mgt || mgt[-1] == '/') continue;     /* <match .../> sin <value> */
                const char *me = bfind(mgt, gend, "</match>");
                if (!me) continue;
                for (const char *v = mgt; (v = bfind(v, me, "<value")); v++)
                    if (attr_text(v, me, "value", &k)) add_key(b, k.p, k.n, tf, start, stop);
            }
        }
        p = gend;
    }
    free(x.ids);
    mb_free(&x.data); mb_free(&m); mb_free(&k); mb_free(&href);
    return count;
}

/* Entradas marcadas dentro del XHTML. */
static long index_xhtml(epub *z, long file, int is_dict, sidx_builder *b)
{
    mbuf d = {0}, k = {0};
    long count = 0;
    if (zip_get_rec(z, (size_t)file, &d)) { mb_free(&d); return 0; }
    const char *s = d.p, *end = s + d.n;
    /* <idx:entry> (Kindle) */
    for (const char *p = s; (p = bfind(p, end, "<idx:entry")); ) {
        const char *ee = elem_end(p, end);
        if (!ee) break;
        int any = 0;
        for (const char *q = p; (q = bfind(q, ee, "<idx:orth")); q++) {
            if (attr_text(q, ee, "value", &k) && k.n) any = 1;
            else {
                const char *oe = elem_end(q, ee), *gt = memchr(q, '>', (size_t)(ee - q));
                k.n = 0;
                if (oe && gt && gt[-1] != '/') {
                    /* el texto de <idx:orth> sin las <idx:infl> que pueda contener */
                    const char *ie = bfind(gt, oe, "<idx:infl");
                    const char *te = ie && ie < oe ? ie : oe;
                    xml_text(gt + 1, (size_t)(te - gt - 1), 1, &k);
                    any = k.n > 0;
                }
            }
            if (k.n) { add_key(b, k.p, k.n, file, (size_t)(p - s), (size_t)(ee - s)); count++; }
        }
        for (const char *q = p; (q = bfind(q, ee, "<idx:iform")); q++)
            if (attr_text(q, ee, "value", &k) && k.n) add_key(b, k.p, k.n, file, (size_t)(p - s), (size_t)(ee - s));
        (void)any;
        p = ee;
    }
    if (count) goto out;
    /* pyglossary: <div class="groupEntry"><h2 class="groupHeadword">a|b</h2> */
    for (const char *p = s; (p = bfind(p, end, "class=\"groupEntry\"")); ) {
        const char *lt = p;
        while (lt > s && *lt != '<') lt--;
        const char *ee = elem_end(lt, end);
        if (!ee) break;
        const char *h = bfind(lt, ee, "class=\"groupHeadword\"");
        const char *hgt = h && h < ee ? memchr(h, '>', (size_t)(ee - h)) : NULL;
        const char *he = hgt ? bfind(hgt, ee, "</h2>") : NULL;
        if (he && he < ee) {
            k.n = 0;
            xml_text(hgt + 1, (size_t)(he - hgt - 1), 1, &k);
            /* las alternativas van unidas con '|' */
            const char *w = k.p, *we = k.p + k.n;
            while (w < we) {
                const char *bar = memchr(w, '|', (size_t)(we - w));
                const char *e2 = bar ? bar : we;
                add_key(b, w, (size_t)(e2 - w), file, (size_t)(lt - s), (size_t)(ee - s));
                w = e2 + 1;
            }
            count++;
        }
        p = ee;
    }
    if (count) goto out;
    /* EPUB 3 Dictionaries (2.2.2.1): cada entrada es un <article> dentro de un
     * body/section epub:type="dictionary"; sus titulares van en <dfn> (con
     * title= como forma canónica). Solo en diccionarios declarados, para no
     * tomar por diccionario un libro que use <article> y <dfn>. */
    if (!is_dict && !bfind(s, end, "dictionary\"") && !bfind(s, end, "dictionary ")) goto out;
    if (!is_dict) {
        int typed = 0;
        for (const char *t = s; !typed && (t = bfind(t, end, "epub:type=")); t += 10) {
            const char *q = t + 10;
            char qc = *q++;
            const char *e2 = (qc == '"' || qc == '\'') ? memchr(q, qc, (size_t)(end - q)) : NULL;
            if (!e2) continue;
            for (const char *w = q; w + 10 <= e2; w++)
                if (!memcmp(w, "dictionary", 10) && (w == q || w[-1] == ' ') && (w + 10 == e2 || w[10] == ' ')) { typed = 1; break; }
        }
        if (!typed) goto out;
    }
    for (const char *p = s; (p = bfind(p, end, "<article")); ) {
        if (p[8] != ' ' && p[8] != '>') { p += 8; continue; }
        const char *ee = elem_end(p, end);
        if (!ee) break;
        for (const char *q = p; (q = bfind(q, ee, "<dfn")); q++) {
            if (q[4] != ' ' && q[4] != '>') continue;
            const char *gt = memchr(q, '>', (size_t)(ee - q)), *de = gt ? bfind(gt, ee, "</dfn>") : NULL;
            if (!de || de > ee) break;
            if (!attr_text(q, gt + 1, "title", &k) || !k.n) {
                k.n = 0;
                xml_text(gt + 1, (size_t)(de - gt - 1), 1, &k);
            }
            if (k.n) { add_key(b, k.p, k.n, file, (size_t)(p - s), (size_t)(ee - s)); count++; }
        }
        p = ee;
    }
out:
    mb_free(&d); mb_free(&k);
    return count;
}

static int epub_probe(const char *p) { return ends_with_ci(p, ".epub"); }

static int epub_needs_index(const char *path, const char *cache_dir)
{
    for (int k = 0; k < 2; k++) {
        char *ip = sidx_path_for(path, cache_dir, k);
        int fresh = sidx_is_fresh(ip, path);
        free(ip);
        if (fresh) return 0;
    }
    return 1;
}

static int build(epub *z, md_dict *d, const char *ip, char *err, int errlen)
{
    mbuf c = {0}, opf = {0}, t = {0}, title = {0}, lang = {0};
    int rc = -1;
    long ci = zip_find(z, "META-INF/container.xml");
    const char *v;
    size_t vn;
    char *opf_path = NULL;
    if (ci >= 0 && !zip_get(z, (size_t)ci, &c)) {
        for (const char *rf = c.p; !opf_path && (rf = strstr(rf, "<rootfile")); rf++)
            if (attr(rf, c.p + c.n, "full-path", &v, &vn)) opf_path = resolve("", v, vn);
    }
    long oi = opf_path ? zip_find(z, opf_path) : -1;
    if (oi < 0 || zip_get(z, (size_t)oi, &opf)) { md_seterr(err, errlen, "EPUB sin content.opf: %s", d->path); goto out; }
    const char *s = opf.p, *end = s + opf.n;
    const char *ti = strstr(s, "<dc:title");
    if (ti) {
        const char *gt = strchr(ti, '>'), *te = gt ? strstr(gt, "</dc:title>") : NULL;
        if (te) xml_text(gt + 1, (size_t)(te - gt - 1), 1, &title);
    }
    const char *la = strstr(s, "<dc:language");
    if (la) {
        const char *gt = strchr(la, '>'), *le = gt ? strstr(gt, "</dc:language>") : NULL;
        if (le) xml_text(gt + 1, (size_t)(le - gt - 1), 1, &lang);
    }
    /* código ISO: "en", "es-ES" -> "en", "es"; nombres como "English" no valen */
    {
        size_t n = 0;
        while (n < lang.n && ((lang.p[n] | 32) >= 'a' && (lang.p[n] | 32) <= 'z')) n++;
        if (n < 2 || n > 3 || (n < lang.n && lang.p[n] != '-' && lang.p[n] != '_')) lang.n = 0;
        else { lang.n = n; for (size_t i = 0; i < n; i++) lang.p[i] |= 32; }
        mb_cstr(&lang);
    }

    /* <dc:type>dictionary</dc:type> */
    int is_dict = 0;
    for (const char *ty = s; (ty = strstr(ty, "<dc:type")) && ty < end; ty += 8) {
        const char *gt = strchr(ty, '>'), *te = gt ? strstr(gt, "</dc:type>") : NULL;
        if (!te) break;
        t.n = 0;
        xml_text(gt + 1, (size_t)(te - gt - 1), 1, &t);
        if (t.n == 10 && !memcmp(t.p, "dictionary", 10)) is_dict = 1;
    }
    /* <meta property="source-language">en</meta> y target-language */
    mbuf lto = {0};
    for (const char *mt = s; (mt = strstr(mt, "<meta")) && mt < end; mt += 5) {
        const char *pv;
        size_t pn;
        if (!attr(mt, end, "property", &pv, &pn)) continue;
        int src = pn == 15 && !memcmp(pv, "source-language", 15), dst = pn == 15 && !memcmp(pv, "target-language", 15);
        if (!src && !dst) continue;
        const char *gt = strchr(mt, '>'), *me = gt ? strstr(gt, "</meta>") : NULL;
        if (!me) continue;
        mbuf *dstb = src ? &lang : &lto;
        dstb->n = 0;
        xml_text(gt + 1, (size_t)(me - gt - 1), 1, dstb);
        size_t n = 0;
        while (n < dstb->n && ((dstb->p[n] | 32) >= 'a' && (dstb->p[n] | 32) <= 'z')) n++;
        if (n < 2 || n > 3 || (n < dstb->n && dstb->p[n] != '-' && dstb->p[n] != '_')) dstb->n = 0;
        else { dstb->n = n; for (size_t i = 0; i < n; i++) dstb->p[i] |= 32; }
        mb_cstr(dstb);
    }

    sidx_builder *b = sidx_new();
    long count = 0;
    /* 1. mapas de claves de búsqueda: los del manifiesto (media-type o
     *    properties="search-key-map dictionary") y los enlazados desde
     *    <collection role="dictionary">. Los de glosarios se ignoran. */
    char *done[64];
    int ndone = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *tagname = pass == 0 ? "<item" : "<collection";
        for (const char *p = s; (p = strstr(p, tagname)) && p < end; p++) {
            const char *hrefs[64];
            size_t hlen[64];
            int nh = 0;
            if (pass == 0) {
                if (p[5] != ' ') continue;
                int skm = 0, gloss = 0;
                if (attr(p, end, "media-type", &v, &vn) && vn == strlen("application/vnd.epub.search-key-map+xml") && !memcmp(v, "application/vnd.epub.search-key-map+xml", vn)) skm = 1;
                if (attr(p, end, "properties", &v, &vn)) {
                    t.n = 0; mb_put(&t, v, vn); mb_cstr(&t);
                    if (strstr(t.p, "search-key-map")) skm = 1;
                    if (strstr(t.p, "glossary") && !strstr(t.p, "dictionary")) gloss = 1;
                }
                if (skm && !gloss && attr(p, end, "href", &v, &vn)) { hrefs[0] = v; hlen[0] = vn; nh = 1; }
            } else {
                if (!attr(p, end, "role", &v, &vn) || vn != 10 || memcmp(v, "dictionary", 10)) continue;
                const char *ce = strstr(p, "</collection>");
                if (!ce) ce = end;
                for (const char *l = p; (l = strstr(l, "<link")) && l < ce && nh < 64; l += 5)
                    if (attr(l, ce, "href", &v, &vn)) { hrefs[nh] = v; hlen[nh] = vn; nh++; }
                is_dict = 1;
            }
            for (int h = 0; h < nh; h++) {
                char *hp = resolve(opf_path, hrefs[h], hlen[h]);
                int seen = 0;
                for (int i = 0; i < ndone; i++) if (!strcmp(done[i], hp)) seen = 1;
                long fi = seen ? -1 : zip_find(z, hp);
                /* en una colección, solo los enlaces que son mapas de claves */
                if (fi >= 0 && pass == 1) {
                    mbuf head = {0};
                    size_t hl = z->e[fi].usize < 2048 ? z->e[fi].usize : 2048;
                    if (zip_read_range(z, (size_t)fi, 0, hl, &head) || !head.p || !strstr(head.p, "<search-key-map")) fi = -1;
                    mb_free(&head);
                }
                if (fi >= 0) {
                    count += index_skm(z, hp, b);
                    if (ndone < 64) { done[ndone++] = hp; hp = NULL; }
                }
                free(hp);
            }
        }
    }
    for (int i = 0; i < ndone; i++) free(done[i]);
    /* 2. entradas dentro de los XHTML, en el orden del spine */
    if (!count) {
        for (const char *p = s; (p = strstr(p, "<itemref")) && p < end; p++) {
            if (!attr(p, end, "idref", &v, &vn)) continue;
            /* busca el item con ese id */
            for (const char *q = s; (q = strstr(q, "<item ")) && q < end; q++) {
                const char *iv;
                size_t ivn;
                if (!attr(q, end, "id", &iv, &ivn) || ivn != vn || memcmp(iv, v, vn)) continue;
                if (attr(q, end, "href", &iv, &ivn)) {
                    char *hp = resolve(opf_path, iv, ivn);
                    long fi = zip_find(z, hp);
                    free(hp);
                    if (fi >= 0) count += index_xhtml(z, fi, is_dict, b);
                }
                break;
            }
        }
    }
    if (!count) {
        sidx_free(b);
        mb_free(&lto);
        md_seterr(err, errlen, "el EPUB no parece un diccionario (sin mapa de búsqueda ni entradas marcadas): %s", d->path);
        goto out;
    }
    rc = sidx_write(b, ip, d->path, title.n ? title.p : d->name, lang.n ? lang.p : NULL, lto.n ? lto.p : NULL, err, errlen);
    sidx_free(b);
    mb_free(&lto);
out:
    free(opf_path);
    mb_free(&c); mb_free(&opf); mb_free(&t); mb_free(&title); mb_free(&lang);
    return rc;
}

static int epub_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    epub *z = calloc(1, sizeof *z);
    z->fp = fopen(d->path, "rb");
    if (!z->fp) { md_seterr(err, errlen, "no se puede abrir %s", d->path); goto fail; }
    if (zip_dir(z, err, errlen)) goto fail;
    char *ip = NULL;
    for (int k = 0; k < 2 && !ip; k++) {
        char *i2 = sidx_path_for(d->path, cache_dir, k);
        if (sidx_is_fresh(i2, d->path)) ip = i2; else free(i2);
    }
    if (!ip) {
        ip = sidx_path_for(d->path, cache_dir, 1);
        char *cp = ck_path(ip);
        ck_begin(z, cp);
        int brc = build(z, d, ip, err, errlen);
        if (brc || ck_finish(z, d->path)) {
            if (z->ckf) { fclose(z->ckf); z->ckf = NULL; }
            remove(cp);
            if (z->ck) for (size_t i = 0; i < z->n; i++) { free(z->ck[i]); z->ck[i] = NULL; }
        }
        free(cp);
        if (brc) { free(ip); goto fail; }
    } else {
        char *cp = ck_path(ip);
        if (ck_load(z, cp, d->path)) {
            if (z->ck) for (size_t i = 0; i < z->n; i++) free(z->ck[i]);
            free(z->ck); free(z->nck); z->ck = NULL; z->nck = NULL;
        }
        free(cp);
    }
    char *name = NULL, *lf = NULL, *lt = NULL;
    z->idx = sidx_open(ip, &name, &lf, &lt, &d->word_count, err, errlen);
    free(ip);
    if (!z->idx) goto fail;
    if (name && *name) { free(d->name); d->name = name; } else free(name);
    d->lang_from = lf;
    d->lang_to = lt;
    d->is_html = 1;
    d->priv = z;
    return 0;
fail:
    epub_free(z);
    return -1;
}

static void epub_close(md_dict *d) { epub_free(d->priv); }

/* ¿La etiqueta que empieza en t tiene el atributo booleano hidden? */
static int has_hidden(const char *t, const char *end)
{
    const char *gt = memchr(t, '>', (size_t)(end - t));
    if (!gt) return 0;
    for (const char *p = t + 1; p + 6 <= gt; p++)
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && !memcmp(p, "hidden", 6) &&
            (p[6] == ' ' || p[6] == '=' || p[6] == '>' || p[6] == '/' || p[6] == '\n')) return 1;
    return 0;
}

/* Quita los prefijos idx: (las etiquetas de Kindle no son HTML) y los
 * elementos ocultos (hidden), como las entradas condensadas de EPUB 3
 * (<aside epub:type="condensed-entry" hidden>), que el motor de KOReader
 * mostraría. */
static void clean_html(const char *s, size_t n, mbuf *out)
{
    out->n = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] == '<' && i + 1 < n && s[i + 1] != '/' && s[i + 1] != '!' && has_hidden(s + i, s + n)) {
            const char *ee = elem_end(s + i, s + n);
            if (ee) { i = (size_t)(ee - s); continue; }
        }
        if (s[i] == '<' && i + 5 < n && (!memcmp(s + i + 1, "idx:", 4) || !memcmp(s + i + 1, "/idx:", 5))) {
            const char *g = memchr(s + i, '>', n - i);
            if (!g) break;
            i = (size_t)(g - s) + 1;
            continue;
        }
        mb_putc(out, s[i++]);
    }
    mb_cstr(out);
}

static int epub_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    epub *z = d->priv;
    sidx_hit hits[64];
    int n = sidx_find(z->idx, word, fuzzy, hits, max < 64 ? max : 64);
    mbuf html = {0};
    for (int i = 0; i < n; i++) {
        long file = (long)(hits[i].off >> 40);
        size_t pos = (size_t)(hits[i].off & ((1ull << 40) - 1));
        mbuf art = {0};
        if ((size_t)file < z->n && !zip_read_range(z, (size_t)file, pos, hits[i].len, &art)) {
            clean_html(art.p, art.n, &html);
            if (!md_results_has(r, hits[i].word, html.p)) md_results_add(r, hits[i].word, strlen(hits[i].word), html.p, html.n);
        }
        mb_free(&art);
        free(hits[i].word);
    }
    mb_free(&html);
    return r->n;
}

const md_format_t md_fmt_epub = {"epub", epub_probe, epub_needs_index, epub_open, epub_close, epub_lookup};
