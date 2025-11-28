#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include "builddef.h"
#ifdef USE_FFMPEG

#include <libavutil/samplefmt.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_output audio_output;

audio_output* audio_output_init(int sample_rate, int channels, AVSampleFormat sample_fmt);
void audio_output_start(audio_output* ao);
void audio_output_pause(audio_output* ao);
void audio_output_resume(audio_output* ao);
int audio_output_write(audio_output* ao, const uint8_t* data, size_t len);
double audio_output_get_clock(audio_output* ao);
void audio_output_set_pts(audio_output* ao, uint64_t pts, double time_base);
void audio_output_flush(audio_output* ao);
void audio_output_destroy(audio_output* ao);
audio_output* audio_output_get_global(void);

#ifdef __cplusplus
}
#endif

#endif // USE_FFMPEG
#endif // AUDIO_OUTPUT_H

