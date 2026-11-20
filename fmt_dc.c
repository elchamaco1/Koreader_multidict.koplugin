/* TangoDict (.dc), el formato propio de DictTango.
 *
 * Un .dc es una base de datos SQLite (https://github.com/Jimex/TangoDict):
 *   DictionaryInfo      nombre, tipo de plantilla, compresión del contenido...
 *   DictionaryProperty  propiedades clave/valor (descripción, diccionario ZSTD
 *                       en base64, cabeceras HTML...)
 *   DictionaryHeadword  entradas (palabra y hasta 5 claves alternativas)
 *   DictionaryEntry     contenido de cada entrada, opcionalmente comprimido con
 *                       Zstandard (ZSTD o ZSTD_DICT)
 *
 * La especificación publicada no detalla todas las columnas, así que se leen
 * del propio esquema (PRAGMA table_info) y se eligen por nombre. Las búsquedas
 * son consultas SQL sobre los índices de la base: no se genera nada aparte.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "md_internal.h"
#include "sqlite/sqlite3.h"
/* las funciones de zstd no se exportan desde libmultidict */
#define ZSTDLIB_VISIBLE
#define ZSTDLIB_HIDDEN
#define ZDICTLIB_VISIBLE
#define ZSTDERRORLIB_VISIBLE
#define ZSTDERRORLIB_HIDDEN
#include "zstd/zstd.h"

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *q_exact, *q_prefix;
    int compressed;
    ZSTD_DCtx *dctx;
    ZSTD_DDict *ddict;
    char *page_head, *page_foot, *entry_head, *entry_foot;
} dcdict;

/* ---------------------------------------------------------------------- */

static int dc_probe(const char *path) { return ends_with_ci(path, ".dc"); }
static int dc_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

typedef struct {
    char name[64][64];
    char type[64][32];
    int n;
} colset;

static void table_cols(sqlite3 *db, const char *table, colset *cs)
{
    char sql[128];
    snprintf(sql, sizeof sql, "PRAGMA table_info(\"%s\")", table);
    sqlite3_stmt *st;
    cs->n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW && cs->n < 64) {
        const char *n = (const char *)sqlite3_column_text(st, 1);
        const char *t = (const char *)sqlite3_column_text(st, 2);
        snprintf(cs->name[cs->n], 64, "%s", n ? n : "");
        snprintf(cs->type[cs->n], 32, "%s", t ? t : "");
        cs->n++;
    }
    sqlite3_finalize(st);
}

/* Primera columna cuyo nombre coincide (sin distinguir mayúsculas) con alguno de la lista. */
static const char *pick(colset *cs, const char *const *names)
{
    for (int k = 0; names[k]; k++)
        for (int i = 0; i < cs->n; i++)
            if (!strcasecmp(cs->name[i], names[k])) return cs->name[i];
    return NULL;
}

/* Primera columna cuyo nombre contiene alguno de los fragmentos. */
static const char *pick_like(colset *cs, const char *const *frags, const char *exclude)
{
    for (int k = 0; frags[k]; k++)
        for (int i = 0; i < cs->n; i++) {
            if (exclude && !strcasecmp(cs->name[i], exclude)) continue;
            char low[64];
            size_t j = 0;
            for (; cs->name[i][j] && j < 63; j++) low[j] = (char)tolower((unsigned char)cs->name[i][j]);
            low[j] = 0;
            if (strstr(low, frags[k])) return cs->name[i];
        }
    return NULL;
}

static int has_table(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name=?1 COLLATE NOCASE",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    return found;
}

static char *query_text(sqlite3 *db, const char *sql, const char *arg)
{
    sqlite3_stmt *st;
    char *r = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    if (arg) sqlite3_bind_text(st, 1, arg, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        const void *b = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        r = md_strndup(b ? (const char *)b : "", (size_t)n);
    }
    sqlite3_finalize(st);
    return r;
}

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static void b64decode(const char *s, mbuf *out)
{
    uint32_t acc = 0;
    int bits = 0;
    for (; *s; s++) {
        int v = b64v((unsigned char)*s);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            mb_putc(out, (char)((acc >> bits) & 0xFF));
        }
    }
}

