/* API pública de libmultidict. */
#include <stdlib.h>
#include <string.h>

#include "md_internal.h"

static const md_format_t *formats[] = {&md_fmt_mdx, &md_fmt_dictd, &md_fmt_dsl, &md_fmt_xdxf, &md_fmt_tab, &md_fmt_dc, &md_fmt_mdictdb, &md_fmt_kobo, &md_fmt_sdt, &md_fmt_csv, &md_fmt_quickdic, &md_fmt_slob, &md_fmt_zim, &md_fmt_sdic, &md_fmt_apple, &md_fmt_bgl, &md_fmt_epub, &md_fmt_mobi, NULL};

const char *md_version(void) { return "1.5.0"; }

static const md_format_t *find_format(const char *path)
{
    if (!path) return NULL;
    /* dentro de un paquete de Apple (.dictionary) solo vale Body.data: el resto
     * (KeyText.index, EntryID.index...) no son diccionarios aunque lo parezcan */
    if (strstr(path, ".dictionary/")) return md_fmt_apple.probe(path) ? &md_fmt_apple : NULL;
    for (int i = 0; formats[i]; i++)
        if (formats[i]->probe(path)) return formats[i];
    return NULL;
}

int md_probe(const char *path) { return find_format(path) != NULL; }

int md_needs_index(const char *path, const char *cache_dir)
{
    const md_format_t *f = find_format(path);
    return f ? f->needs_index(path, cache_dir) : 0;
}

md_dict *md_open(const char *path, const char *cache_dir, char *err, int errlen)
{
    if (err && errlen > 0) err[0] = 0;
    const md_format_t *f = find_format(path);
    if (!f) {
        md_seterr(err, errlen, "formato no reconocido: %s", path ? path : "(null)");
        return NULL;
    }
    md_dict *d = calloc(1, sizeof *d);
    d->fmt = f;
    d->path = md_strdup(path);
    d->word_count = -1;
    if (f->open(d, cache_dir, err, errlen)) {
        free(d->path);
        free(d->name);
        free(d->lang_from);
        free(d->lang_to);
        free(d);
        return NULL;
    }
    if (!d->name || !*d->name) {
        free(d->name);
        d->name = md_basename_noext(path);
    }
    return d;
}

void md_close(md_dict *d)
{
    if (!d) return;
    d->fmt->close(d);
    free(d->path);
    free(d->name);
    free(d->lang_from);
    free(d->lang_to);
    free(d);
}

const char *md_name(md_dict *d) { return d && d->name ? d->name : ""; }
const char *md_format(md_dict *d) { return d ? d->fmt->id : ""; }
int md_is_html(md_dict *d) { return d ? d->is_html : 0; }
const char *md_lang_from(md_dict *d) { return d && d->lang_from ? d->lang_from : ""; }
const char *md_lang_to(md_dict *d) { return d && d->lang_to ? d->lang_to : ""; }
long md_word_count(md_dict *d) { return d ? d->word_count : -1; }

/* Orden estable: primero la palabra tal cual, luego la misma sin distinguir
 * mayúsculas, y después el resto (variantes sin tilde, flexiones, prefijos). */
static void rank_results(md_results *r, const char *word)
{
    if (!r || r->n < 2) return;
    int *rank = malloc(sizeof(int) * (size_t)r->n);
    if (!rank) return;
    mbuf q = {0}, k = {0};
    md_fold(word, strlen(word), FOLD_PLAIN, 0, &q);
    for (int i = 0; i < r->n; i++) {
        if (!strcmp(r->words[i], word)) { rank[i] = 0; continue; }
        k.n = 0;
        md_fold(r->words[i], strlen(r->words[i]), FOLD_PLAIN, 0, &k);
        rank[i] = (k.n == q.n && !memcmp(k.p, q.p, q.n)) ? 1 : 2;
    }
    for (int i = 1; i < r->n; i++) {       /* inserción: estable */
        int rk = rank[i];
        char *w = r->words[i], *df = r->defs[i];
        int j = i - 1;
        while (j >= 0 && rank[j] > rk) { rank[j + 1] = rank[j]; r->words[j + 1] = r->words[j]; r->defs[j + 1] = r->defs[j]; j--; }
        rank[j + 1] = rk; r->words[j + 1] = w; r->defs[j + 1] = df;
    }
    mb_free(&q); mb_free(&k);
    free(rank);
}

md_results *md_lookup(md_dict *d, const char *word, int fuzzy, int max_results)
{
    md_results *r = calloc(1, sizeof *r);
    if (!d || !word || !*word) return r;
    if (max_results <= 0) max_results = 32;
    d->fmt->lookup(d, word, fuzzy ? 1 : 0, max_results, r);
    rank_results(r, word);
    return r;
}

int md_results_count(md_results *r) { return r ? r->n : 0; }
const char *md_results_word(md_results *r, int i) { return r && i >= 0 && i < r->n ? r->words[i] : ""; }
const char *md_results_definition(md_results *r, int i) { return r && i >= 0 && i < r->n ? r->defs[i] : ""; }

void md_results_free(md_results *r)
{
    if (!r) return;
    for (int i = 0; i < r->n; i++) {
        free(r->words[i]);
        free(r->defs[i]);
    }
    free(r->words);
    free(r->defs);
    free(r);
}
