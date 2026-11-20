/* Definiciones internas de libmultidict (no forman parte de la API pública). */
#ifndef MD_INTERNAL_H
#define MD_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "multidict.h"

#define MD_HIDDEN __attribute__((visibility("hidden")))

/* ---------- búfer dinámico ---------- */
typedef struct {
    char *p;
    size_t n, cap;
} mbuf;

MD_HIDDEN int  mb_reserve(mbuf *b, size_t extra);
MD_HIDDEN void mb_put(mbuf *b, const void *s, size_t n);
MD_HIDDEN void mb_puts(mbuf *b, const char *s);
MD_HIDDEN void mb_putc(mbuf *b, char c);
MD_HIDDEN void mb_printf(mbuf *b, const char *fmt, ...);
MD_HIDDEN char *mb_cstr(mbuf *b);       /* garantiza terminador NUL, no lo cuenta en n */
MD_HIDDEN char *mb_steal(mbuf *b);      /* devuelve p (con NUL) y vacía el búfer */
MD_HIDDEN void mb_free(mbuf *b);
MD_HIDDEN void mb_html_escape(mbuf *b, const char *s, size_t n);

/* ---------- lectura aleatoria de archivos (planos, gzip, dictzip) ---------- */
typedef struct rfile rfile;
MD_HIDDEN rfile *rf_open(const char *path, char *err, int errlen);
MD_HIDDEN void rf_close(rfile *f);
MD_HIDDEN int rf_read(rfile *f, uint64_t off, size_t len, mbuf *out); /* añade a out; 0 = ok */
MD_HIDDEN size_t rf_pread(rfile *f, uint64_t off, void *dst, size_t len); /* bytes leídos */
MD_HIDDEN uint64_t rf_size(rfile *f);   /* tamaño descomprimido (UINT64_MAX si se desconoce) */

/* ---------- utilidades de texto ---------- */
enum { ENC_UTF8 = 0, ENC_UTF16LE = 1, ENC_UTF16BE = 2 };
MD_HIDDEN void utf16_to_utf8(const uint8_t *s, size_t n, int big_endian, mbuf *out);
MD_HIDDEN int  iconv_to_utf8(const char *enc, const char *s, size_t n, mbuf *out);
MD_HIDDEN uint32_t utf8_next(const char **ps, const char *end);
MD_HIDDEN void utf8_put(mbuf *b, uint32_t cp);
MD_HIDDEN uint32_t md_lower(uint32_t cp);

enum {
    FOLD_PLAIN = 0, /* solo minúsculas y espacios colapsados */
    FOLD_DICTD = 1, /* además: solo alfanuméricos y espacios (orden de diccionario) */
    FOLD_MDX   = 2, /* además: elimina signos de puntuación y espacios (StripKey) */
    FOLD_LOOSE = 3, /* sin mayúsculas, acentos ni puntuación (selección de bloques) */
    FOLD_LOOSE_T = 4, /* igual, pero se detiene en el primer signo de puntuación */
    FOLD_ROOT = 5     /* como FOLD_LOOSE con la colación ICU raíz (й es letra propia) */
};
MD_HIDDEN void md_fold(const char *s, size_t n, int mode, int keep_case, mbuf *out);
MD_HIDDEN int  md_keycmp(const char *a, size_t an, const char *b, size_t bn);
MD_HIDDEN int  md_has_prefix(const char *s, size_t sn, const char *pre, size_t pn);

MD_HIDDEN int  ends_with_ci(const char *s, const char *suffix);
MD_HIDDEN char *md_strdup(const char *s);
MD_HIDDEN char *md_strndup(const char *s, size_t n);
MD_HIDDEN char *md_basename_noext(const char *path);
MD_HIDDEN void md_seterr(char *err, int errlen, const char *fmt, ...);

/* ---------- resultados ---------- */
struct md_results {
    int n, cap;
    char **words;
    char **defs;
};
MD_HIDDEN int md_results_add(md_results *r, const char *word, size_t wn, const char *def, size_t dn);
MD_HIDDEN int md_results_has(md_results *r, const char *word, const char *def);

/* ---------- diccionario ---------- */
typedef struct md_format md_format_t;

struct md_dict {
    const md_format_t *fmt;
    char *path;
    char *name;
    char *lang_from, *lang_to;
    int is_html;
    long word_count;
    void *priv;
};

struct md_format {
    const char *id;
    int  (*probe)(const char *path);
    int  (*needs_index)(const char *path, const char *cache_dir);
    int  (*open)(md_dict *d, const char *cache_dir, char *err, int errlen);
    void (*close)(md_dict *d);
    /* key: palabra ya normalizada con md_fold(FOLD_PLAIN) */
    int  (*lookup)(md_dict *d, const char *word, int fuzzy, int max, md_results *r);
};

