#include "builddef.h"
#ifdef USE_FFMPEG
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "lib/internal.h"

#define AUDIO_BUFFER_SIZE 4096

typedef struct audio_output {
  SDL_AudioDeviceID device_id;
  SDL_AudioSpec want_spec;
  SDL_AudioSpec have_spec;
  uint8_t* buffer;
  size_t buffer_size;
  size_t buffer_pos;
  size_t buffer_used;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool playing;
  bool paused;
  double audio_clock;  // Current audio position in seconds
  uint64_t audio_pts;  // Audio PTS in timebase units
  int sample_rate;
  int channels;
  AVSampleFormat sample_fmt;
} audio_output;

static audio_output* g_audio = NULL;

static void audio_callback(void* userdata, uint8_t* stream, int len) {
  audio_output* ao = (audio_output*)userdata;
  if (!ao || !ao->playing) {
    memset(stream, 0, len);
    return;
  }

  pthread_mutex_lock(&ao->mutex);

  size_t bytes_to_write = len;
  size_t bytes_available = ao->buffer_used - ao->buffer_pos;

  if (bytes_available == 0) {
    // Buffer underrun - fill with silence
    memset(stream, 0, len);
    pthread_mutex_unlock(&ao->mutex);
    return;
  }

  if (bytes_to_write > bytes_available) {
    bytes_to_write = bytes_available;
  }

  memcpy(stream, ao->buffer + ao->buffer_pos, bytes_to_write);
  ao->buffer_pos += bytes_to_write;
  ao->buffer_used -= bytes_to_write;

  // Update audio clock based on bytes written
  if (ao->sample_rate > 0 && ao->channels > 0) {
    double samples_written = bytes_to_write / (ao->channels * sizeof(int16_t));
    ao->audio_clock += samples_written / ao->sample_rate;
  }

  // Compact buffer if we've consumed a significant portion
  if (ao->buffer_pos > ao->buffer_size / 2) {
    memmove(ao->buffer, ao->buffer + ao->buffer_pos, ao->buffer_used);
    ao->buffer_pos = 0;
  }

  pthread_cond_signal(&ao->cond);
  pthread_mutex_unlock(&ao->mutex);

  // Fill remaining with silence if needed
  if (bytes_to_write < (size_t)len) {
    memset(stream + bytes_to_write, 0, len - bytes_to_write);
  }
}

audio_output* audio_output_init(int sample_rate, int channels, AVSampleFormat sample_fmt) {
  if (SDL_Init(SDL_INIT_AUDIO) < 0) {
    logerror("SDL audio init failed: %s", SDL_GetError());
    return NULL;
  }

  audio_output* ao = calloc(1, sizeof(audio_output));
  if (!ao) {
    return NULL;
  }

  ao->sample_rate = sample_rate;
  ao->channels = channels;
  ao->sample_fmt = sample_fmt;

  SDL_memset(&ao->want_spec, 0, sizeof(ao->want_spec));
  ao->want_spec.freq = sample_rate;
  ao->want_spec.format = AUDIO_S16SYS;  // Signed 16-bit samples, system byte order
  ao->want_spec.channels = channels;
  ao->want_spec.samples = AUDIO_BUFFER_SIZE;
  ao->want_spec.callback = audio_callback;
  ao->want_spec.userdata = ao;

  ao->device_id = SDL_OpenAudioDevice(NULL, 0, &ao->want_spec, &ao->have_spec,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
  if (ao->device_id == 0) {
    logerror("SDL_OpenAudioDevice failed: %s", SDL_GetError());
    free(ao);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return NULL;
  }

  // Update to actual specs
  ao->sample_rate = ao->have_spec.freq;
  ao->channels = ao->have_spec.channels;

  ao->buffer_size = sample_rate * channels * sizeof(int16_t) * 2; // 2 seconds buffer
  ao->buffer = malloc(ao->buffer_size);
  if (!ao->buffer) {
    SDL_CloseAudioDevice(ao->device_id);
    free(ao);
    return NULL;
  }

  if (pthread_mutex_init(&ao->mutex, NULL) != 0) {
    free(ao->buffer);
    SDL_CloseAudioDevice(ao->device_id);
    free(ao);
    return NULL;
  }

  if (pthread_cond_init(&ao->cond, NULL) != 0) {
    pthread_mutex_destroy(&ao->mutex);
    free(ao->buffer);
    SDL_CloseAudioDevice(ao->device_id);
    free(ao);
    return NULL;
  }

  ao->playing = false;
  ao->paused = false;
  ao->buffer_pos = 0;
  ao->buffer_used = 0;
  ao->audio_clock = 0.0;

  g_audio = ao;
  return ao;
}

void audio_output_start(audio_output* ao) {
  if (!ao) return;
  ao->playing = true;
  ao->paused = false;
  SDL_PauseAudioDevice(ao->device_id, 0);
}

void audio_output_pause(audio_output* ao) {
  if (!ao) return;
  ao->paused = true;
  SDL_PauseAudioDevice(ao->device_id, 1);
}

void audio_output_resume(audio_output* ao) {
  if (!ao) return;
  ao->paused = false;
  SDL_PauseAudioDevice(ao->device_id, 0);
}

int audio_output_write(audio_output* ao, const uint8_t* data, size_t len) {
  if (!ao || !data) return -1;

  pthread_mutex_lock(&ao->mutex);

  // Wait if buffer is too full
  while (ao->buffer_used + len > ao->buffer_size) {
    pthread_cond_wait(&ao->cond, &ao->mutex);
    if (!ao->playing) {
      pthread_mutex_unlock(&ao->mutex);
      return -1;
    }
  }

  memcpy(ao->buffer + ao->buffer_pos + ao->buffer_used, data, len);
  ao->buffer_used += len;

  pthread_mutex_unlock(&ao->mutex);
  return 0;
}

double audio_output_get_clock(audio_output* ao) {
  if (!ao) return 0.0;
  pthread_mutex_lock(&ao->mutex);
  double clock = ao->audio_clock;
  pthread_mutex_unlock(&ao->mutex);
  return clock;
}

void audio_output_set_pts(audio_output* ao, uint64_t pts, double time_base) {
  if (!ao) return;
  pthread_mutex_lock(&ao->mutex);
  ao->audio_pts = pts;
  ao->audio_clock = pts * time_base;
  pthread_mutex_unlock(&ao->mutex);
}

void audio_output_flush(audio_output* ao) {
  if (!ao) return;
  pthread_mutex_lock(&ao->mutex);
  ao->buffer_pos = 0;
  ao->buffer_used = 0;
  ao->audio_clock = 0.0;
  pthread_mutex_unlock(&ao->mutex);
}

void audio_output_destroy(audio_output* ao) {
  if (!ao) return;

  ao->playing = false;
  pthread_cond_broadcast(&ao->cond);

  SDL_CloseAudioDevice(ao->device_id);
  pthread_mutex_destroy(&ao->mutex);
  pthread_cond_destroy(&ao->cond);
  free(ao->buffer);
  free(ao);

  if (g_audio == ao) {
    g_audio = NULL;
  }

  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

audio_output* audio_output_get_global(void) {
  return g_audio;
}

#endif // USE_FFMPEG

