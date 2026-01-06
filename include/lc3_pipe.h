#pragma once
#include <stdint.h>
typedef struct lc3_decoder_s* lc3_decoder_t;
lc3_decoder_t lc3_decoder_create(int samplerate, double frame_ms, int channels);
void lc3_decoder_destroy(lc3_decoder_t d);
size_t lc3_frame_bytes(lc3_decoder_t d);
size_t lc3_samples_per_frame(lc3_decoder_t d);
void lc3_decode(lc3_decoder_t d, const uint8_t* in, size_t in_bytes, int16_t* out);
