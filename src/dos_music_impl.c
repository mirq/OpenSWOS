/* Single translation unit that compiles the two vendored pieces of the PC music player
 * (src/dos_music.c): the Opal OPL3 emulator (third_party/opal, public domain) and the
 * Miles/AIL pitch and volume model (third_party/opl_models, MIT). SDL builds only. */
#ifdef SWOS_CHUNKY_BUILD
#include "../third_party/opal/opal.c"
#include "../third_party/opl_models/model_ail.c"
#endif
