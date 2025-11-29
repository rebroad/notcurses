#include "internal.h"

#ifndef __MINGW32__
__attribute__((visibility("default"))) ncloglevel_e loglevel = NCLOGLEVEL_SILENT;
#else
__declspec(dllexport) ncloglevel_e loglevel = NCLOGLEVEL_SILENT;
#endif

void notcurses_debug(const notcurses* nc, FILE* debugfp){
  fbuf f;
  if(fbuf_init_small(&f)){
    return;
  }
  notcurses_debug_fbuf(nc, &f);
  fbuf_finalize(&f, debugfp);
}
