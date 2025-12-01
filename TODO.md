TODO
====

1. General Allocation Pressure
   - Profiling shows ~60% of CPU time in `av_frame_unref`, `av_buffer_unref`,
	 `av_packet_unref`, and friends.
   - Reusing buffers (where possible) and avoiding per-frame plane creation
	 will reduce allocator churn and improve FPS.

