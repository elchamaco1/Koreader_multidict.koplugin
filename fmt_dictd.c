/* dictd / dict.org: archivo .index (texto ordenado) + .dict o .dict.dz.
 *
 * El .index ya viene ordenado, así que se busca con búsqueda binaria directa,
 * sin generar nada. Como cada herramienta (dictfmt, pyglossary…) ordena con
 * reglas ligeramente distintas, si la búsqueda binaria falla se hace un
 * recorrido lineal en memoria (unos milisegundos incluso con índices grandes). */
#include <stdlib.h>
#include <string.h>

#include "md_internal.h"

typedef struct {
    char *idx;      /* .index completo en memoria */
    size_t idx_len;
    rfile *data;    /* .dict / .dict.dz */
} dictd;

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint64_t b64num(const char *s, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d = b64val(s[i]);
        if (d < 0) break;
        v = v * 64 + (uint64_t)d;
    }
    return v;
}

typedef struct {
    const char *word;
    size_t wn;
    uint64_t off, len;
} iline;

static int parse_line(const char *s, const char *end, iline *l)
{
    const char *t1 = memchr(s, '\t', (size_t)(end - s));
    if (!t1) return -1;
    const char *t2 = memchr(t1 + 1, '\t', (size_t)(end - t1 - 1));
    if (!t2) return -1;
    const char *t3 = t2 + 1;
    while (t3 < end && *t3 != '\t' && *t3 != '\n' && *t3 != '\r') t3++;
    l->word = s;
    l->wn = (size_t)(t1 - s);
    l->off = b64num(t1 + 1, (size_t)(t2 - t1 - 1));
    l->len = b64num(t2 + 1, (size_t)(t3 - t2 - 1));
    return 0;
}

static const char *line_start(const char *base, const char *p)
{
    while (p > base && p[-1] != '\n') p--;
    return p;
}

static const char *line_end(const char *p, const char *end)
{
    const char *q = memchr(p, '\n', (size_t)(end - p));
    return q ? q : end;
}

static int dictd_probe(const char *path)
{
    if (!ends_with_ci(path, ".index")) return 0;
    return 1;
}

static int dictd_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

static char *data_path(const char *index_path, int dz)
{
    size_t n = strlen(index_path) - strlen(".index");
    mbuf b = {0};
    mb_put(&b, index_path, n);
    mb_puts(&b, dz ? ".dict.dz" : ".dict");
    return mb_steal(&b);
}

static void add_def(dictd *dd, iline *l, md_results *r)
{
    mbuf def = {0};
    if (l->len > 0 && l->len < 16 * 1024 * 1024 && rf_read(dd->data, l->off, (size_t)l->len, &def) == 0) {
        mb_cstr(&def);
        /* dictd repite el encabezado en la primera línea: la quitamos si coincide */
        const char *p = def.p;
        size_t n = def.n;
        if (n > l->wn && !strncmp(p, l->word, l->wn) && (p[l->wn] == '\n' || p[l->wn] == '\r')) {
            p += l->wn;
            n -= l->wn;
            while (n && (*p == '\n' || *p == '\r')) { p++; n--; }
        }
        char *w = md_strndup(l->word, l->wn);
        char *dtxt = md_strndup(p, n);
        if (!md_results_has(r, w, dtxt)) md_results_add(r, w, l->wn, p, n);
        free(w);
        free(dtxt);
    }
    mb_free(&def);
}

