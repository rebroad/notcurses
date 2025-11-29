#include "builddef.h"
#ifdef USE_FFMPEG
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
#include <libswscale/version.h>
#include <libswresample/swresample.h>
#include <libswresample/version.h>
// libavdevice removed - it depends on SDL2 which interferes with terminal initialization
// avdevice is only needed for device I/O, not file playback
// #include <libavdevice/avdevice.h>
#include <libavformat/version.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include "lib/visual-details.h"
#include "lib/internal.h"
// Define API macro for visibility export
#ifndef __MINGW32__
#define API __attribute__((visibility("default")))
#else
#define API __declspec(dllexport)
#endif
#include "ffmpeg-audio.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVCodec;
struct AVCodecParameters;
struct AVPacket;

typedef struct ncvisual_details {
  struct AVFormatContext* fmtctx;
  struct AVCodecContext* codecctx;     // video codec context
  struct AVCodecContext* subtcodecctx; // subtitle codec context
  struct AVCodecContext* audiocodecctx; // audio codec context
  struct AVFrame* frame;               // frame as read/loaded/converted
  struct AVFrame* audio_frame;         // decoded audio frame
  struct AVCodec* codec;
  struct AVCodec* subtcodec;
  struct AVCodec* audiocodec;
  struct AVPacket* packet;
  struct SwsContext* swsctx;
  struct SwsContext* rgbactx;
  struct SwrContext* swrctx;           // audio resampler context
  AVSubtitle subtitle;
  int stream_index;        // match against this following av_read_frame()
  int sub_stream_index;    // subtitle stream index, can be < 0 if no subtitles
  int audio_stream_index;  // audio stream index, can be < 0 if no audio
  bool packet_outstanding;
  bool audio_packet_outstanding;
  pthread_mutex_t packet_mutex; // protects packet reading from format context
} ncvisual_details;

#define IMGALLOCALIGN 64

uint64_t ffmpeg_pkt_duration(const AVFrame* frame){
#if LIBAVUTIL_VERSION_MAJOR < 58
      return frame->pkt_duration;
#else
      return frame->duration;
#endif
}

/*static void
print_frame_summary(const AVCodecContext* cctx, const AVFrame* f){
  if(f == NULL){
    fprintf(stderr, "NULL frame\n");
    return;
  }
  char pfmt[128];
  av_get_pix_fmt_string(pfmt, sizeof(pfmt), f->format);
  fprintf(stderr, "Frame %05d %p (%d? %d?) %dx%d pfmt %d (%s)\n",
          cctx ? cctx->frame_number : 0, f,
          f->coded_picture_number,
          f->display_picture_number,
          f->width, f->height,
          f->format, pfmt);
  fprintf(stderr, " Data (%d):", AV_NUM_DATA_POINTERS);
  int i;
  for(i = 0 ; i < AV_NUM_DATA_POINTERS ; ++i){
    fprintf(stderr, " %p", f->data[i]);
  }
  fprintf(stderr, "\n Linesizes:");
  for(i = 0 ; i < AV_NUM_DATA_POINTERS ; ++i){
    fprintf(stderr, " %d", f->linesize[i]);
  }
  if(f->sample_aspect_ratio.num == 0 && f->sample_aspect_ratio.den == 1){
    fprintf(stderr, "\n Aspect ratio unknown");
  }else{
    fprintf(stderr, "\n Aspect ratio %d:%d", f->sample_aspect_ratio.num, f->sample_aspect_ratio.den);
  }
  if(f->interlaced_frame){
    fprintf(stderr, " [ILaced]");
  }
  if(f->palette_has_changed){
    fprintf(stderr, " [NewPal]");
  }
  fprintf(stderr, " PTS %" PRId64 " Flags: 0x%04x\n", f->pts, f->flags);
  fprintf(stderr, " %" PRIu64 "ms@%" PRIu64 "ms (%skeyframe) qual: %d\n",
          ffmpeg_pkt_duration(f), // FIXME in 'time_base' units
          f->best_effort_timestamp,
          f->key_frame ? "" : "non-",
          f->quality);
}*/

static char*
deass(const char* ass){
  // SSA/ASS formats:
  // Dialogue: Marked=0,0:02:40.65,0:02:41.79,Wolf main,Cher,0000,0000,0000,,Et les enregistrements de ses ondes delta ?
  // FIXME more
  if(strncmp(ass, "Dialogue:", strlen("Dialogue:"))){
    return NULL;
  }
  const char* delim = strchr(ass, ',');
  int commas = 0; // we want 8
  while(delim && commas < 8){
    delim = strchr(delim + 1, ',');
    ++commas;
  }
  if(!delim){
    return NULL;
  }
  // handle ASS syntax...\i0, \b0, etc.
  char* dup = strdup(delim + 1);
  char* c = dup;
  while(*c){
    if(*c == '\\'){
      *c = ' ';
      ++c;
      if(*c){
        *c = ' ';;
      }
    }
    ++c;
  }
  return dup;
}

static struct ncplane*
subtitle_plane_from_text(ncplane* parent, const char* text){
  if(parent == NULL){
//logerror("need a parent plane\n");
    return NULL;
  }
  int width = ncstrwidth(text, NULL, NULL);
  if(width <= 0){
//logwarn("couldn't extract subtitle from %s\n", text);
    return NULL;
  }
  int rows = (width + ncplane_dim_x(parent) - 1) / ncplane_dim_x(parent);
  struct ncplane_options nopts = {
    .y = ncplane_dim_y(parent) - (rows + 1),
    .rows = rows,
    .cols = ncplane_dim_x(parent),
    .name = "subt",
  };
  struct ncplane* n = ncplane_create(parent, &nopts);
  if(n == NULL){
//logerror("error creating subtitle plane\n");
    return NULL;
  }
  uint64_t channels = 0;
  ncchannels_set_fg_alpha(&channels, NCALPHA_HIGHCONTRAST);
  ncchannels_set_fg_rgb8(&channels, 0x88, 0x88, 0x88);
  ncplane_stain(n, -1, -1, 0, 0, channels, channels, channels, channels);
  ncchannels_set_fg_default(&channels);
  ncplane_puttext(n, 0, NCALIGN_LEFT, text, NULL);
  ncchannels_set_bg_alpha(&channels, NCALPHA_TRANSPARENT);
  ncplane_set_base(n, " ", 0, channels);
  return n;
}

