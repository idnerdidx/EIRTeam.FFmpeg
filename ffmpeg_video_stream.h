/**************************************************************************/
/*  ffmpeg_video_stream.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             EIRTeam.FFmpeg                             */
/*                         https://ph.eirteam.moe                         */
/**************************************************************************/
/* Copyright (c) 2023-present Álex Román (EIRTeam) & contributors.        */
/*                                                                        */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef FFMPEG_VIDEO_STREAM_H
#define FFMPEG_VIDEO_STREAM_H

#include "video_decoder.h"

#ifdef GDEXTENSION

// Headers for building as GDExtension plug-in.
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/video_stream.hpp>
#include <godot_cpp/classes/video_stream_playback.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

#else

#include "core/object/ref_counted.h"
#include "scene/resources/atlas_texture.h"
#include "scene/resources/texture_rd.h"
#include "scene/resources/video_stream.h"

#endif

class YUVGPUConverter : public RefCounted {
	RID shader;
	Ref<Image> yuv_plane_images[4];
	RID yuv_plane_textures[4];
	RID yuv_planes_uniform_sets[4];
	RID pipeline;
	Ref<Texture2DRD> out_texture;
	RID out_uniform_set;
	Vector2i frame_size;

	struct PushConstant {
		uint32_t use_alpha;
		uint8_t padding[11];
	} push_constant;

private:
	void _ensure_pipeline();
	Error _ensure_plane_textures();
	Error _ensure_output_texture();
	RID _create_uniform_set(const RID &p_texture_rd_rid, int p_shader_set);
	void _upload_plane_images();
	void _clear_texture_internal();
	void _convert_internal();

public:
	void set_plane_image(int p_plane_idx, Ref<Image> p_image);
	Vector2i get_frame_size() const;
	void set_frame_size(const Vector2i &p_frame_size);
	void convert();
	Ref<Texture2D> get_output_texture() const;
	void clear_output_texture();

	YUVGPUConverter();
	~YUVGPUConverter();
};

// We have to use this function redirection system for GDExtension because the naming conventions
// for the functions we are supposed to override are different there
#include "gdextension_build/func_redirect.h"
class FFmpegVideoStreamPlayback : public VideoStreamPlayback {
	GDCLASS(FFmpegVideoStreamPlayback, VideoStreamPlayback);

	const int LENIENCE_BEFORE_SEEK = 2500;

	// Wall-clock presentation lock.
	// playback_position used to be a per-machine accumulator (playback_position += p_delta), so four
	// displays playing the same file drifted apart, and the only correction GDScript can make
	// (stream_position) is a decoder seek i.e. a frame skip or hold - MEASURED, the correction was
	// itself the jitter, and no band setting beat leaving it alone. Instead derive the shared epoch
	// from the wall-clock-derived seek the scheduler already issues at clip start:
	//     anchor = now - target   reduces to the itinerary's own play time,
	// which is identical on every machine (their system clocks agree within ~3ms). Position then
	// becomes a pure function of the shared clock: every display selects the same frame with no
	// corrections at all, and a machine that stalls CATCHES UP instead of lagging permanently.
	// Clock RATE recovery, not position jumps.
	// Every position-based correction tried on this wall made sync WORSE, because writing
	// playback_position changes which frame is selected and disturbs the decode pipeline - the
	// correction was itself the jitter, and a jump also re-freezes a fresh error. Instead trim the
	// RATE of the playback clock by a fraction of a percent so the error decays smoothly over a few
	// seconds, never skipping or holding a frame. This is standard clock recovery (genlock/broadcast)
	// and it CONVERGES regardless of when a display started - which is the whole point: staggered and
	// simultaneous restarts must end up identical.
	//   err (ms) = shared-schedule target - playback_position   (+ve = this display is behind)
	//   rate     = 1 + TRIM_GAIN*err, clamped to +/-MAX_TRIM
	// TRIM_GAIN 0.0012 => a 1-frame (33ms) error asks for 4% rate; MAX_TRIM caps at 5%, so 40ms
	// converges in <1s. Tightened from 0.0003/2% after telemetry showed one display (the 12GB box)
	// tracking 20-40ms behind because the loop corrected slower than it accumulated jitter. 5% rate is
	// imperceptible on video; revisit if these clips ever carry audio. Gross desync still hard-seeks.
	static constexpr double CLOCK_TRIM_GAIN = 0.0012;
	static constexpr double CLOCK_MAX_TRIM = 0.05;
	bool wall_locked = false;
	double wall_anchor_unix_ms = 0.0;
	static double now_unix_ms();
	// PER-STREAM opt-in for wall-clock sync, set from FFmpegVideoStream::wall_clock_sync.
	// Must be per-stream, not per-process: wall sync makes the clip's position a function of a
	// shared clock AND takes loop wraps from that clock, so applying it to one-shot content
	// (trigger stings, non-looping overlays) would wrap them at duration instead of letting
	// them end - `finished` would never fire and the caller's cleanup would never run.
	// It also must never be inferred from a seek: `looping` cannot serve as the signal because
	// it is never assigned anywhere in this repo (a constant false).
	// Only meaningful for looping content played at 1x.
	bool wall_sync_opt_in = false;
	// Separate from the opt-in on purpose. Wall sync (slaving position to the shared clock via
	// rate trim) is correct for ANY content, one-shot included - it just keeps the screens
	// together. Taking the LOOP WRAP from that clock is only valid for content that actually
	// loops: applied to a one-shot clip it seeks back to 0 at duration instead of ending, so
	// `finished` never fires and the caller never gets to clean up (hide the overlay, clear its
	// playing flag). Off unless the embedder says the stream loops.
	bool wall_loop_opt_in = false;

