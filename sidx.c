/* Índice auxiliar ordenado (.mdidx) para formatos que no traen índice propio.
 *
 * Se construye una sola vez y luego se consulta con búsqueda binaria directa
 * sobre el archivo (unas 20 lecturas pequeñas por consulta), sin cargarlo en
 * memoria, igual que sdcv hace con el .idx de StarDict.
 *
 * Formato:
 *   "MDIDX\2\0\0"                 8 bytes
 *   u64 tamaño del origen, u64 mtime del origen, u32 nº de entradas
 *   3 x (u16 longitud + bytes)    nombre, idioma origen, idioma destino
 *   u64 offset de la tabla, u64 offset de los registros
 *   tabla: n x u32 (offset relativo de cada registro, en orden)
 *   registro: u16 lon + clave normalizada, u16 lon + palabra, u64 off, u32 len
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "md_internal.h"

static const char MAGIC[8] = {'M', 'D', 'I', 'D', 'X', 2, 0, 0};

typedef struct {
    uint32_t fold_off, word_off;
    uint16_t fold_len, word_len;
    uint64_t off;
    uint32_t len;
} sentry;

struct sidx_builder {
    mbuf arena;
    sentry *e;
    size_t n, cap;
};

sidx_builder *sidx_new(void) { return calloc(1, sizeof(sidx_builder)); }

void sidx_add(sidx_builder *b, const char *word, size_t wn, uint64_t off, uint32_t len)
{
    while (wn && (word[wn - 1] == ' ' || word[wn - 1] == '\t')) wn--;
    while (wn && (*word == ' ' || *word == '\t')) { word++; wn--; }
    if (!wn || wn > 4000) return;
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        sentry *ne = realloc(b->e, cap * sizeof *ne);
        if (!ne) return;
        b->e = ne;
        b->cap = cap;
    }
    sentry *s = &b->e[b->n];
    s->fold_off = (uint32_t)b->arena.n;
    md_fold(word, wn, FOLD_PLAIN, 0, &b->arena);
    s->fold_len = (uint16_t)(b->arena.n - s->fold_off);
    if (!s->fold_len) { b->arena.n = s->fold_off; return; }
    s->word_off = (uint32_t)b->arena.n;
    mb_put(&b->arena, word, wn);
    s->word_len = (uint16_t)wn;
    s->off = off;
    s->len = len;
    b->n++;
}

static const char *g_arena;
static int cmp_sentry(const void *a, const void *b)
{
    const sentry *x = a, *y = b;
    int r = md_keycmp(g_arena + x->fold_off, x->fold_len, g_arena + y->fold_off, y->fold_len);
    if (r) return r;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return 0;
}

static void w16(FILE *f, uint32_t v) { unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)}; fwrite(b, 1, 2, f); }
static void w32(FILE *f, uint32_t v) { unsigned char b[4]; for (int i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (8 * i)); fwrite(b, 1, 4, f); }
static void w64(FILE *f, uint64_t v) { unsigned char b[8]; for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i)); fwrite(b, 1, 8, f); }
static void wstr(FILE *f, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 60000) n = 60000;
    w16(f, (uint32_t)n);
    if (n) fwrite(s, 1, n, f);
}

int sidx_write(sidx_builder *b, const char *out_path, const char *src_path,
               const char *name, const char *lf, const char *lt, char *err, int errlen)
{
    struct stat st;
    if (stat(src_path, &st)) {
        md_seterr(err, errlen, "no se puede leer %s", src_path);
        return -1;
    }
    g_arena = b->arena.p;
    if (b->n) qsort(b->e, b->n, sizeof(sentry), cmp_sentry);

    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        md_seterr(err, errlen, "no se puede escribir el índice %s", out_path);
        return -1;
    }
    fwrite(MAGIC, 1, 8, f);
    w64(f, (uint64_t)st.st_size);
    w64(f, (uint64_t)st.st_mtime);
    w32(f, (uint32_t)b->n);
    wstr(f, name);
    wstr(f, lf);
    wstr(f, lt);
    uint64_t table_off = (uint64_t)ftello(f) + 16;
    uint64_t rec_off = table_off + 4 * (uint64_t)b->n;
    w64(f, table_off);
    w64(f, rec_off);
    uint32_t rel = 0;
    for (size_t i = 0; i < b->n; i++) {
        w32(f, rel);
        rel += 2u + b->e[i].fold_len + 2u + b->e[i].word_len + 12u;
    }
    for (size_t i = 0; i < b->n; i++) {
        sentry *s = &b->e[i];
        w16(f, s->fold_len);
        fwrite(b->arena.p + s->fold_off, 1, s->fold_len, f);
        w16(f, s->word_len);
        fwrite(b->arena.p + s->word_off, 1, s->word_len, f);
        w64(f, s->off);
        w32(f, s->len);
    }
    int bad = ferror(f);
    if (fclose(f) || bad) {
        unlink(tmp);
        md_seterr(err, errlen, "error escribiendo el índice %s", out_path);
        return -1;
    }
    if (rename(tmp, out_path)) {
        unlink(tmp);
        md_seterr(err, errlen, "no se puede renombrar el índice %s", out_path);
        return -1;
    }
    return 0;
}

void sidx_free(sidx_builder *b)
{
    if (!b) return;
    mb_free(&b->arena);
    free(b->e);
    free(b);
}

/* ---------------------------------------------------------------------- */