static uint32_t palette[NCPALETTESIZE];

struct ncplane* ffmpeg_subtitle(ncplane* parent, const ncvisual* ncv){
  for(unsigned i = 0 ; i < ncv->details->subtitle.num_rects ; ++i){
    // it is possible that there are more than one subtitle rects present,
    // but we only bother dealing with the first one we find FIXME?
    const AVSubtitleRect* rect = ncv->details->subtitle.rects[i];
    if(rect->type == SUBTITLE_ASS){
      char* ass = deass(rect->ass);
      struct ncplane* n = NULL;
      if(ass){
        n = subtitle_plane_from_text(parent, ass);
      }
      free(ass);
      return n;
    }else if(rect->type == SUBTITLE_TEXT){;
      return subtitle_plane_from_text(parent, rect->text);
    }else if(rect->type == SUBTITLE_BITMAP){
      // there are technically up to AV_NUM_DATA_POINTERS planes, but we
      // only try to work with the first FIXME?
      if(rect->linesize[0] != rect->w){
//logwarn("bitmap subtitle size %d != width %d\n", rect->linesize[0], rect->w);
        continue;
      }
      struct notcurses* nc = ncplane_notcurses(parent);
      const unsigned cellpxy = ncplane_pile_const(parent)->cellpxy;
      const unsigned cellpxx = ncplane_pile_const(parent)->cellpxx;
      if(cellpxy <= 0 || cellpxx <= 0){
        continue;
      }
      struct ncvisual* v = ncvisual_from_palidx(rect->data[0], rect->h,
                                                rect->w, rect->w,
                                                NCPALETTESIZE, 1, palette);
      if(v == NULL){
        return NULL;
      }
      int rows = (rect->h + cellpxx - 1) / cellpxy;
      struct ncplane_options nopts = {
        .rows = rows,
        .cols = (rect->w + cellpxx - 1) / cellpxx,
        .y = ncplane_dim_y(parent) - rows - 1,
        .name = "t1st",
      };
      struct ncplane* vn = ncplane_create(parent, &nopts);
      if(vn == NULL){
        ncvisual_destroy(v);
        return NULL;
      }
      struct ncvisual_options vopts = {
        .n = vn,
        .blitter = NCBLIT_PIXEL,
        .scaling = NCSCALE_STRETCH,
      };
      if(ncvisual_blit(nc, v, &vopts) == NULL){
        ncplane_destroy(vn);
        ncvisual_destroy(v);
        return NULL;
      }
      ncvisual_destroy(v);
      return vn;
    }
  }
  return NULL;
}

static int
averr2ncerr(int averr){
  if(averr == AVERROR_EOF){
    return 1;
  }
  // FIXME need to map averror codes to ncerrors
//fprintf(stderr, "AVERR: %d/%x %d/%x\n", averr, averr, -averr, -averr);
  return -1;
}

// force an AVImage to RGBA for safe use with the ncpixel API
static int
force_rgba(ncvisual* n){
  const int targformat = AV_PIX_FMT_RGBA;
  AVFrame* inf = n->details->frame;
//fprintf(stderr, "%p got format: %d (%d/%d) want format: %d (%d/%d)\n", n->details->frame, inf->format, n->pixy, n->pixx, targformat);
  if(inf->format == targformat){
    return 0;
  }
  AVFrame* sframe = av_frame_alloc();
  if(sframe == NULL){
//fprintf(stderr, "Couldn't allocate output frame for scaled frame\n");
    return -1;
  }
//fprintf(stderr, "WHN NCV: %d/%d\n", inf->width, inf->height);
  n->details->rgbactx = sws_getCachedContext(n->details->rgbactx,
                                            inf->width, inf->height, inf->format,
                                            inf->width, inf->height, targformat,
                                            SWS_LANCZOS, NULL, NULL, NULL);
  if(n->details->rgbactx == NULL){
//fprintf(stderr, "Error retrieving details->rgbactx\n");
    return -1;
  }
  memcpy(sframe, inf, sizeof(*inf));
  sframe->format = targformat;
  sframe->width = inf->width;
  sframe->height = inf->height;
  int size = av_image_alloc(sframe->data, sframe->linesize,
                            sframe->width, sframe->height,
                            sframe->format,
                            IMGALLOCALIGN);
  if(size < 0){
//fprintf(stderr, "Error allocating visual data (%d X %d)\n", sframe->height, sframe->width);
    return -1;
  }
//fprintf(stderr, "INFRAME DAA: %p SDATA: %p FDATA: %p\n", inframe->data[0], sframe->data[0], ncv->details->frame->data[0]);
  int height = sws_scale(n->details->rgbactx, (const uint8_t* const*)inf->data,
                         inf->linesize, 0, inf->height, sframe->data,
                         sframe->linesize);
  if(height < 0){
//fprintf(stderr, "Error applying converting %d\n", inf->format);
    av_frame_free(&sframe);
    return -1;
  }
  int bpp = av_get_bits_per_pixel(av_pix_fmt_desc_get(sframe->format));
  if(bpp != 32){
//fprintf(stderr, "Bad bits-per-pixel (wanted 32, got %d)\n", bpp);
    av_frame_free(&sframe);
    return -1;
  }
  n->rowstride = sframe->linesize[0];
  if((uint32_t*)sframe->data[0] != n->data){
//fprintf(stderr, "SETTING UP RESIZE %p\n", n->data);
    if(n->details->frame){
      if(n->owndata){
        // we don't free the frame data here, because it's going to be
        // freed (if appropriate) by ncvisual_set_data() momentarily.
        av_freep(&n->details->frame);
      }
    }
    ncvisual_set_data(n, sframe->data[0], true);
  }
  n->details->frame = sframe;
  return 0;
}