/* Lee la propiedad `key` de DictionaryProperty (columnas detectadas). */
static char *get_property(sqlite3 *db, const char *kcol, const char *vcol, const char *key)
{
    if (!kcol || !vcol) return NULL;
    char sql[256];
    snprintf(sql, sizeof sql, "SELECT \"%s\" FROM DictionaryProperty WHERE \"%s\"=?1 COLLATE NOCASE LIMIT 1", vcol, kcol);
    return query_text(db, sql, key);
}

static char *uri_for(const char *path)
{
    mbuf b = {0};
    mb_puts(&b, "file:");
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '?' || c == '#' || c == '%' || c < 0x20) mb_printf(&b, "%%%02X", c);
        else mb_putc(&b, (char)c);
    }
    mb_puts(&b, "?mode=ro&immutable=1");
    return mb_steal(&b);
}

static void dc_free(dcdict *t)
{
    if (!t) return;
    if (t->q_exact) sqlite3_finalize(t->q_exact);
    if (t->q_prefix) sqlite3_finalize(t->q_prefix);
    if (t->db) sqlite3_close(t->db);
    if (t->dctx) ZSTD_freeDCtx(t->dctx);
    if (t->ddict) ZSTD_freeDDict(t->ddict);
    free(t->page_head); free(t->page_foot); free(t->entry_head); free(t->entry_foot);
    free(t);
}

