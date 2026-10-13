/**************************************************************************/
/*  packed_frame_scaler.cpp                                               */
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

#include "packed_frame_scaler.h"

#include <cstring>

extern "C" {
#include "libavutil/error.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
}

PackedFrameScaler::~PackedFrameScaler() {
	sws_freeContext(sws);
	av_free(scratch);
}

bool PackedFrameScaler::scales_directly(int p_width) {
	return p_width % DIRECT_WIDTH_ALIGN == 0;
}

int PackedFrameScaler::scale(const AVFrame *p_src, AVPixelFormat p_dst_format, int p_bytes_per_pixel, uint8_t *p_dst) {
	const int width = p_src->width;
	const int height = p_src->height;
	sws = sws_getCachedContext(sws,
			width, height, (AVPixelFormat)p_src->format,
			width, height, p_dst_format,
			SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (sws == nullptr) {
		return AVERROR(EINVAL);
	}

	const int packed_stride = width * p_bytes_per_pixel;
	if (scales_directly(width)) {
		uint8_t *dst_planes[4] = { p_dst, nullptr, nullptr, nullptr };
		int dst_strides[4] = { packed_stride, 0, 0, 0 };
		return sws_scale(sws, p_src->data, p_src->linesize, 0, height, dst_planes, dst_strides);
	}

	// The stride is padded out to a DIRECT_WIDTH_ALIGN multiple of pixels, so every row -
	// the last included - has room for the block overshoot.
	const int padded_stride = FFALIGN(width, DIRECT_WIDTH_ALIGN) * p_bytes_per_pixel;
	const size_t needed = (size_t)padded_stride * height;
	if (needed > scratch_size) {
		av_free(scratch);
		scratch = (uint8_t *)av_malloc(needed);
		scratch_size = scratch != nullptr ? needed : 0;
		if (scratch == nullptr) {
			return AVERROR(ENOMEM);
		}
	}
	uint8_t *dst_planes[4] = { scratch, nullptr, nullptr, nullptr };
	int dst_strides[4] = { padded_stride, 0, 0, 0 };
	const int result = sws_scale(sws, p_src->data, p_src->linesize, 0, height, dst_planes, dst_strides);
	if (result < 0) {
		return result;
	}
	for (int y = 0; y < height; y++) {
		memcpy(p_dst + (size_t)y * packed_stride, scratch + (size_t)y * padded_stride, packed_stride);
	}
	return result;
}