// turn arbitrary input packets into RGBA frames. reads packets until it gets
// a visual frame. a packet might contain several frames (this is typically
// true only of audio), and a frame might be carried across several packets.
// * avcodec_receive_frame() returns EAGAIN if it needs more packets.
// * avcodec_send_packet() returns EAGAIN if avcodec_receive_frame() needs
//    be called to extract further frames; in this case, the packet ought
//    be resubmitted once the existing frames are cleared.
static int
ffmpeg_decode(ncvisual* n){
  if(n->details->fmtctx == NULL){ // not a file-backed ncvisual
    return -1;
  }
  bool have_frame = false;
  bool unref = false;
  // note that there are two loops here; once we're out of the external one,
  // we've either returned a failure, or we have a frame. averr2ncerr()
  // translates AVERROR_EOF into a return of 1.
  do{
    if(!n->details->packet_outstanding){
      do{
        if(unref){
          av_packet_unref(n->details->packet);
        }
        int averr;
        pthread_mutex_lock(&n->details->packet_mutex);
        averr = av_read_frame(n->details->fmtctx, n->details->packet);
        pthread_mutex_unlock(&n->details->packet_mutex);
        if(averr < 0){
          /*if(averr != AVERROR_EOF){
            fprintf(stderr, "Error reading frame info (%s)\n", av_err2str(averr));
          }*/
          return averr2ncerr(averr);
        }
        unref = true;
        if(n->details->packet->stream_index == n->details->sub_stream_index){
          int result = 0, ret;
          avsubtitle_free(&n->details->subtitle);
          ret = avcodec_decode_subtitle2(n->details->subtcodecctx, &n->details->subtitle, &result, n->details->packet);
          if(ret >= 0 && result){
            // FIXME?
          }
        }
      }while(n->details->packet->stream_index != n->details->stream_index);
      n->details->packet_outstanding = true;
      int averr = avcodec_send_packet(n->details->codecctx, n->details->packet);
      if(averr < 0){
        n->details->packet_outstanding = false;
        av_packet_unref(n->details->packet);
  //fprintf(stderr, "Error processing AVPacket\n");
        return averr2ncerr(averr);
      }
    }
    int averr = avcodec_receive_frame(n->details->codecctx, n->details->frame);
    if(averr >= 0){
      have_frame = true;
    }else if(averr < 0){
      av_packet_unref(n->details->packet);
      have_frame = false;
      n->details->packet_outstanding = false;
      if(averr != AVERROR(EAGAIN)){
        return averr2ncerr(averr);
      }
    }
//fprintf(stderr, "Error decoding AVPacket\n");
  }while(!have_frame);
//print_frame_summary(n->details->codecctx, n->details->frame);
  const AVFrame* f = n->details->frame;
  n->rowstride = f->linesize[0];
  n->pixx = n->details->frame->width;
  n->pixy = n->details->frame->height;
//fprintf(stderr, "good decode! %d/%d %d %p\n", n->details->frame->height, n->details->frame->width, n->rowstride, f->data);
  ncvisual_set_data(n, f->data[0], false);
  force_rgba(n);
  return 0;
}

static ncvisual_details*
ffmpeg_details_init(void){
  ncvisual_details* deets = malloc(sizeof(*deets));
  if(deets){
    memset(deets, 0, sizeof(*deets));
    deets->stream_index = -1;
    deets->sub_stream_index = -1;
    deets->audio_stream_index = -1;
    if(pthread_mutex_init(&deets->packet_mutex, NULL) != 0){
      free(deets);
      return NULL;
    }
    if((deets->frame = av_frame_alloc()) == NULL){
      pthread_mutex_destroy(&deets->packet_mutex);
      free(deets);
      return NULL;
    }
    if((deets->audio_frame = av_frame_alloc()) == NULL){
      av_frame_free(&deets->frame);
      pthread_mutex_destroy(&deets->packet_mutex);
      free(deets);
      return NULL;
    }
  }
  return deets;
}

static ncvisual*
ffmpeg_create(){
  ncvisual* nc = malloc(sizeof(*nc));
  if(nc){
    memset(nc, 0, sizeof(*nc));
    if((nc->details = ffmpeg_details_init()) == NULL){
      free(nc);
      return NULL;
    }
  }
  return nc;
}

static ncvisual*
ffmpeg_from_file(const char* filename){
  ncvisual* ncv = ffmpeg_create();
  if(ncv == NULL){
    // fprintf(stderr, "Couldn't create %s (%s)\n", filename, strerror(errno));
    return NULL;
  }
//fprintf(stderr, "FRAME FRAME: %p\n", ncv->details->frame);
  int averr = avformat_open_input(&ncv->details->fmtctx, filename, NULL, NULL);
  if(averr < 0){
//fprintf(stderr, "Couldn't open %s (%d)\n", filename, averr);
    goto err;
  }
  averr = avformat_find_stream_info(ncv->details->fmtctx, NULL);
  if(averr < 0){
//fprintf(stderr, "Error extracting stream info from %s (%d)\n", filename, averr);
    goto err;
  }
//av_dump_format(ncv->details->fmtctx, 0, filename, false);
  if((averr = av_find_best_stream(ncv->details->fmtctx, AVMEDIA_TYPE_SUBTITLE, -1, -1,
#if LIBAVFORMAT_VERSION_MAJOR >= 59
                                  (const AVCodec**)&ncv->details->subtcodec, 0)) >= 0){
#else
                                  &ncv->details->subtcodec, 0)) >= 0){
#endif
    ncv->details->sub_stream_index = averr;
    if((ncv->details->subtcodecctx = avcodec_alloc_context3(ncv->details->subtcodec)) == NULL){
      //fprintf(stderr, "Couldn't allocate decoder for %s\n", filename);
      goto err;
    }
    // FIXME do we need avcodec_parameters_to_context() here?
    if(avcodec_open2(ncv->details->subtcodecctx, ncv->details->subtcodec, NULL) < 0){
      //fprintf(stderr, "Couldn't open codec for %s (%s)\n", filename, av_err2str(*averr));
      goto err;
    }
  }else{
    ncv->details->sub_stream_index = -1;
  }
  // Find audio stream
  if((averr = av_find_best_stream(ncv->details->fmtctx, AVMEDIA_TYPE_AUDIO, -1, -1,
#if LIBAVFORMAT_VERSION_MAJOR >= 59
                                  (const AVCodec**)&ncv->details->audiocodec, 0)) >= 0){
#else
                                  &ncv->details->audiocodec, 0)) >= 0){
#endif
    ncv->details->audio_stream_index = averr;
    AVStream* ast = ncv->details->fmtctx->streams[ncv->details->audio_stream_index];
    if((ncv->details->audiocodecctx = avcodec_alloc_context3(ncv->details->audiocodec)) == NULL){
      //fprintf(stderr, "Couldn't allocate audio decoder for %s\n", filename);
      goto err;
    }
    if(avcodec_parameters_to_context(ncv->details->audiocodecctx, ast->codecpar) < 0){
      goto err;
    }
    if(avcodec_open2(ncv->details->audiocodecctx, ncv->details->audiocodec, NULL) < 0){
      //fprintf(stderr, "Couldn't open audio codec for %s\n", filename);
      goto err;
    }
  }else{
    ncv->details->audio_stream_index = -1;
  }