static int dc_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    (void)cache_dir;
    dcdict *t = calloc(1, sizeof *t);
    char *uri = uri_for(d->path);
    int rc = sqlite3_open_v2(uri, &t->db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
    free(uri);
    if (rc != SQLITE_OK) {
        md_seterr(err, errlen, "no se puede abrir %s: %s", d->path, t->db ? sqlite3_errmsg(t->db) : "sin memoria");
        dc_free(t);
        return -1;
    }
    if (!has_table(t->db, "DictionaryHeadword") || !has_table(t->db, "DictionaryEntry")) {
        md_seterr(err, errlen, "%s no parece un diccionario TangoDict (faltan las tablas DictionaryHeadword/DictionaryEntry)",
                  d->path);
        dc_free(t);
        return -1;
    }

    /* --- columnas --- */
    colset hc, ec, pc;
    table_cols(t->db, "DictionaryHeadword", &hc);
    table_cols(t->db, "DictionaryEntry", &ec);
    table_cols(t->db, "DictionaryProperty", &pc);

    static const char *const h_id[] = {"Id", "HeadwordId", "RowId", NULL};
    static const char *const h_word[] = {"Headword", "Word", "Keyword", "Key", "Name", "Text", NULL};
    static const char *const e_fk[] = {"HeadwordId", "DictionaryHeadwordId", "HeadwordRowId", "WordId", NULL};
    static const char *const e_fk_like[] = {"headwordid", "headword", "wordid", NULL};
    static const char *const e_content[] = {"Content", "Definition", "Html", "Body", "Data", "EntryContent",
                                            "Text", "Value", NULL};
    static const char *const e_content_like[] = {"content", "definition", "html", "data", "body", NULL};
    static const char *const e_word[] = {"Headword", "Word", "Keyword", "EntryName", "Name", NULL};
    static const char *const p_key[] = {"Name", "Key", "PropertyName", "PropertyKey", "Property", NULL};
    static const char *const p_val[] = {"Value", "PropertyValue", "Data", "Content", NULL};

    const char *hid = pick(&hc, h_id);
    const char *hword = pick(&hc, h_word);
    const char *efk = pick(&ec, e_fk);
    if (!efk) efk = pick_like(&ec, e_fk_like, NULL);
    const char *econt = pick(&ec, e_content);
    if (!econt) econt = pick_like(&ec, e_content_like, efk);
    const char *eword = pick(&ec, e_word);
    if (eword && efk && !strcasecmp(eword, efk)) eword = NULL;
    const char *pkey = pick(&pc, p_key), *pval = pick(&pc, p_val);

    if (!hword || !econt || (!efk && !eword)) {
        mbuf m = {0};
        mb_puts(&m, "estructura TangoDict no reconocida. DictionaryHeadword(");
        for (int i = 0; i < hc.n; i++) mb_printf(&m, "%s%s", i ? "," : "", hc.name[i]);
        mb_puts(&m, ") DictionaryEntry(");
        for (int i = 0; i < ec.n; i++) mb_printf(&m, "%s%s", i ? "," : "", ec.name[i]);
        mb_puts(&m, ")");
        md_seterr(err, errlen, "%s", mb_cstr(&m));
        mb_free(&m);
        dc_free(t);
        return -1;
    }

    /* claves alternativas Keyword1..Keyword5 (solo las que existan) */
    mbuf where = {0}, wherep = {0};
    mb_printf(&where, "h.\"%s\"=?1", hword);
    mb_printf(&wherep, "(h.\"%s\">=?1 AND h.\"%s\"<?2)", hword, hword);
    /* DictionaryInfo.KeywordNInUsed dice qué claves alternativas se usan; las
     * que no, se omiten para que SQLite pueda usar los índices en vez de
     * recorrer la tabla. Si el indicador falta o es NULL, se consulta igual. */
    int has_info = has_table(t->db, "DictionaryInfo");
    for (int k = 1; k <= 5; k++) {
        char kn[16];
        snprintf(kn, sizeof kn, "Keyword%d", k);
        int col = -1;
        for (int i = 0; i < hc.n; i++)
            if (!strcasecmp(hc.name[i], kn) && strcasecmp(kn, hword)) col = i;
        if (col < 0) continue;
        if (has_info) {
            char q[96];
            snprintf(q, sizeof q, "SELECT Keyword%dInUsed FROM DictionaryInfo LIMIT 1", k);
            char *v = query_text(t->db, q, NULL);
            int unused = v && *v && (!strcmp(v, "0") || !strcasecmp(v, "false"));
            free(v);
            if (unused) continue;
        }
        mb_printf(&where, " OR h.\"%s\"=?1", hc.name[col]);
    }

    /* unión entrada-contenido: por id de entrada, o por la propia palabra */
    mbuf join = {0};
    if (efk && hid) mb_printf(&join, "e.\"%s\"=h.\"%s\"", efk, hid);
    else if (efk) mb_printf(&join, "e.\"%s\"=h.rowid", efk);
    else mb_printf(&join, "e.\"%s\"=h.\"%s\"", eword, hword);

    mbuf sql = {0};
    mb_printf(&sql, "SELECT h.\"%s\", e.\"%s\" FROM DictionaryHeadword h JOIN DictionaryEntry e ON %s WHERE (%s) "
                    "ORDER BY h.rowid, e.rowid LIMIT ?3",
              hword, econt, mb_cstr(&join), mb_cstr(&where));
    rc = sqlite3_prepare_v2(t->db, mb_cstr(&sql), -1, &t->q_exact, NULL);
    sql.n = 0;
    mb_printf(&sql, "SELECT h.\"%s\", e.\"%s\" FROM DictionaryHeadword h JOIN DictionaryEntry e ON %s WHERE %s "
                    "ORDER BY h.\"%s\", e.rowid LIMIT ?3",
              hword, econt, mb_cstr(&join), mb_cstr(&wherep), hword);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(t->db, mb_cstr(&sql), -1, &t->q_prefix, NULL);
    if (rc != SQLITE_OK) md_seterr(err, errlen, "consulta TangoDict no válida: %s", sqlite3_errmsg(t->db));
    mb_free(&sql);
    mb_free(&where);
    mb_free(&wherep);
    mb_free(&join);
    if (rc != SQLITE_OK) { dc_free(t); return -1; }

    /* --- DictionaryInfo --- */
    if (has_table(t->db, "DictionaryInfo")) {
        char *v = query_text(t->db, "SELECT DictionaryName FROM DictionaryInfo LIMIT 1", NULL);
        if (v && *v) { free(d->name); d->name = v; } else free(v);
        v = query_text(t->db, "SELECT IsContentCompressed FROM DictionaryInfo LIMIT 1", NULL);
        t->compressed = v && (atoi(v) != 0 || !strcasecmp(v, "true"));
        free(v);
        v = query_text(t->db, "SELECT NoOfHeadwords FROM DictionaryInfo LIMIT 1", NULL);
        if (v) d->word_count = atol(v);
        free(v);
    }
    if (d->word_count <= 0) {
        char *v = query_text(t->db, "SELECT count(*) FROM DictionaryHeadword", NULL);
        if (v) d->word_count = atol(v);
        free(v);
    }

    /* --- propiedades: diccionario ZSTD y fragmentos HTML --- */
    char *zd = get_property(t->db, pkey, pval, "ZstdDictionary");
    if (zd && *zd) {
        mbuf raw = {0};
        b64decode(zd, &raw);
        if (raw.n) t->ddict = ZSTD_createDDict(raw.p, raw.n);
        mb_free(&raw);
    }
    free(zd);
    t->page_head = get_property(t->db, pkey, pval, "PageBodyHeaderHtml");
    t->page_foot = get_property(t->db, pkey, pval, "PageBodyFooterHtml");
    t->entry_head = get_property(t->db, pkey, pval, "EntryHeaderHtml");
    t->entry_foot = get_property(t->db, pkey, pval, "EntryFooterHtml");

    t->dctx = ZSTD_createDCtx();
    d->is_html = 1;
    d->priv = t;
    return 0;
}

