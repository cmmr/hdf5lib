#include "context.h"

/* blosc2_plugin.c keeps its contexts alive across filter calls so that their
   thread pools survive from one chunk to the next.  A context remembers the
   super-chunk it was created for, and codecs such as ZFP and NDLZ read their
   array geometry from it, so a reused context has to be pointed at each new
   super-chunk.  c-blosc2 has no public call for that.  This file is compiled
   with c-blosc2's own flags, so the context layout seen here is the real one. */
void hdf5lib_blosc2_ctx_set_schunk(blosc2_context *ctx, blosc2_schunk *schunk) {
  ctx->schunk = schunk;
}
