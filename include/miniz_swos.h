/*
 * miniz_swos.h -- how OpenSWOS builds miniz (third_party/miniz/OPENSWOS.md).
 *
 * Include this, never miniz.h directly: the configuration below has to be the same in
 * every file that sees miniz, including the one that compiles it (src/miniz_impl.c).
 */
#ifndef MINIZ_SWOS_H
#define MINIZ_SWOS_H

#define MINIZ_NO_STDIO                  /* buffers only: no files */
#ifdef SWOS_CHUNKY_BUILD
/* ZIP archives, in memory: the netplay match report (src/match_report.c) is one
 * zip of both players' logs. The entries carry the time they were written. */
#else
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS           /* the Amiga: no ZIP reading or writing */
#endif
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES  /* mz_* names only: never clashes with a system zlib */

#include "../third_party/miniz/miniz.h"

#endif /* MINIZ_SWOS_H */