//fprintf(stderr, "FRAME FRAME: %p\n", ncv->details->frame);
  if((ncv->details->packet = av_packet_alloc()) == NULL){
    // fprintf(stderr, "Couldn't allocate packet for %s\n", filename);
    goto err;
  }
  if((averr = av_find_best_stream(ncv->details->fmtctx, AVMEDIA_TYPE_VIDEO, -1, -1,
#if LIBAVFORMAT_VERSION_MAJOR >= 59
                                  (const AVCodec**)&ncv->details->codec, 0)) < 0){
#else
                                  &ncv->details->codec, 0)) < 0){
#endif
    // fprintf(stderr, "Couldn't find visuals in %s (%s)\n", filename, av_err2str(*averr));
    goto err;
  }
  ncv->details->stream_index = averr;
  if(ncv->details->codec == NULL){
    //fprintf(stderr, "Couldn't find decoder for %s\n", filename);
    goto err;
  }
  AVStream* st = ncv->details->fmtctx->streams[ncv->details->stream_index];
  if((ncv->details->codecctx = avcodec_alloc_context3(ncv->details->codec)) == NULL){
    //fprintf(stderr, "Couldn't allocate decoder for %s\n", filename);
    goto err;
  }
  if(avcodec_parameters_to_context(ncv->details->codecctx, st->codecpar) < 0){
    goto err;
  }
  if(avcodec_open2(ncv->details->codecctx, ncv->details->codec, NULL) < 0){
    //fprintf(stderr, "Couldn't open codec for %s (%s)\n", filename, av_err2str(*averr));
    goto err;
  }
//fprintf(stderr, "FRAME FRAME: %p\n", ncv->details->frame);
  // frame is set up in prep_details(), so that format can be set there, as
  // is necessary when it is prepared from inputs other than files.
  if(ffmpeg_decode(ncv)){
    goto err;
  }
  return ncv;

err:
  ncvisual_destroy(ncv);
  return NULL;
}

// Decode audio packet and return number of samples decoded
// Returns: >0 = samples decoded, 0 = need more data, <0 = error
static int
ffmpeg_decode_audio(ncvisual* ncv, AVPacket* packet){
  if(!ncv->details->audiocodecctx || ncv->details->audio_stream_index < 0){
    return -1;
  }

  if(packet && packet->stream_index == ncv->details->audio_stream_index){
    int averr = avcodec_send_packet(ncv->details->audiocodecctx, packet);
    if(averr < 0 && averr != AVERROR(EAGAIN) && averr != AVERROR_EOF){
      return averr2ncerr(averr);
    }
    ncv->details->audio_packet_outstanding = (averr == 0);
  }

  if(!ncv->details->audio_packet_outstanding){
    return 0;
  }

  int averr = avcodec_receive_frame(ncv->details->audiocodecctx, ncv->details->audio_frame);
  if(averr == 0){
    ncv->details->audio_packet_outstanding = false;
    return ncv->details->audio_frame->nb_samples;
  }else if(averr == AVERROR(EAGAIN)){
    return 0; // need more packets
  }else if(averr == AVERROR_EOF){
    return 1; // EOF
  }else{
    return averr2ncerr(averr);
  }
}

