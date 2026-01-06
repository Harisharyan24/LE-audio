
#include "lc3_pipe.h"
#include <lc3.h>  // from liblc3
struct lc3_decoder_s { lc3_decoder_t h; int nsamples; size_t nbytes; };

lc3_decoder_t lc3_decoder_create(int sr, double frame_ms, int ch)
{
    lc3_decoder_t d = calloc(1, sizeof(*d));
    d->h = lc3_setup_decoder(sr, frame_ms, ch);
    d->nsamples = lc3_frame_samples(sr, frame_ms);
    d->nbytes   = lc3_frame_bytes(sr, frame_ms);
    return d;
}
void lc3_decoder_destroy(lc3_decoder_t d){ if(!d) return; lc3_free(d->h); free(d); }
size_t lc3_frame_bytes(lc3_decoder_t d){ return d->nbytes; }
size_t lc3_samples_per_frame(lc3_decoder_t d){ return d->nsamples; }
void lc3_decode(lc3_decoder_t d, const uint8_t* in, size_t in_bytes, int16_t* out)
{
    lc3_decode(d->h, in, in_bytes, LC3_PCM_FORMAT_S16, out, d->nsamples);
}