static int dir_writable(const char *path)
{
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
    return access(dir, W_OK) == 0;
}

static char *cache_name(const char *src, const char *cache_dir)
{
    mbuf b = {0};
    mb_puts(&b, cache_dir);
    mb_putc(&b, '/');
    for (const char *p = src; *p; p++) mb_putc(&b, (*p == '/' || *p == ' ') ? '_' : *p);
    mb_puts(&b, ".mdidx");
    return mb_steal(&b);
}

char *sidx_path_for(const char *src, const char *cache_dir, int for_write)
{
    mbuf b = {0};
    mb_puts(&b, src);
    mb_puts(&b, ".mdidx");
    char *primary = mb_steal(&b);
    if (for_write) {
        if (dir_writable(src) || !cache_dir || !*cache_dir) return primary;
        free(primary);
        return cache_name(src, cache_dir);
    }
    if (access(primary, R_OK) == 0 || !cache_dir || !*cache_dir) return primary;
    char *alt = cache_name(src, cache_dir);
    if (access(alt, R_OK) == 0) { free(primary); return alt; }
    free(alt);
    return primary;
}

static int r16(FILE *f, uint32_t *v) { unsigned char b[2]; if (fread(b, 1, 2, f) != 2) return -1; *v = b[0] | ((uint32_t)b[1] << 8); return 0; }
static int r32(FILE *f, uint32_t *v) { unsigned char b[4]; if (fread(b, 1, 4, f) != 4) return -1; *v = 0; for (int i = 3; i >= 0; i--) *v = (*v << 8) | b[i]; return 0; }
static int r64(FILE *f, uint64_t *v) { unsigned char b[8]; if (fread(b, 1, 8, f) != 8) return -1; *v = 0; for (int i = 7; i >= 0; i--) *v = (*v << 8) | b[i]; return 0; }
static char *rstr(FILE *f)
{
    uint32_t n;
    if (r16(f, &n)) return NULL;
    char *s = malloc(n + 1);
    if (!s) return NULL;
    if (n && fread(s, 1, n, f) != n) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

int sidx_is_fresh(const char *idx_path, const char *src_path)
{
    struct stat st;
    if (stat(src_path, &st)) return 0;
    FILE *f = fopen(idx_path, "rb");
    if (!f) return 0;
    char m[8];
    uint64_t sz = 0, mt = 0;
    int ok = fread(m, 1, 8, f) == 8 && !memcmp(m, MAGIC, 8) && !r64(f, &sz) && !r64(f, &mt) &&
             sz == (uint64_t)st.st_size && mt == (uint64_t)st.st_mtime;
    fclose(f);
    return ok;
}

struct sidx {
    FILE *fp;
    uint32_t count;
    uint64_t table_off, rec_off;
    mbuf key, word;
};

sidx *sidx_open(const char *idx_path, char **name, char **lf, char **lt, long *count, char *err, int errlen)
{
    FILE *f = fopen(idx_path, "rb");
    if (!f) {
        md_seterr(err, errlen, "no se puede abrir el índice %s", idx_path);
        return NULL;
    }
    char m[8];
    uint64_t sz, mt;
    uint32_t n;
    if (fread(m, 1, 8, f) != 8 || memcmp(m, MAGIC, 8) || r64(f, &sz) || r64(f, &mt) || r32(f, &n)) {
        fclose(f);
        md_seterr(err, errlen, "índice dañado: %s", idx_path);
        return NULL;
    }
    sidx *s = calloc(1, sizeof *s);
    s->fp = f;
    s->count = n;
    char *a = rstr(f), *b = rstr(f), *c = rstr(f);
    if (name) *name = a; else free(a);
    if (lf) *lf = b; else free(b);
    if (lt) *lt = c; else free(c);
    r64(f, &s->table_off);
    r64(f, &s->rec_off);
    if (count) *count = (long)n;
    return s;
}

void sidx_close(sidx *s)
{
    if (!s) return;
    fclose(s->fp);
    mb_free(&s->key);
    mb_free(&s->word);
    free(s);
}

/* Lee el registro i; deja la clave en s->key y, si want_rest, la palabra y el offset. */
static int read_rec(sidx *s, uint32_t i, int want_rest, uint64_t *off, uint32_t *len)
{
    uint32_t rel, kl, wl;
    if (fseeko(s->fp, (off_t)(s->table_off + 4 * (uint64_t)i), SEEK_SET) || r32(s->fp, &rel)) return -1;
    if (fseeko(s->fp, (off_t)(s->rec_off + rel), SEEK_SET) || r16(s->fp, &kl)) return -1;
    s->key.n = 0;
    if (mb_reserve(&s->key, kl) || fread(s->key.p, 1, kl, s->fp) != kl) return -1;
    s->key.n = kl;
    if (!want_rest) return 0;
    if (r16(s->fp, &wl)) return -1;
    s->word.n = 0;
    if (mb_reserve(&s->word, wl) || fread(s->word.p, 1, wl, s->fp) != wl) return -1;
    s->word.n = wl;
    mb_cstr(&s->word);
    if (r64(s->fp, off) || r32(s->fp, len)) return -1;
    return 0;
}

int sidx_find(sidx *s, const char *word, int prefix, sidx_hit *hits, int max)
{
    mbuf q = {0};
    md_fold(word, strlen(word), FOLD_PLAIN, 0, &q);
    if (!q.n) { mb_free(&q); return 0; }
    /* lower bound */
    uint32_t lo = 0, hi = s->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (read_rec(s, mid, 0, NULL, NULL)) break;
        if (md_keycmp(s->key.p, s->key.n, q.p, q.n) < 0) lo = mid + 1;
        else hi = mid;
    }
    int found = 0;
    for (uint32_t i = lo; i < s->count && found < max; i++) {
        uint64_t off;
        uint32_t len;
        if (read_rec(s, i, 1, &off, &len)) break;
        int match = prefix ? md_has_prefix(s->key.p, s->key.n, q.p, q.n)
                           : md_keycmp(s->key.p, s->key.n, q.p, q.n) == 0;
        if (!match) break;
        hits[found].word = md_strndup(s->word.p, s->word.n);
        hits[found].off = off;
        hits[found].len = len;
        found++;
    }
    mb_free(&q);
    return found;
}