static void dc_close(md_dict *d) { dc_free(d->priv); }

/* Contenido de la fila: lo descomprime si es un frame Zstandard. */
static int content_of(dcdict *t, sqlite3_stmt *st, int col, mbuf *out)
{
    const uint8_t *b = sqlite3_column_blob(st, col);
    size_t n = (size_t)sqlite3_column_bytes(st, col);
    out->n = 0;
    if (!b || !n) { mb_cstr(out); return 0; }
    /* número mágico de Zstandard: 28 B5 2F FD */
    int is_zstd = n >= 4 && b[0] == 0x28 && b[1] == 0xB5 && b[2] == 0x2F && b[3] == 0xFD;
    if (!is_zstd) {
        mb_put(out, b, n);
        mb_cstr(out);
        return 0;
    }
    (void)t->compressed;
    unsigned long long sz = ZSTD_getFrameContentSize(b, n);
    if (sz != ZSTD_CONTENTSIZE_UNKNOWN && sz != ZSTD_CONTENTSIZE_ERROR && sz < 256ull * 1024 * 1024) {
        if (mb_reserve(out, (size_t)sz)) return -1;
        size_t r = t->ddict ? ZSTD_decompress_usingDDict(t->dctx, out->p, (size_t)sz, b, n, t->ddict)
                            : ZSTD_decompressDCtx(t->dctx, out->p, (size_t)sz, b, n);
        if (ZSTD_isError(r)) return -1;
        out->n = r;
    } else {
        /* tamaño desconocido: en flujo */
        ZSTD_DCtx_reset(t->dctx, ZSTD_reset_session_only);
        if (t->ddict) ZSTD_DCtx_refDDict(t->dctx, t->ddict);
        ZSTD_inBuffer in = {b, n, 0};
        while (in.pos < in.size) {
            if (mb_reserve(out, 65536)) return -1;
            ZSTD_outBuffer o = {out->p + out->n, out->cap - out->n - 1, 0};
            size_t r = ZSTD_decompressStream(t->dctx, &o, &in);
            if (ZSTD_isError(r)) return -1;
            out->n += o.pos;
            if (r == 0) break;
        }
        if (t->ddict) ZSTD_DCtx_refDDict(t->dctx, NULL);
    }
    mb_cstr(out);
    return 0;
}

