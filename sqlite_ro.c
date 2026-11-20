/* SQLite 3.46 (dominio público) configurado para abrir diccionarios en modo
 * solo lectura: sin hilos, sin extensiones cargables y sin lo obsoleto. */
#define SQLITE_THREADSAFE 0
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_SHARED_CACHE 1
#define SQLITE_OMIT_PROGRESS_CALLBACK 1
#define SQLITE_OMIT_AUTHORIZATION 1
#define SQLITE_OMIT_TRACE 1
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_DQS 0
#define SQLITE_MAX_EXPR_DEPTH 0
#define SQLITE_TEMP_STORE 3
#define SQLITE_ENABLE_REGEXP 0
#include "sqlite/sqlite3.c"
