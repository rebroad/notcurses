TODO
====

1. Subtitle Plane Reuse
   - Current behavior recreates/destroys the subtitle plane every frame.
   - This churn forces libavutil to repeatedly `av_frame_unref`/`av_buffer_unref`,
	 causing heavy CPU usage and low FPS.
   - Cache the subtitle plane, update it only when text/geometry changes.

2. Audio Mutex Contention
   - `ffmpeg_audio_request_packets` / `ffmpeg_get_decoded_audio_frame` spends
	 double-digit CPU percentage inside `pthread_mutex_lock`.
   - Consider coarse-grained locking or condition variables so the audio thread
	 waits instead of busy-spinning on the packet queue.

3. General Allocation Pressure
   - Profiling shows ~60% of CPU time in `av_frame_unref`, `av_buffer_unref`,
	 `av_packet_unref`, and friends.
   - Reusing buffers (where possible) and avoiding per-frame plane creation
	 will reduce allocator churn and improve FPS.

