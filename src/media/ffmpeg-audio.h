#ifndef FFMPEG_AUDIO_H
#define FFMPEG_AUDIO_H

#include "builddef.h"
#ifdef USE_FFMPEG

// FFmpeg headers need extern "C" when included from C++
#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif

#include <stdbool.h>
#include "lib/visual-details.h"
// Define API macro for visibility export
#ifndef __MINGW32__
#define API __attribute__((visibility("default")))
#else
#define API __declspec(dllexport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Helper functions to access audio stream information
// These functions allow play.cpp to check for audio and get audio parameters
// without directly accessing the internal structure

// Check if an ncvisual has an audio stream
API bool ffmpeg_has_audio(ncvisual* ncv);

// Get audio codec context (returns NULL if no audio)
API AVCodecContext* ffmpeg_get_audio_codec_context(ncvisual* ncv);

// Get audio stream index (returns -1 if no audio)
API int ffmpeg_get_audio_stream_index(ncvisual* ncv);

// Get audio stream time_base
API double ffmpeg_get_audio_time_base(ncvisual* ncv);

// Initialize audio resampler for output format
API int ffmpeg_init_audio_resampler_public(ncvisual* ncv, int out_sample_rate, int out_channels);

// Decode audio packet (public wrapper)
API int ffmpeg_decode_audio_public(ncvisual* ncv, AVPacket* packet);

// Resample audio frame (public wrapper)
API int ffmpeg_resample_audio_public(ncvisual* ncv, uint8_t** out_data, int* out_linesize,
								  int out_samples, int out_sample_rate, int out_channels);

// Read next audio packet from file (thread-safe, caller must free packet)
API int ffmpeg_read_audio_packet(ncvisual* ncv, AVPacket** pkt);

// Get the current decoded audio frame
API AVFrame* ffmpeg_get_audio_frame(ncvisual* ncv);

// Seek both video and audio streams to beginning (for looping)
API int ffmpeg_seek_to_start(ncvisual* ncv);

#ifdef __cplusplus
}
#endif

#endif // USE_FFMPEG
#endif // FFMPEG_AUDIO_H

