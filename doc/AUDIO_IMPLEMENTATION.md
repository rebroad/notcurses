# Audio Support Implementation Plan for ncplayer

## Overview
To add audio playback to ncplayer, we need to:
1. Detect and decode audio streams alongside video
2. Output audio to the system sound device
3. Synchronize audio and video playback
4. Handle audio in a separate thread

## Key Files to Modify

### 1. `src/media/ffmpeg.c` - Core Media Handling
- Extend `ncvisual_details` structure to include audio codec context
- Add audio stream detection in `ffmpeg_from_file()`
- Modify `ffmpeg_decode()` to handle audio packets
- Implement audio decoding loop

### 2. `src/player/play.cpp` - Player Logic
- Add audio playback thread
- Implement A/V synchronization
- Handle audio initialization/shutdown
- Add command-line options for audio (enable/disable, volume)

### 3. New File: `src/media/audio-output.c` (or similar)
- Audio output abstraction layer
- Choose between ALSA/PulseAudio/SDL2/libao
- Audio buffer management
- Format conversion (resampling)

### 4. `CMakeLists.txt` - Build Configuration
- Add audio library dependencies (pkg_check_modules)
- Link audio libraries to notcurses target

## Architecture Changes Needed

### Current Structure:
```
ncvisual_details {
  - Video codec context
  - Subtitle codec context
  - Video frame
  - Stream indices
}
```

### Proposed Structure:
```
ncvisual_details {
  - Video codec context
  - Audio codec context  [NEW]
  - Subtitle codec context
  - Video frame
  - Audio frame          [NEW]
  - SwrContext          [NEW] (for audio resampling)
  - Stream indices (including audio_stream_index)
}
```

## Implementation Steps

### Phase 1: Audio Stream Detection
1. In `ffmpeg_from_file()`, add `av_find_best_stream()` for AVMEDIA_TYPE_AUDIO
2. Allocate audio codec context (similar to subtitle handling)
3. Store audio stream index

### Phase 2: Audio Decoding
1. Modify `ffmpeg_decode()` to handle audio packets (currently skips them at line 340)
2. Create separate audio decoding function
3. Use libswresample to convert to standard format (PCM S16LE, 44.1kHz stereo)

### Phase 3: Audio Output
1. Choose audio library (SDL2 recommended for cross-platform)
2. Initialize audio device
3. Create audio callback/thread
4. Implement ring buffer for audio samples

### Phase 4: Synchronization
1. Track audio clock vs video clock
2. Adjust playback speed or skip/drop frames
3. Handle pause/resume for both streams
4. Sync on seek operations

### Phase 5: Integration
1. Start audio thread when playback begins
2. Stop audio thread when playback ends
3. Handle errors gracefully
4. Add command-line flags (-a/--audio, --no-audio)

## Dependencies Needed

### Required Libraries:
- `libswresample` (part of FFmpeg, likely already available)
- Audio output library:
  - **SDL2** (recommended: cross-platform, well-tested)
  - **libao** (lightweight abstraction layer)
  - **ALSA** (Linux-specific)
  - **PulseAudio** (Linux-specific)

### CMake Configuration:
```cmake
if(${USE_FFMPEG})
  pkg_check_modules(SWRESAMPLE REQUIRED libswresample>=4.0)
  # Choose one:
  pkg_check_modules(SDL2 REQUIRED sdl2>=2.0)
  # OR
  pkg_check_modules(AO REQUIRED ao>=1.0)
  # OR
  pkg_check_modules(ALSA REQUIRED alsa>=1.0)
endif()
```

## Useful Reference Projects

### 1. FFmpeg Examples
- `ffmpeg/doc/examples/`: Official FFmpeg examples
- `decoding_encoding.c`: Shows audio/video decoding
- `audio_decode_example.c`: Audio-specific example

### 2. mpv
- Full-featured media player
- Excellent A/V sync implementation
- Open source: https://github.com/mpv-player/mpv
- Key files: `audio/out/`, `audio/decode/`, `player/av_common.c`

### 3. VLC
- Cross-platform media player
- Multiple audio output backends
- Open source: https://github.com/videolan/vlc
- Key files: `modules/audio_output/`

### 4. Simple Terminal Players
- **termvideo** (Go): https://github.com/levkush/termvideo
- **terminal-video-player-py** (Python): Simpler implementation to study

### 5. SDL Audio Examples
- SDL2 documentation: https://wiki.libsdl.org/SDL2/CategoryAudio
- Official examples in SDL2 source

## Key Code Patterns to Look For

### Audio Thread Pattern:
```c
// Pseudocode structure
void* audio_thread(void* arg) {
  while (playing) {
    // Get decoded audio frame
    // Convert format if needed
    // Write to audio device buffer
    // Update audio clock
  }
}
```

### A/V Sync Pattern:
```c
// Pseudocode
double video_clock = get_video_pts();
double audio_clock = get_audio_pts();
double diff = video_clock - audio_clock;

if (diff > 0.1) {
  // Video ahead - slow down video or speed up audio
} else if (diff < -0.1) {
  // Audio ahead - speed up video or slow down audio
}
```

### Packet Demultiplexing:
```c
// Currently in ffmpeg_decode() - need to handle audio too
while (av_read_frame(fmtctx, packet) >= 0) {
  if (packet->stream_index == video_stream_index) {
    // Handle video packet
  } else if (packet->stream_index == audio_stream_index) {
    // Handle audio packet [NEW]
  }
  av_packet_unref(packet);
}
```

## Challenges to Consider

1. **Threading Complexity**: Audio needs continuous playback while video can pause
2. **Buffer Management**: Audio needs low-latency, continuous buffer
3. **Seeking**: Both streams must seek together
4. **Error Handling**: What if audio fails but video works?
5. **Platform Differences**: Different audio APIs on Linux/macOS/Windows
6. **Terminal Compatibility**: Some terminals might conflict with audio

## Testing Strategy

1. Test with videos that have audio
2. Test with videos that have no audio
3. Test with audio-only files (if supported)
4. Test seeking while audio is playing
5. Test pause/resume
6. Test with different audio formats (especially AC3)
7. Test on different platforms

## Incremental Approach

1. **Step 1**: Add audio stream detection (no playback yet)
2. **Step 2**: Decode audio to buffers (verify with debug output)
3. **Step 3**: Add minimal audio output (simple, no sync)
4. **Step 4**: Add threading
5. **Step 5**: Add synchronization
6. **Step 6**: Polish and error handling