// Initialize audio resampler for converting to SDL format
static int
ffmpeg_init_audio_resampler(ncvisual* ncv, int out_sample_rate, int out_channels){
  FILE* log_file = fopen("/tmp/ncplayer_audio.log", "a");

  if(!ncv->details->audiocodecctx || ncv->details->audio_stream_index < 0){
    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: no codec context or invalid stream index\n");
      fclose(log_file);
    }
    return -1;
  }

  AVCodecContext* acodecctx = ncv->details->audiocodecctx;
  AVChannelLayout out_ch_layout = {0};
  AVChannelLayout in_ch_layout = {0};

  if(log_file){
    fprintf(log_file, "ffmpeg_init_audio_resampler: codec context: sample_rate=%d, sample_fmt=%d, ch_layout.nb_channels=%d\n",
            acodecctx->sample_rate, acodecctx->sample_fmt, acodecctx->ch_layout.nb_channels);
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    fprintf(log_file, "ffmpeg_init_audio_resampler: codec context (old API): channels=%d\n", acodecctx->channels);
    #pragma GCC diagnostic pop
    fflush(log_file);
  }

  // Initialize output channel layout
  if(out_channels == 1){
    av_channel_layout_default(&out_ch_layout, 1);
  }else{
    av_channel_layout_default(&out_ch_layout, 2);
  }

  if(log_file){
    fprintf(log_file, "ffmpeg_init_audio_resampler: output layout initialized: nb_channels=%d\n", out_ch_layout.nb_channels);
    fflush(log_file);
  }

  // Get input channel layout
  int ret = 0;
  if(acodecctx->ch_layout.nb_channels == 0){
    // Old API - need to construct channel layout from deprecated channels field
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    int in_channels = acodecctx->channels;
    #pragma GCC diagnostic pop
    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: using old API, creating layout for %d channels\n", in_channels);
      fflush(log_file);
    }
    av_channel_layout_default(&in_ch_layout, in_channels);
  }else{
    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: copying existing channel layout (%d channels)\n",
              acodecctx->ch_layout.nb_channels);
      fflush(log_file);
    }
    ret = av_channel_layout_copy(&in_ch_layout, &acodecctx->ch_layout);
    if(ret < 0){
      if(log_file){
        fprintf(log_file, "ffmpeg_init_audio_resampler: failed to copy input channel layout (ret=%d)\n", ret);
        fclose(log_file);
      }
      av_channel_layout_uninit(&out_ch_layout);
      return -1;
    }
  }

  if(log_file){
    fprintf(log_file, "ffmpeg_init_audio_resampler: input layout initialized: nb_channels=%d\n", in_ch_layout.nb_channels);
    fprintf(log_file, "ffmpeg_init_audio_resampler: input=%d ch, %d Hz, fmt=%d -> output=%d ch, %d Hz, fmt=%d\n",
            in_ch_layout.nb_channels, acodecctx->sample_rate, acodecctx->sample_fmt,
            out_ch_layout.nb_channels, out_sample_rate, AV_SAMPLE_FMT_S16);
    fflush(log_file);
  }

  // Validate channel layouts before using them
  if(in_ch_layout.nb_channels == 0 || out_ch_layout.nb_channels == 0){
    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: invalid channel layout (in=%d, out=%d)\n",
              in_ch_layout.nb_channels, out_ch_layout.nb_channels);
      fclose(log_file);
    }
    av_channel_layout_uninit(&out_ch_layout);
    av_channel_layout_uninit(&in_ch_layout);
    return -1;
  }

  // Ensure swrctx is NULL before allocation
  if(ncv->details->swrctx){
    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: freeing existing swrctx\n");
      fflush(log_file);
    }
    swr_free(&ncv->details->swrctx);
  }
  ncv->details->swrctx = NULL; // Ensure it's NULL

  // Use older API with channel masks (more stable, avoids segfaults)
  // Clean up channel layouts first
  av_channel_layout_uninit(&out_ch_layout);
  av_channel_layout_uninit(&in_ch_layout);

  {
      if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: using older API with channel masks...\n");
      fflush(log_file);
    }

    // Convert to channel masks for older API
    uint64_t in_channel_mask = 0;
    uint64_t out_channel_mask = 0;

    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    // Get channel count
    int in_ch_count = (acodecctx->ch_layout.nb_channels > 0) ?
                      acodecctx->ch_layout.nb_channels : acodecctx->channels;

    // Map channel count to standard layout masks
    if(in_ch_count == 1){
      in_channel_mask = AV_CH_LAYOUT_MONO;
    }else if(in_ch_count == 2){
      in_channel_mask = AV_CH_LAYOUT_STEREO;
    }else if(in_ch_count == 6){
      in_channel_mask = AV_CH_LAYOUT_5POINT1;
    }else if(in_ch_count == 4){
      in_channel_mask = AV_CH_LAYOUT_QUAD;
    }else if(in_ch_count == 8){
      in_channel_mask = AV_CH_LAYOUT_7POINT1;
    }else{
      // For unknown channel counts, use a safe default
      if(log_file){
        fprintf(log_file, "ffmpeg_init_audio_resampler: warning: unsupported channel count %d, using stereo\n", in_ch_count);
        fflush(log_file);
      }
      in_channel_mask = AV_CH_LAYOUT_STEREO;
      in_ch_count = 2; // Force to stereo
    }
    #pragma GCC diagnostic pop

    out_channel_mask = (out_channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;

    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: using channel masks: in=0x%llx (%d ch), out=0x%llx (%d ch)\n",
              (unsigned long long)in_channel_mask, in_ch_count,
              (unsigned long long)out_channel_mask, out_channels);
      fflush(log_file);
    }

    ncv->details->swrctx = swr_alloc_set_opts(
      NULL,
      out_channel_mask, AV_SAMPLE_FMT_S16, out_sample_rate,
      in_channel_mask, acodecctx->sample_fmt, acodecctx->sample_rate,
      0, NULL);

    if(!ncv->details->swrctx){
      if(log_file){
        fprintf(log_file, "ffmpeg_init_audio_resampler: swr_alloc_set_opts also failed\n");
        fclose(log_file);
      }
      return -1;
    }

    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: swr_alloc_set_opts succeeded\n");
      fflush(log_file);
    }
  }

  if(!ncv->details->swrctx){
    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: ERROR: swrctx is NULL after allocation!\n");
      fclose(log_file);
    }
    return -1;
  }

  ret = swr_init(ncv->details->swrctx);
  if(ret < 0){
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    if(log_file){
      fprintf(log_file, "ffmpeg_init_audio_resampler: swr_init failed (ret=%d): %s\n", ret, errbuf);
      fclose(log_file);
    }
    swr_free(&ncv->details->swrctx);
    ncv->details->swrctx = NULL;
    return -1;
  }

  if(log_file){
    fprintf(log_file, "ffmpeg_init_audio_resampler: success\n");
    fclose(log_file);
  }

  return 0;
}

// Resample audio frame to output format
// Returns number of output samples, or <0 on error
static int
ffmpeg_resample_audio(ncvisual* ncv, uint8_t** out_data, int* out_linesize,
                      int out_samples, int out_sample_rate, int out_channels){
  if(!ncv->details->swrctx || !ncv->details->audio_frame){
    return -1;
  }

  AVFrame* frame = ncv->details->audio_frame;
  const uint8_t** in_data = (const uint8_t**)frame->extended_data;
  int in_samples = frame->nb_samples;

  // Calculate max output samples needed
  int64_t max_out_samples = swr_get_delay(ncv->details->swrctx, out_sample_rate) + in_samples;
  max_out_samples = av_rescale_rnd(max_out_samples, out_sample_rate,
                                   ncv->details->audiocodecctx->sample_rate, AV_ROUND_UP);

  // Allocate output buffer if needed
  int ret = av_samples_alloc(out_data, out_linesize, out_channels, max_out_samples,
                            AV_SAMPLE_FMT_S16, 0);
  if(ret < 0){
    return -1;
  }

  // Resample
  int out_count = swr_convert(ncv->details->swrctx, out_data, max_out_samples,
                              in_data, in_samples);

  if(out_count < 0){
    av_freep(out_data);
    return -1;
  }

  *out_linesize = av_samples_get_buffer_size(NULL, out_channels, out_count,
                                             AV_SAMPLE_FMT_S16, 1);
  return out_count;
}