static int dictd_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    (void)cache_dir;
    dictd *dd = calloc(1, sizeof *dd);
    char *dp = data_path(d->path, 1);
    dd->data = rf_open(dp, NULL, 0);
    free(dp);
    if (!dd->data) {
        dp = data_path(d->path, 0);
        dd->data = rf_open(dp, err, errlen);
        free(dp);
    }
    if (!dd->data) { free(dd); return -1; }
    rfile *ix = rf_open(d->path, err, errlen);
    if (!ix) { rf_close(dd->data); free(dd); return -1; }
    mbuf b = {0};
    rf_read(ix, 0, (size_t)rf_size(ix), &b);
    rf_close(ix);
    mb_cstr(&b);
    dd->idx_len = b.n;
    dd->idx = mb_steal(&b);
    d->priv = dd;

    /* cuenta de entradas y nombre desde 00databaseshort / 00-database-short */
    long lines = 0;
    for (size_t i = 0; i < dd->idx_len; i++) if (dd->idx[i] == '\n') lines++;
    d->word_count = lines;
    static const char *keys[] = {"00databaseshort\t", "00-database-short\t", NULL};
    for (int k = 0; keys[k]; k++) {
        const char *p = strstr(dd->idx, keys[k]);
        if (!p || (p != dd->idx && p[-1] != '\n')) continue;
        iline l;
        if (parse_line(p, line_end(p, dd->idx + dd->idx_len), &l)) continue;
        mbuf def = {0};
        if (rf_read(dd->data, l.off, (size_t)l.len, &def) == 0) {
            mb_cstr(&def);
            /* la primera línea suele ser la propia clave; nos quedamos con la siguiente no vacía */
            char *save = NULL;
            for (char *tok = strtok_r(def.p, "\r\n", &save); tok; tok = strtok_r(NULL, "\r\n", &save)) {
                while (*tok == ' ' || *tok == '\t') tok++;
                if (!*tok || !strncmp(tok, "00", 2)) continue;
                free(d->name);
                d->name = md_strdup(tok);
                break;
            }
        }
        mb_free(&def);
        break;
    }
    d->is_html = 0;
    return 0;
}

static void dictd_close(md_dict *d)
{
    dictd *dd = d->priv;
    if (!dd) return;
    free(dd->idx);
    rf_close(dd->data);
    free(dd);
}

static int dictd_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    dictd *dd = d->priv;
    const char *base = dd->idx, *end = dd->idx + dd->idx_len;
    mbuf q = {0}, k = {0};
    md_fold(word, strlen(word), FOLD_DICTD, 0, &q);
    if (!q.n) { mb_free(&q); return 0; }

    /* búsqueda binaria por bytes: encuentra la primera línea con clave >= q */
    size_t lo = 0, hi = dd->idx_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *ls = line_start(base, base + mid);
        const char *le = line_end(ls, end);
        iline l;
        k.n = 0;
        if (parse_line(ls, le, &l) == 0) md_fold(l.word, l.wn, FOLD_DICTD, 0, &k);
        if (md_keycmp(k.p ? k.p : "", k.n, q.p, q.n) < 0) lo = (size_t)(le - base) + 1;
        else hi = (size_t)(ls - base);
    }
    int found = 0;
    const char *p = base + (lo < dd->idx_len ? lo : dd->idx_len);
    p = line_start(base, p);
    while (p < end && found < max) {
        const char *le = line_end(p, end);
        iline l;
        if (parse_line(p, le, &l) == 0) {
            k.n = 0;
            md_fold(l.word, l.wn, FOLD_DICTD, 0, &k);
            int c = md_keycmp(k.p, k.n, q.p, q.n);
            int match = fuzzy ? md_has_prefix(k.p, k.n, q.p, q.n) : c == 0;
            if (!match && c > 0) break;
            if (match) { add_def(dd, &l, r); found++; }
        }
        p = le + 1;
    }

    /* respaldo: recorrido lineal si el orden del .index no coincide con el nuestro */
    if (!found) {
        for (p = base; p < end && found < max; ) {
            const char *le = line_end(p, end);
            iline l;
            if (parse_line(p, le, &l) == 0) {
                k.n = 0;
                md_fold(l.word, l.wn, FOLD_DICTD, 0, &k);
                int match = fuzzy ? md_has_prefix(k.p, k.n, q.p, q.n) : md_keycmp(k.p, k.n, q.p, q.n) == 0;
                if (match) { add_def(dd, &l, r); found++; }
            }
            p = le + 1;
        }
    }
    mb_free(&q);
    mb_free(&k);
    return found;
}

const md_format_t md_fmt_dictd = {"dictd", dictd_probe, dictd_needs_index, dictd_open, dictd_close, dictd_lookup};
