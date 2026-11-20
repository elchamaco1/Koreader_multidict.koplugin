/* Descompresor Zstandard (solo decodificación) compilado como una sola unidad,
 * igual que el zstddeclib.c oficial. Código de zstd 1.5.7 (licencia BSD). */
/* las funciones de zstd no se exportan desde libmultidict */
#define ZSTDLIB_VISIBLE
#define ZSTDLIB_HIDDEN
#define ZDICTLIB_VISIBLE
#define ZSTDERRORLIB_VISIBLE
#define ZSTDERRORLIB_HIDDEN
#undef XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#define ZSTD_DISABLE_ASM 1
#define ZSTD_NO_TRACE 1
#define ZSTD_MULTITHREAD_DISABLED 1
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_STRIP_ERROR_STRINGS 1
#define DEBUGLEVEL 0
#include "zstd/common/debug.c"
#include "zstd/common/entropy_common.c"
#include "zstd/common/error_private.c"
#include "zstd/common/fse_decompress.c"
#include "zstd/common/xxhash.c"
#include "zstd/common/zstd_common.c"
#include "zstd/decompress/huf_decompress.c"
#include "zstd/decompress/zstd_ddict.c"
#include "zstd/decompress/zstd_decompress.c"
#include "zstd/decompress/zstd_decompress_block.c"