// iterate over the decoded frames, calling streamer() with curry for each.
// frames carry a presentation time relative to the beginning, so we get an
// initial timestamp, and check each frame against the elapsed time to sync
// up playback.
static int
ffmpeg_stream(notcurses* nc, ncvisual* ncv, float timescale,
              ncstreamcb streamer, const struct ncvisual_options* vopts,
              void* curry){
  struct timespec begin; // time we started
  clock_gettime(CLOCK_MONOTONIC, &begin);
  uint64_t nsbegin = timespec_to_ns(&begin);
  //bool usets = false;
  // each frame has a duration in units of time_base. keep the aggregate, in case
  // we don't have PTS available.
  uint64_t sum_duration = 0;
  ncplane* newn = NULL;
  struct ncvisual_options activevopts;
  memcpy(&activevopts, vopts, sizeof(*vopts));
  int ncerr;
  do{
    // codecctx seems to be off by a factor of 2 regularly. instead, go with
    // the time_base from the avformatctx. except ts isn't properly reset for
    // all media when we loop =[. we seem to be accurate enough now with the
    // tbase/ppd. see https://github.com/dankamongmen/notcurses/issues/1352.
    double tbase = av_q2d(ncv->details->fmtctx->streams[ncv->details->stream_index]->time_base);
    if(isnan(tbase)){
      tbase = 0;
    }
    if(activevopts.n){
      ncplane_erase(activevopts.n); // new frame could be partially transparent
    }
    // decay the blitter explicitly, so that the callback knows the blitter it
    // was actually rendered with. basically just need rgba_blitter(), but
    // that's not exported.
    ncvgeom geom;
    ncvisual_geom(nc, ncv, &activevopts, &geom);
    activevopts.blitter = geom.blitter;
    if((newn = ncvisual_blit(nc, ncv, &activevopts)) == NULL){
      if(activevopts.n != vopts->n){
        ncplane_destroy(activevopts.n);
      }
      return -1;
    }
    if(activevopts.n != newn){
      activevopts.n = newn;
    }
    // display duration in units of time_base
    const uint64_t pktduration = ffmpeg_pkt_duration(ncv->details->frame);
    uint64_t duration = pktduration * tbase * NANOSECS_IN_SEC;
    double schedns = nsbegin;
    sum_duration += (duration * timescale);
    schedns += sum_duration;
    struct timespec abstime;
    ns_to_timespec(schedns, &abstime);
    int r;
    if(streamer){
      r = streamer(ncv, &activevopts, &abstime, curry);
    }else{
      r = ncvisual_simple_streamer(ncv, &activevopts, &abstime, curry);
    }
    if(r){
      if(activevopts.n != vopts->n){
        ncplane_destroy(activevopts.n);
      }
      return r;
    }
  }while((ncerr = ffmpeg_decode(ncv)) == 0);
  if(activevopts.n != vopts->n){
    ncplane_destroy(activevopts.n);
  }
  if(ncerr == 1){ // 1 indicates reaching EOF
    ncerr = 0;
  }
  return ncerr;
}

static int
ffmpeg_decode_loop(ncvisual* ncv){
  int r = ffmpeg_decode(ncv);
  if(r == 1){
    // Seek all streams (video and audio) back to start
    pthread_mutex_lock(&ncv->details->packet_mutex);
    if(av_seek_frame(ncv->details->fmtctx, -1, 0, AVSEEK_FLAG_BACKWARD) < 0){
      pthread_mutex_unlock(&ncv->details->packet_mutex);
      // FIXME log error
      return -1;
    }
    // Flush codecs
    avcodec_flush_buffers(ncv->details->codecctx);
    if(ncv->details->audiocodecctx){
      avcodec_flush_buffers(ncv->details->audiocodecctx);
    }
    if(ncv->details->subtcodecctx){
      avcodec_flush_buffers(ncv->details->subtcodecctx);
    }
    ncv->details->packet_outstanding = false;
    ncv->details->audio_packet_outstanding = false;
    pthread_mutex_unlock(&ncv->details->packet_mutex);
    if(ffmpeg_decode(ncv) < 0){
      return -1;
    }
  }
  return r;
}