static void run_query(md_dict *d, dcdict *t, sqlite3_stmt *st, int max, md_results *r, int depth);

static void emit(md_dict *d, dcdict *t, const char *word, mbuf *c, md_results *r, int depth)
{
    /* entradas enlazadas, como en MDict */
    if (c->n > 8 && !strncmp(c->p, "@@@LINK=", 8)) {
        if (depth >= 5) return;
        char *target = md_strndup(c->p + 8, c->n - 8);
        char *nl = strpbrk(target, "\r\n");
        if (nl) *nl = 0;
        md_results tmp = {0};
        sqlite3_reset(t->q_exact);
        sqlite3_bind_text(t->q_exact, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(t->q_exact, 3, 4);
        run_query(d, t, t->q_exact, 4, &tmp, depth + 1);
        for (int i = 0; i < tmp.n; i++) {
            if (!md_results_has(r, word, tmp.defs[i])) md_results_add(r, word, strlen(word), tmp.defs[i], strlen(tmp.defs[i]));
            free(tmp.words[i]);
            free(tmp.defs[i]);
        }
        free(tmp.words);
        free(tmp.defs);
        free(target);
        return;
    }
    mbuf html = {0};
    if (t->entry_head) mb_puts(&html, t->entry_head);
    /* enlaces internos de MDict -> KOReader */
    const char *p = c->p, *end = c->p + c->n;
    while (p < end) {
        const char *e = strstr(p, "entry://");
        if (!e || e >= end) { mb_put(&html, p, (size_t)(end - p)); break; }
        mb_put(&html, p, (size_t)(e - p));
        mb_puts(&html, "bword://");
        p = e + 8;
    }
    if (t->entry_foot) mb_puts(&html, t->entry_foot);
    mb_cstr(&html);
    if (!md_results_has(r, word, html.p)) md_results_add(r, word, strlen(word), html.p, html.n);
    mb_free(&html);
}

static void run_query(md_dict *d, dcdict *t, sqlite3_stmt *st, int max, md_results *r, int depth)
{
    /* recogemos las filas antes de procesarlas: un enlace reutiliza la consulta */
    md_results rows = {0};
    mbuf c = {0};
    while (sqlite3_step(st) == SQLITE_ROW && rows.n < max) {
        const char *w = (const char *)sqlite3_column_text(st, 0);
        if (content_of(t, st, 1, &c)) continue;
        md_results_add(&rows, w ? w : "", w ? strlen(w) : 0, c.p, c.n);
    }
    sqlite3_reset(st);
    for (int i = 0; i < rows.n; i++) {
        mbuf cb = {0};
        mb_puts(&cb, rows.defs[i]);
        if (r->n < max) emit(d, t, rows.words[i], &cb, r, depth);
        mb_free(&cb);
        free(rows.words[i]);
        free(rows.defs[i]);
    }
    free(rows.words);
    free(rows.defs);
    mb_free(&c);
}

static int dc_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    dcdict *t = d->priv;
    sqlite3_stmt *st = fuzzy ? t->q_prefix : t->q_exact;
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, word, -1, SQLITE_TRANSIENT);
    if (fuzzy) {
        mbuf up = {0};
        mb_puts(&up, word);
        mb_puts(&up, "\xF4\x8F\xBF\xBF"); /* U+10FFFF: cota superior del prefijo */
        sqlite3_bind_text(st, 2, up.p, (int)up.n, SQLITE_TRANSIENT);
        mb_free(&up);
    }
    sqlite3_bind_int(st, 3, max);
    run_query(d, t, st, max, r, 0);
    return r->n;
}

const md_format_t md_fmt_dc = {"tangodict", dc_probe, dc_needs_index, dc_open, dc_close, dc_lookup};

