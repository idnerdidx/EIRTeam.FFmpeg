/**************************************************************************/
/*  packed_frame_scaler.h                                                 */
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

#ifndef PACKED_FRAME_SCALER_H
#define PACKED_FRAME_SCALER_H

// Deliberately free of Godot types so tests/packed_frame_scaler_test.cpp can build it
// against libswscale alone.

#include <cstddef>
#include <cstdint>

extern "C" {
#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"
#include "libswscale/swscale.h"
}

// Converts a decoded frame into a tightly packed destination (row stride == width * bytes
// per pixel), the layout Image::create_from_data takes.
//
// sws_scale's SIMD output stores whole pixel blocks, so on a packed destination whose width
// is not a block multiple the LAST row is written past the end of the buffer - measured as
// (ceil16(width) - width) * 4 bytes for yuv420p -> RGBA on AVX2. Into an exact-size heap
// buffer that is heap corruption: a 666-wide video aborts the process with
// "malloc(): corrupted top size". Earlier rows only spill into the next row, which is
// written afterwards, so the end of the buffer is the only unsafe place.
//
// Widths that are a multiple of DIRECT_WIDTH_ALIGN keep the zero-copy path (scale straight
// into the destination); any other width scales into a padded scratch buffer reused across
// frames and copies the rows out.
class PackedFrameScaler {
public:
	// Pixels. Wider than any SIMD block libswscale stores, so an aligned width never overruns.
	static constexpr int DIRECT_WIDTH_ALIGN = 64;

	PackedFrameScaler() = default;
	PackedFrameScaler(const PackedFrameScaler &) = delete;
	PackedFrameScaler &operator=(const PackedFrameScaler &) = delete;
	~PackedFrameScaler();

	static bool scales_directly(int p_width);

	// p_dst must hold exactly width * height * p_bytes_per_pixel bytes. Returns sws_scale's
	// result: the output height, or a negative AVERROR.
	int scale(const AVFrame *p_src, AVPixelFormat p_dst_format, int p_bytes_per_pixel, uint8_t *p_dst);

private:
	SwsContext *sws = nullptr;
	uint8_t *scratch = nullptr;
	size_t scratch_size = 0;
};

#endif // PACKED_FRAME_SCALER_H
