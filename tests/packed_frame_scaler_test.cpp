// PackedFrameScaler regression test - needs only libswscale + libavutil, no Godot.
// Build and run with tests/run_packed_frame_scaler_test.sh.
//
// Every case scales into an EXACT width*height*4 heap buffer followed by a guard band and
// fails if a single guard byte changed - the overrun that corrupted the heap and aborted
// playback of a 666-wide video. Widths that are a DIRECT_WIDTH_ALIGN multiple take the
// zero-copy path, so their cases also check that alignment against the linked libswscale.
// Each case also compares the packed rows with a reference scaled into a padded buffer.

#include "../packed_frame_scaler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "libavutil/cpu.h"
#include "libavutil/pixdesc.h"
}

namespace {

constexpr int BYTES_PER_PIXEL = 4;
constexpr size_t GUARD = 512;
constexpr unsigned char GUARD_BYTE = 0xAA;

AVFrame *make_source(AVPixelFormat p_format, int p_width, int p_height) {
	AVFrame *frame = av_frame_alloc();
	frame->format = p_format;
	frame->width = p_width;
	frame->height = p_height;
	if (av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		return nullptr;
	}
	// A gradient rather than a flat fill, so a row copied from the wrong offset shows.
	for (int plane = 0; plane < 4 && frame->buf[plane] != nullptr; plane++) {
		uint8_t *data = frame->buf[plane]->data;
		for (size_t i = 0; i < frame->buf[plane]->size; i++) {
			data[i] = (uint8_t)((i * 7 + plane * 31) & 0xFF);
		}
	}
	return frame;
}

// The expected packed rows: scaled into a buffer with room to spare, then cut down.
bool reference_rows(const AVFrame *p_src, std::vector<uint8_t> &r_rows) {
	const int width = p_src->width;
	const int height = p_src->height;
	SwsContext *sws = sws_getContext(width, height, (AVPixelFormat)p_src->format,
			width, height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (sws == nullptr) {
		return false;
	}
	const int packed = width * BYTES_PER_PIXEL;
	const int roomy = packed + 1024;
	std::vector<uint8_t> roomy_buf((size_t)roomy * (height + 1));
	uint8_t *dst[4] = { roomy_buf.data(), nullptr, nullptr, nullptr };
	int strides[4] = { roomy, 0, 0, 0 };
	const int result = sws_scale(sws, p_src->data, p_src->linesize, 0, height, dst, strides);
	sws_freeContext(sws);
	if (result != height) {
		return false;
	}
	r_rows.resize((size_t)packed * height);
	for (int y = 0; y < height; y++) {
		memcpy(r_rows.data() + (size_t)y * packed, roomy_buf.data() + (size_t)y * roomy, packed);
	}
	return true;
}

int run_case(PackedFrameScaler &p_scaler, AVPixelFormat p_format, int p_width, int p_height) {
	const char *name = av_get_pix_fmt_name(p_format);
	AVFrame *src = make_source(p_format, p_width, p_height);
	std::vector<uint8_t> expected;
	if (src == nullptr || !reference_rows(src, expected)) {
		printf("SKIP %-14s %5dx%-4d  libswscale cannot convert this format\n", name, p_width, p_height);
		av_frame_free(&src);
		return 0;
	}

	const size_t packed_size = (size_t)p_width * p_height * BYTES_PER_PIXEL;
	// malloc'd like the decoder's pooled buffers, so an overrun lands on heap metadata.
	uint8_t *dst = (uint8_t *)malloc(packed_size + GUARD);
	memset(dst, GUARD_BYTE, packed_size + GUARD);
	const int result = p_scaler.scale(src, AV_PIX_FMT_RGBA, BYTES_PER_PIXEL, dst);

	size_t overrun = 0;
	for (size_t i = 0; i < GUARD; i++) {
		if (dst[packed_size + i] != GUARD_BYTE) {
			overrun = i + 1;
		}
	}
	const bool pixels_match = result == p_height && memcmp(dst, expected.data(), packed_size) == 0;
	const bool ok = overrun == 0 && pixels_match;
	printf("%s %-14s %5dx%-4d  %-6s  overrun=%zu bytes  pixels=%s\n", ok ? "PASS" : "FAIL", name,
			p_width, p_height, PackedFrameScaler::scales_directly(p_width) ? "direct" : "padded", overrun,
			pixels_match ? "match" : "MISMATCH");

	free(dst);
	av_frame_free(&src);
	return ok ? 0 : 1;
}

} // namespace

int main() {
	printf("libswscale %u, cpu flags 0x%x\n", swscale_version(), av_get_cpu_flags());

	const AVPixelFormat formats[] = {
		AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
		AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUVA444P12LE, AV_PIX_FMT_GBRAP12LE,
		AV_PIX_FMT_BGRA, AV_PIX_FMT_RGBA,
	};
	// 666x800 is the video that crashed; the rest cover remainders a block store could
	// overshoot by, plus the aligned widths that keep the direct path. Heights are EVEN:
	// libswscale's SIMD yuv->RGB converter only runs on even heights, so an odd-height case
	// never reaches the overrun (one stays, for odd chroma rounding).
	const int sizes[][2] = {
		{ 666, 800 }, { 665, 38 }, { 667, 38 }, { 670, 38 }, { 664, 38 }, { 672, 38 }, { 665, 37 },
		{ 1, 2 }, { 63, 10 }, { 65, 10 }, { 1278, 10 }, { 1280, 10 }, { 1920, 10 }, { 2048, 10 },
	};

	int failures = 0;
	for (AVPixelFormat format : formats) {
		// One scaler per format, reused across sizes like the decoder's: covers the cached
		// sws context and the scratch buffer being resized between frames.
		PackedFrameScaler scaler;
		for (const auto &size : sizes) {
			failures += run_case(scaler, format, size[0], size[1]);
		}
	}
	printf("%s: %d failing case(s)\n", failures == 0 ? "OK" : "FAILED", failures);
	return failures == 0 ? 0 : 1;
}