/* ====================================================================== */
/* Base SQLite de mdict-utils (`mdict -x dic.mdx --exdb`, `mdict --txt-db`) */
/*   meta(key TEXT, value TEXT)          cabecera del MDX, "zip" = comprimido */
/*   mdx(entry TEXT, paraphrase TEXT|BLOB)  definición, con zlib si zip      */
/*   índice mdx_entry_index sobre entry                                     */
/* ====================================================================== */

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *q_exact, *q_prefix;
} mdb;

/* Tabla de palabras: "mdx" (mdict-utils), "mdict" u otra con una columna de
 * palabra (entry, word, key...) y otra de definición (paraphrase, definition...). */
static int find_table(sqlite3 *db, char tbl[64], char ecol[64], char pcol[64])
{
    static const char *const en[] = {"entry", "word", "headword", "key", "term", "keyword", NULL};
    static const char *const pn[] = {"paraphrase", "definition", "content", "meaning", "article", "defi", "html", NULL};
    static const char *const prefer[] = {"mdx", "mdict", NULL};
    for (int k = 0; prefer[k]; k++) {
        if (!has_table(db, prefer[k])) continue;
        colset c;
        table_cols(db, prefer[k], &c);
        const char *e = pick(&c, en), *pc = pick(&c, pn);
        if (e && pc) { snprintf(tbl, 64, "%s", prefer[k]); snprintf(ecol, 64, "%s", e); snprintf(pcol, 64, "%s", pc); return 1; }
    }
    sqlite3_stmt *st;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' AND lower(name)<>'meta'",
                           -1, &st, NULL) != SQLITE_OK) return 0;
    while (!ok && sqlite3_step(st) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(st, 0);
        if (!n || strlen(n) >= 64 || strchr(n, '"')) continue;
        colset c;
        table_cols(db, n, &c);
        const char *e = pick(&c, en), *pc = pick(&c, pn);
        if (e && pc) { snprintf(tbl, 64, "%s", n); snprintf(ecol, 64, "%s", e); snprintf(pcol, 64, "%s", pc); ok = 1; }
    }
    sqlite3_finalize(st);
    return ok;
}

static sqlite3 *open_ro(const char *path)
{
    sqlite3 *db = NULL;
    char *uri = uri_for(path);
    int rc = sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
    free(uri);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    return db;
}

/* Solo se aceptan los .db que tengan la tabla mdx(entry, paraphrase). */
static int mdb_probe(const char *path)
{
    if (!ends_with_ci(path, ".db")) return 0;
    sqlite3 *db = open_ro(path);
    if (!db) return 0;
    char t[64], e[64], p[64];
    int ok = find_table(db, t, e, p);
    sqlite3_close(db);
    return ok;
}

static void mdb_free(mdb *t)
{
    if (!t) return;
    if (t->q_exact) sqlite3_finalize(t->q_exact);
    if (t->q_prefix) sqlite3_finalize(t->q_prefix);
    if (t->db) sqlite3_close(t->db);
    free(t);
}