extern MD_HIDDEN const md_format_t md_fmt_mdx;
extern MD_HIDDEN const md_format_t md_fmt_dictd;
extern MD_HIDDEN const md_format_t md_fmt_dsl;
extern MD_HIDDEN const md_format_t md_fmt_xdxf;
extern MD_HIDDEN const md_format_t md_fmt_tab;
extern MD_HIDDEN const md_format_t md_fmt_dc;
extern MD_HIDDEN const md_format_t md_fmt_mdictdb;
extern MD_HIDDEN const md_format_t md_fmt_kobo;
extern MD_HIDDEN const md_format_t md_fmt_sdt;
extern MD_HIDDEN const md_format_t md_fmt_csv;
extern MD_HIDDEN const md_format_t md_fmt_quickdic;
extern MD_HIDDEN const md_format_t md_fmt_slob;
extern MD_HIDDEN const md_format_t md_fmt_zim;
extern MD_HIDDEN const md_format_t md_fmt_sdic;
extern MD_HIDDEN const md_format_t md_fmt_apple;
extern MD_HIDDEN const md_format_t md_fmt_bgl;
extern MD_HIDDEN const md_format_t md_fmt_epub;
extern MD_HIDDEN const md_format_t md_fmt_mobi;

/* ---------- índice auxiliar ordenado (para formatos sin índice propio) ---------- */
typedef struct sidx_builder sidx_builder;
MD_HIDDEN sidx_builder *sidx_new(void);
MD_HIDDEN void sidx_add(sidx_builder *b, const char *word, size_t wn, uint64_t off, uint32_t len);
MD_HIDDEN int  sidx_write(sidx_builder *b, const char *out_path, const char *src_path,
                          const char *name, const char *lang_from, const char *lang_to,
                          char *err, int errlen);
MD_HIDDEN void sidx_free(sidx_builder *b);

typedef struct sidx sidx;
typedef struct {
    char *word;
    uint64_t off;
    uint32_t len;
} sidx_hit;
MD_HIDDEN char *sidx_path_for(const char *src_path, const char *cache_dir, int for_write);
MD_HIDDEN int   sidx_is_fresh(const char *idx_path, const char *src_path);
MD_HIDDEN sidx *sidx_open(const char *idx_path, char **name, char **lang_from, char **lang_to,
                          long *count, char *err, int errlen);
MD_HIDDEN void  sidx_close(sidx *s);
/* Devuelve el número de aciertos escritos en hits (máx. max); el llamador libera hits[i].word */
MD_HIDDEN int   sidx_find(sidx *s, const char *word, int prefix, sidx_hit *hits, int max);

/* ---------- lector de líneas sobre un rfile ---------- */
typedef struct {
    rfile *f;
    int enc;
    uint64_t pos;     /* offset del siguiente byte sin consumir */
    uint64_t size;
    mbuf raw;         /* ventana de datos crudos */
    uint64_t raw_off; /* offset del primer byte de raw */
    size_t raw_pos;
    int eof;
} linereader;
MD_HIDDEN int  lr_init(linereader *lr, rfile *f);  /* detecta codificación y salta BOM */
/* lee una línea; devuelve 1 si hay línea. line = UTF-8 sin salto final.
 * start/end = offsets crudos (en la codificación original) de la línea, end incluye el salto. */
MD_HIDDEN int  lr_next(linereader *lr, mbuf *line, uint64_t *start, uint64_t *end);
MD_HIDDEN void lr_free(linereader *lr);
MD_HIDDEN void decode_raw(const char *s, size_t n, int enc, mbuf *out);

/* hashing / descompresión */
MD_HIDDEN void ripemd128(const uint8_t *msg, size_t len, uint8_t out[16]);
/* codecs.c */
MD_HIDDEN int  md_decompress(int method, const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len);
MD_HIDDEN void md_salsa20_8(uint8_t *data, size_t n, const uint8_t key[16]);
MD_HIDDEN uint64_t md_xxh64(const void *data, size_t n);
MD_HIDDEN uint8_t *md_lzma2_raw_heap(const uint8_t *in, size_t in_len, size_t *out_len);
MD_HIDDEN uint8_t *md_xz_heap(const uint8_t *in, size_t in_len, size_t *out_len);
MD_HIDDEN uint8_t *md_bzip2_heap(const uint8_t *in, size_t in_len, size_t *out_len);
MD_HIDDEN int  md_lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len);
MD_HIDDEN int  md_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, int zlib_header); /* bytes o -1 */
MD_HIDDEN uint8_t *md_inflate_heap(const uint8_t *in, size_t in_len, size_t *out_len, int zlib_header);

#endif