	// Per-instance telemetry state. These were function-local statics, so three simultaneous
	// playbacks shared ONE 10s window and the lines carried no instance id - two instances
	// logging different anchors was indistinguishable from a real desync while diagnosing.
	int sync_log_id = 0;
	double last_sync_log_ms = 0.0;
	double last_unlocked_log_ms = 0.0;
	double playback_position = 0.0f;

	Ref<VideoDecoder> decoder;
	List<Ref<DecodedFrame>> available_frames;
	List<Ref<DecodedAudioFrame>> available_audio_frames;
	Ref<DecodedFrame> last_frame;
#ifndef FFMPEG_MT_GPU_UPLOAD
	Ref<ImageTexture> last_frame_texture;
#endif
	Ref<Image> last_frame_image;
	Ref<ImageTexture> texture;
	Ref<Texture2DRD> yuv_texture;
	bool looping = false;
	bool buffering = false;
	int frames_processed = 0;
	void seek_into_sync();
	double get_current_frame_time();
	bool check_next_frame_valid(Ref<DecodedFrame> p_decoded_frame);
	bool check_next_audio_frame_valid(Ref<DecodedAudioFrame> p_decoded_frame);
	bool paused = false;
	bool playing = false;
	bool just_seeked = false;

	Ref<YUVGPUConverter> yuv_converter;

private:
	bool is_paused_internal() const;
	void update_internal(double p_delta);
	bool is_playing_internal() const;
	void set_paused_internal(bool p_paused);
	void play_internal();
	void stop_internal();
	void seek_internal(double p_time);
	double get_length_internal() const;
	Ref<Texture2D> get_texture_internal() const;
	double get_playback_position_internal() const;
	int get_mix_rate_internal() const;
	int get_channels_internal() const;

protected:
	void clear();
	static void _bind_methods(){}; // Required by GDExtension, do not remove

public:
	Error load(Ref<FileAccess> p_file_access);

	STREAM_FUNC_REDIRECT_0_CONST(bool, is_paused);
	STREAM_FUNC_REDIRECT_1(void, update, double, p_delta);
	STREAM_FUNC_REDIRECT_0_CONST(bool, is_playing);
	STREAM_FUNC_REDIRECT_1(void, set_paused, bool, p_paused);
	STREAM_FUNC_REDIRECT_0(void, play);
	STREAM_FUNC_REDIRECT_0(void, stop);
	STREAM_FUNC_REDIRECT_1(void, seek, double, p_time);
	STREAM_FUNC_REDIRECT_0_CONST(double, get_length);
	STREAM_FUNC_REDIRECT_0_CONST(Ref<Texture2D>, get_texture);
	STREAM_FUNC_REDIRECT_0_CONST(double, get_playback_position);
	STREAM_FUNC_REDIRECT_0_CONST(int, get_mix_rate);
	STREAM_FUNC_REDIRECT_0_CONST(int, get_channels);
	FFmpegVideoStreamPlayback();
	void set_wall_clock_sync(bool p_enabled) { wall_sync_opt_in = p_enabled; }
	void set_wall_clock_loop(bool p_enabled) { wall_loop_opt_in = p_enabled; }
};

class FFmpegVideoStream : public VideoStream {
	GDCLASS(FFmpegVideoStream, VideoStream);

	bool wall_clock_sync = false;
	bool wall_clock_loop = false;

protected:
	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("set_wall_clock_sync", "enabled"), &FFmpegVideoStream::set_wall_clock_sync);
		ClassDB::bind_method(D_METHOD("is_wall_clock_sync"), &FFmpegVideoStream::is_wall_clock_sync);
		ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wall_clock_sync"), "set_wall_clock_sync", "is_wall_clock_sync");
		ClassDB::bind_method(D_METHOD("set_wall_clock_loop", "enabled"), &FFmpegVideoStream::set_wall_clock_loop);
		ClassDB::bind_method(D_METHOD("is_wall_clock_loop"), &FFmpegVideoStream::is_wall_clock_loop);
		ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wall_clock_loop"), "set_wall_clock_loop", "is_wall_clock_loop");
	}
	Ref<VideoStreamPlayback> instantiate_playback_internal() {
		Ref<FileAccess> fa = FileAccess::open(get_file(), FileAccess::READ);
		if (!fa.is_valid()) {
			return Ref<VideoStreamPlayback>();
		}
		Ref<FFmpegVideoStreamPlayback> pb;
		pb.instantiate();
		if (pb->load(fa) != OK) {
			return nullptr;
		}
		pb->set_wall_clock_sync(wall_clock_sync);
		pb->set_wall_clock_loop(wall_clock_loop);
		return pb;
	}

public:
	void set_wall_clock_sync(bool p_enabled) { wall_clock_sync = p_enabled; }
	bool is_wall_clock_sync() const { return wall_clock_sync; }
	void set_wall_clock_loop(bool p_enabled) { wall_clock_loop = p_enabled; }
	bool is_wall_clock_loop() const { return wall_clock_loop; }
	STREAM_FUNC_REDIRECT_0(Ref<VideoStreamPlayback>, instantiate_playback);
};

#endif // FFMPEG_VIDEO_STREAM_H