static int mdb_open(md_dict *d, const char *cache_dir, char *err, int errlen)
{
    (void)cache_dir;
    mdb *t = calloc(1, sizeof *t);
    t->db = open_ro(d->path);
    char tbl[64], ec[64], pc[64], sql[512];
    if (!t->db || !find_table(t->db, tbl, ec, pc)) {
        md_seterr(err, errlen, "no es una base de mdict-utils: %s", d->path);
        mdb_free(t);
        return -1;
    }
    snprintf(sql, sizeof sql, "SELECT \"%s\", \"%s\" FROM \"%s\" WHERE \"%s\"=?1 LIMIT ?3", ec, pc, tbl, ec);
    int prc = sqlite3_prepare_v2(t->db, sql, -1, &t->q_exact, NULL);
    snprintf(sql, sizeof sql, "SELECT \"%s\", \"%s\" FROM \"%s\" WHERE \"%s\">=?1 AND \"%s\"<?2 ORDER BY \"%s\" LIMIT ?3",
             ec, pc, tbl, ec, ec, ec);
    if (prc || sqlite3_prepare_v2(t->db, sql, -1, &t->q_prefix, NULL)) {
        md_seterr(err, errlen, "consulta no válida: %s", sqlite3_errmsg(t->db));
        mdb_free(t);
        return -1;
    }
    if (has_table(t->db, "meta")) {
        char *v = query_text(t->db, "SELECT value FROM meta WHERE lower(key)='title' LIMIT 1", NULL);
        /* títulos genéricos ("slob", "mdx"...): mejor la primera línea de la descripción */
        static const char *const generic[] = {"", "slob", "mdx", "mdict", "dict", "dictionary", "untitled", "title", NULL};
        int gen = !v;
        for (int g = 0; v && generic[g]; g++) if (!strcasecmp(v, generic[g])) gen = 1;
        if (gen) {
            char *desc = query_text(t->db, "SELECT value FROM meta WHERE lower(key)='description' LIMIT 1", NULL);
            if (desc && *desc) {
                char *br = strstr(desc, "<br");
                if (!br) br = strchr(desc, '\n');
                if (br) *br = 0;
                free(v);
                v = desc;
            } else free(desc);
        }
        if (v && *v && !strstr(v, "No HTML code allowed")) {
            /* el título de MDict puede llevar etiquetas HTML */
            mbuf o = {0};
            for (const char *p = v; *p; p++) {
                if (*p == '<') { const char *q = strchr(p, '>'); if (!q) break; p = q; continue; }
                mb_putc(&o, *p);
            }
            free(d->name);
            d->name = mb_steal(&o);
        }
        free(v);
    }
    snprintf(sql, sizeof sql, "SELECT count(*) FROM \"%s\"", tbl);
    char *n = query_text(t->db, sql, NULL);
    if (n) d->word_count = atol(n);
    free(n);
    d->is_html = 1;
    d->priv = t;
    return 0;
}

static void mdb_close(md_dict *d) { mdb_free(d->priv); }

/* Contenido: texto, o zlib (cabecera 0x78) si se exportó con --exdb-zip. */
static void mdb_content(sqlite3_stmt *st, mbuf *out)
{
    const uint8_t *b = sqlite3_column_blob(st, 1);
    size_t n = (size_t)sqlite3_column_bytes(st, 1);
    out->n = 0;
    if (b && n > 2 && b[0] == 0x78 && ((b[0] << 8) | b[1]) % 31 == 0) {
        size_t ol = 0;
        uint8_t *dec = md_inflate_heap(b, n, &ol, 1);
        if (dec) {
            mb_put(out, dec, ol);
            free(dec);
            mb_cstr(out);
            return;
        }
    }
    if (b) mb_put(out, b, n);
    mb_cstr(out);
}

static void mdb_query(mdb *t, sqlite3_stmt *st, const char *word, const char *upper, int max, md_results *r, int depth);

static void mdb_emit(mdb *t, const char *word, mbuf *c, md_results *r, int depth)
{
    while (c->n && (c->p[c->n - 1] == '\n' || c->p[c->n - 1] == '\r' || c->p[c->n - 1] == 0)) c->n--;
    mb_cstr(c);
    if (c->n > 8 && !strncmp(c->p, "@@@LINK=", 8)) {
        if (depth >= 5) return;
        char *target = md_strndup(c->p + 8, c->n - 8);
        char *nl = strpbrk(target, "\r\n");
        if (nl) *nl = 0;
        md_results tmp = {0};
        mdb_query(t, t->q_exact, target, NULL, 4, &tmp, depth + 1);
        for (int i = 0; i < tmp.n; i++) {
            if (!md_results_has(r, word, tmp.defs[i])) md_results_add(r, word, strlen(word), tmp.defs[i], strlen(tmp.defs[i]));
            free(tmp.words[i]);
            free(tmp.defs[i]);
        }
        free(tmp.words);
        free(tmp.defs);
        free(target);
        return;
    }
    mbuf html = {0};
    const char *p = c->p, *end = c->p + c->n;
    while (p < end) {
        const char *e = strstr(p, "entry://");
        if (!e || e >= end) { mb_put(&html, p, (size_t)(end - p)); break; }
        mb_put(&html, p, (size_t)(e - p));
        mb_puts(&html, "bword://");
        p = e + 8;
    }
    mb_cstr(&html);
    if (!md_results_has(r, word, html.p)) md_results_add(r, word, strlen(word), html.p, html.n);
    mb_free(&html);
}