// do a resize *without* updating the ncvisual structure. if the target
// parameters are already matched, the existing data will be returned.
// otherwise, a scaled copy will be returned. they can be differentiated by
// comparing the result against ncv->data.
static uint32_t*
ffmpeg_resize_internal(const ncvisual* ncv, int rows, int* stride, int cols,
                       const blitterargs* bargs){
  const AVFrame* inframe = ncv->details->frame;
//print_frame_summary(NULL, inframe);
  const int targformat = AV_PIX_FMT_RGBA;
//fprintf(stderr, "got format: %d (%d/%d) want format: %d (%d/%d)\n", inframe->format, inframe->height, inframe->width, targformat, rows, cols);
  // FIXME need account for beg{y,x} here, no? what if no inframe?
  if(!inframe || (cols == inframe->width && rows == inframe->height && inframe->format == targformat)){
    // no change necessary. return original data -- we don't duplicate.
    *stride = ncv->rowstride;
    return ncv->data;
  }
  // Validate inframe fields before use
  if(inframe->width <= 0 || inframe->height <= 0 || inframe->format < 0){
    return NULL;
  }
  const int srclenx = bargs->lenx ? bargs->lenx : inframe->width;
  const int srcleny = bargs->leny ? bargs->leny : inframe->height;
  // Validate dimensions
  if(srclenx <= 0 || srcleny <= 0 || cols <= 0 || rows <= 0){
    return NULL;
  }
//fprintf(stderr, "src %d/%d -> targ %d/%d ctx: %p\n", srcleny, srclenx, rows, cols, ncv->details->swsctx);
  // Always pass NULL to sws_getCachedContext to avoid passing potentially corrupted pointers
  // This disables context caching but is safer. sws_getCachedContext will create a new context.
  // If the old context exists and is valid, sws_getCachedContext will free it internally.
  // However, we can't safely detect if a pointer is corrupted, so we always pass NULL.
  ncv->details->swsctx = sws_getCachedContext(NULL,
                                              srclenx, srcleny,
                                              inframe->format,
                                              cols, rows, targformat,
                                              SWS_LANCZOS, NULL, NULL, NULL);
  if(ncv->details->swsctx == NULL){
//fprintf(stderr, "Error retrieving details->swsctx\n");
    return NULL;
  }
  // necessitated by ffmpeg AVPicture API
  uint8_t* dptrs[4];
  int dlinesizes[4];
  int size = av_image_alloc(dptrs, dlinesizes, cols, rows, targformat, IMGALLOCALIGN);
  if(size < 0){
//fprintf(stderr, "Error allocating visual data (%d X %d)\n", sframe->height, sframe->width);
    return NULL;
  }
//fprintf(stderr, "INFRAME DAA: %p SDATA: %p FDATA: %p to %d/%d\n", inframe->data[0], sframe->data[0], ncv->details->frame->data[0], sframe->height, sframe->width);
  const uint8_t* data[4] = { (uint8_t*)ncv->data, };
  int height = sws_scale(ncv->details->swsctx, data,
                         inframe->linesize, 0, srcleny, dptrs, dlinesizes);
  if(height < 0){
//fprintf(stderr, "Error applying scaling (%d X %d)\n", inframe->height, inframe->width);
    av_freep(&dptrs[0]);
    return NULL;
  }
//fprintf(stderr, "scaled %d/%d to %d/%d\n", ncv->pixy, ncv->pixx, rows, cols);
  *stride = dlinesizes[0]; // FIXME check for others?
  return (uint32_t*)dptrs[0];
}

// resize frame, converting to RGBA (if necessary) along the way
static int
ffmpeg_resize(ncvisual* n, unsigned rows, unsigned cols){
  struct blitterargs bargs = {0};
  int stride;
  void* data = ffmpeg_resize_internal(n, rows, &stride, cols, &bargs);
  if(data == n->data){ // no change, return
    return 0;
  }
  if(data == NULL){
    return -1;
  }
  AVFrame* inf = n->details->frame;
//fprintf(stderr, "WHN NCV: %d/%d %p\n", inf->width, inf->height, n->data);
  inf->width = cols;
  inf->height = rows;
  inf->linesize[0] = stride;
  n->rowstride = stride;
  n->pixy = rows;
  n->pixx = cols;
  ncvisual_set_data(n, data, true);
//fprintf(stderr, "SIZE SCALED: %d %d (%u)\n", n->details->frame->height, n->details->frame->width, n->details->frame->linesize[0]);
  return 0;
}

// rows/cols: scaled output geometry (pixels)
static int
ffmpeg_blit(const ncvisual* ncv, unsigned rows, unsigned cols, ncplane* n,
            const struct blitset* bset, const blitterargs* bargs){
  void* data;
  int stride = 0;
  data = ffmpeg_resize_internal(ncv, rows, &stride, cols, bargs);
  if(data == NULL){
    return -1;
  }
//fprintf(stderr, "WHN NCV: bargslen: %d/%d targ: %d/%d\n", bargs->leny, bargs->lenx, rows, cols);
  int ret = 0;
  if(rgba_blit_dispatch(n, bset, stride, data, rows, cols, bargs) < 0){
    ret = -1;
  }
  if(data != ncv->data){
    av_freep(&data); // &dptrs[0]
  }
  return ret;
}

static void
ffmpeg_details_seed(ncvisual* ncv){
  av_frame_unref(ncv->details->frame);
  memset(ncv->details->frame, 0, sizeof(*ncv->details->frame));
  ncv->details->frame->linesize[0] = ncv->rowstride;
  ncv->details->frame->width = ncv->pixx;
  ncv->details->frame->height = ncv->pixy;
  ncv->details->frame->format = AV_PIX_FMT_RGBA;
}

static int
ffmpeg_log_level(int level){
  switch(level){
    case NCLOGLEVEL_SILENT: return AV_LOG_QUIET;
    case NCLOGLEVEL_PANIC: return AV_LOG_PANIC;
    case NCLOGLEVEL_FATAL: return AV_LOG_FATAL;
    case NCLOGLEVEL_ERROR: return AV_LOG_ERROR;
    case NCLOGLEVEL_WARNING: return AV_LOG_WARNING;
    case NCLOGLEVEL_INFO: return AV_LOG_INFO;
    case NCLOGLEVEL_VERBOSE: return AV_LOG_VERBOSE;
    case NCLOGLEVEL_DEBUG: return AV_LOG_DEBUG;
    case NCLOGLEVEL_TRACE: return AV_LOG_TRACE;
    default: break;
  }
  fprintf(stderr, "Invalid log level: %d\n", level);
  return AV_LOG_TRACE;
}

static int
ffmpeg_init(int logl){
  av_log_set_level(ffmpeg_log_level(logl));
  // avdevice_register_all() removed - libavdevice depends on SDL2 which interferes with terminal initialization
  // avdevice is only needed for device I/O, not file playback
  // FIXME could also use av_log_set_callback() and capture the message...
  return 0;
}