static void mdb_query(mdb *t, sqlite3_stmt *st, const char *word, const char *upper, int max, md_results *r, int depth)
{
    md_results rows = {0};
    mbuf c = {0};
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, word, -1, SQLITE_TRANSIENT);
    if (upper) sqlite3_bind_text(st, 2, upper, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, max);
    while (sqlite3_step(st) == SQLITE_ROW && rows.n < max) {
        const char *w = (const char *)sqlite3_column_text(st, 0);
        mdb_content(st, &c);
        md_results_add(&rows, w ? w : "", w ? strlen(w) : 0, c.p, c.n);
    }
    sqlite3_reset(st);
    for (int i = 0; i < rows.n; i++) {
        mbuf cb = {0};
        mb_puts(&cb, rows.defs[i]);
        if (r->n < max) mdb_emit(t, rows.words[i], &cb, r, depth);
        mb_free(&cb);
        free(rows.words[i]);
        free(rows.defs[i]);
    }
    free(rows.words);
    free(rows.defs);
    mb_free(&c);
}

/* Variantes de mayúsculas para no perder entradas: tal cual, todo en
 * minúsculas y con la primera letra en mayúscula (consultas por índice). */
static void case_variants(const char *w, mbuf *lower, mbuf *title)
{
    const char *s = w, *end = w + strlen(w);
    int first = 1;
    while (s < end) {
        uint32_t c = utf8_next(&s, end);
        uint32_t lc = md_lower(c);
        utf8_put(lower, lc);
        if (first) {
            /* mayúscula: inversa de md_lower para los casos comunes */
            uint32_t uc = lc;
            if (lc >= 'a' && lc <= 'z') uc = lc - 32;
            else if (lc >= 0xE0 && lc <= 0xFE && lc != 0xF7) uc = lc - 32;
            else if (lc >= 0x430 && lc <= 0x44F) uc = lc - 32;
            else if (lc >= 0x450 && lc <= 0x45F) uc = lc - 80;
            else if (lc >= 0x3B1 && lc <= 0x3C9 && lc != 0x3C2) uc = lc - 32;
            utf8_put(title, uc);
            first = 0;
        } else {
            utf8_put(title, lc);
        }
    }
    mb_cstr(lower);
    mb_cstr(title);
}

static int mdb_lookup(md_dict *d, const char *word, int fuzzy, int max, md_results *r)
{
    mdb *t = d->priv;
    if (fuzzy) {
        mbuf up = {0};
        mb_puts(&up, word);
        mb_puts(&up, "\xF4\x8F\xBF\xBF");
        mdb_query(t, t->q_prefix, word, up.p, max, r, 0);
        mb_free(&up);
        return r->n;
    }
    mbuf lower = {0}, title = {0};
    case_variants(word, &lower, &title);
    mdb_query(t, t->q_exact, word, NULL, max, r, 0);
    if (strcmp(lower.p, word)) mdb_query(t, t->q_exact, lower.p, NULL, max, r, 0);
    if (strcmp(title.p, word) && strcmp(title.p, lower.p)) mdb_query(t, t->q_exact, title.p, NULL, max, r, 0);
    mb_free(&lower);
    mb_free(&title);
    return r->n;
}

static int mdb_needs_index(const char *p, const char *c) { (void)p; (void)c; return 0; }

const md_format_t md_fmt_mdictdb = {"mdict-db", mdb_probe, mdb_needs_index, mdb_open, mdb_close, mdb_lookup};