static void
ffmpeg_printbanner(fbuf* f){
  fbuf_printf(f, "avformat %u.%u.%u avutil %u.%u.%u swscale %u.%u.%u avcodec %u.%u.%u" NL,
              LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO,
              LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, LIBAVUTIL_VERSION_MICRO,
              LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, LIBSWSCALE_VERSION_MICRO,
              LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
  // avdevice removed - it depends on SDL2
}

static void
ffmpeg_details_destroy(ncvisual_details* deets){
  // avcodec_close() is deprecated; avcodec_free_context() suffices
  avcodec_free_context(&deets->subtcodecctx);
  avcodec_free_context(&deets->audiocodecctx);
  avcodec_free_context(&deets->codecctx);
  av_frame_free(&deets->frame);
  av_frame_free(&deets->audio_frame);
  swr_free(&deets->swrctx);
  sws_freeContext(deets->rgbactx);
  sws_freeContext(deets->swsctx);
  av_packet_free(&deets->packet);
  avformat_close_input(&deets->fmtctx);
  avsubtitle_free(&deets->subtitle);
  pthread_mutex_destroy(&deets->packet_mutex);
  free(deets);
}

static void
ffmpeg_destroy(ncvisual* ncv){
  if(ncv){
    ffmpeg_details_destroy(ncv->details);
    if(ncv->owndata){
      free(ncv->data);
    }
    free(ncv);
  }
}

// Public wrapper functions for audio access from play.cpp
API bool ffmpeg_has_audio(ncvisual* ncv){
  if(!ncv || !ncv->details){
    return false;
  }
  return ncv->details->audio_stream_index >= 0 && ncv->details->audiocodecctx != NULL;
}

AVCodecContext* ffmpeg_get_audio_codec_context(ncvisual* ncv){
  if(!ncv || !ncv->details){
    return NULL;
  }
  return ncv->details->audiocodecctx;
}

int ffmpeg_get_audio_stream_index(ncvisual* ncv){
  if(!ncv || !ncv->details){
    return -1;
  }
  return ncv->details->audio_stream_index;
}

double ffmpeg_get_audio_time_base(ncvisual* ncv){
  if(!ncv || !ncv->details || ncv->details->audio_stream_index < 0 || !ncv->details->fmtctx){
    return 0.0;
  }
  AVStream* ast = ncv->details->fmtctx->streams[ncv->details->audio_stream_index];
  if(!ast){
    return 0.0;
  }
  return av_q2d(ast->time_base);
}

int ffmpeg_init_audio_resampler_public(ncvisual* ncv, int out_sample_rate, int out_channels){
  return ffmpeg_init_audio_resampler(ncv, out_sample_rate, out_channels);
}

int ffmpeg_decode_audio_public(ncvisual* ncv, AVPacket* packet){
  return ffmpeg_decode_audio(ncv, packet);
}

int ffmpeg_resample_audio_public(ncvisual* ncv, uint8_t** out_data, int* out_linesize,
                                  int out_samples, int out_sample_rate, int out_channels){
  return ffmpeg_resample_audio(ncv, out_data, out_linesize, out_samples, out_sample_rate, out_channels);
}

// Get the current decoded audio frame (after calling ffmpeg_decode_audio_public)
API AVFrame* ffmpeg_get_audio_frame(ncvisual* ncv){
  if(!ncv || !ncv->details){
    return NULL;
  }
  return ncv->details->audio_frame;
}

// Read next audio packet from the file (thread-safe)
// Returns: 0 = success, <0 = error, 1 = EOF
// Caller must free the packet with av_packet_free()
int ffmpeg_read_audio_packet(ncvisual* ncv, AVPacket** pkt){
  if(!ncv || !ncv->details || ncv->details->audio_stream_index < 0 || !ncv->details->fmtctx){
    return -1;
  }

  *pkt = av_packet_alloc();
  if(!*pkt){
    return -1;
  }

  pthread_mutex_lock(&ncv->details->packet_mutex);
  int averr;
  do{
    averr = av_read_frame(ncv->details->fmtctx, *pkt);
    if(averr < 0){
      pthread_mutex_unlock(&ncv->details->packet_mutex);
      av_packet_free(pkt);
      if(averr == AVERROR_EOF){
        return 1;
      }
      return averr2ncerr(averr);
    }
    // Skip non-audio packets
    if((*pkt)->stream_index != ncv->details->audio_stream_index){
      av_packet_unref(*pkt);
      continue;
    }
    // Found audio packet
    break;
  }while(1);
  pthread_mutex_unlock(&ncv->details->packet_mutex);
  return 0;
}

// Seek both video and audio streams to beginning (for looping)
API int ffmpeg_seek_to_start(ncvisual* ncv){
  if(!ncv || !ncv->details || !ncv->details->fmtctx){
    return -1;
  }

  pthread_mutex_lock(&ncv->details->packet_mutex);

  int ret = av_seek_frame(ncv->details->fmtctx, -1, 0, AVSEEK_FLAG_BACKWARD);

  if(ret >= 0){
    // Flush codecs
    if(ncv->details->codecctx){
      avcodec_flush_buffers(ncv->details->codecctx);
    }
    if(ncv->details->audiocodecctx){
      avcodec_flush_buffers(ncv->details->audiocodecctx);
    }
    if(ncv->details->subtcodecctx){
      avcodec_flush_buffers(ncv->details->subtcodecctx);
    }

    ncv->details->packet_outstanding = false;
    ncv->details->audio_packet_outstanding = false;
  }

  pthread_mutex_unlock(&ncv->details->packet_mutex);
  return ret;
}

ncvisual_implementation local_visual_implementation = {
  .visual_init = ffmpeg_init,
  .visual_printbanner = ffmpeg_printbanner,
  .visual_blit = ffmpeg_blit,
  .visual_create = ffmpeg_create,
  .visual_from_file = ffmpeg_from_file,
  .visual_details_seed = ffmpeg_details_seed,
  .visual_decode = ffmpeg_decode,
  .visual_decode_loop = ffmpeg_decode_loop,
  .visual_stream = ffmpeg_stream,
  .visual_subtitle = ffmpeg_subtitle,
  .visual_resize = ffmpeg_resize,
  .visual_destroy = ffmpeg_destroy,
  .rowalign = 64, // ffmpeg wants multiples of IMGALIGN (64)
  .canopen_images = true,
  .canopen_videos = true,
};

#endif
