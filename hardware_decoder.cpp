// MIT; same license as EIRTeam.FFmpeg.
#include "hardware_decoder.h"
#include <cstdlib>
#include <cstring>
#include <cstddef>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace eirteam {
namespace {
bool opaque_420(AVPixelFormat format) {
	return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_NV12;
}

AVHWDeviceType platform_device() {
#if defined(_WIN32)
	return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__linux__)
	return AV_HWDEVICE_TYPE_VAAPI;
#else
	return AV_HWDEVICE_TYPE_NONE;
#endif
}

bool software_requested() {
	const char *mode = std::getenv("IDX_FFMPEG_HW_DECODE");
	// Fail closed for typos. No "force hardware" mode that could suppress fallback.
	return mode && std::strcmp(mode, "auto") != 0;
}

const char *device_path(AVHWDeviceType type) {
	if (type != AV_HWDEVICE_TYPE_VAAPI) {
		return nullptr;
	}
	const char *override_path = std::getenv("IDX_FFMPEG_VAAPI_DEVICE");
	if (!override_path) {
		return "/dev/dri/renderD128";
	}
	const char *prefix = "/dev/dri/renderD";
	if (std::strncmp(override_path, prefix, std::strlen(prefix)) != 0) {
		return ""; // invalid path: let creation fail, never reinterpret as a display
	}
	const char *digits = override_path + std::strlen(prefix);
	if (!*digits) {
		return "";
	}
	for (const char *p = digits; *p; ++p) {
		if (*p < '0' || *p > '9') {
			return "";
		}
	}
	return override_path;
}
} // namespace

HardwareDecoder::HardwareDecoder(const char *p_path) : HardwareDecoder(p_path, Operations{}) {}
HardwareDecoder::HardwareDecoder(const char *p_path, Operations p_operations) :
		path(p_path), operations(p_operations), cpu_frame(av_frame_alloc()), planar_frame(av_frame_alloc()) {}

HardwareDecoder::~HardwareDecoder() {
	av_frame_free(&cpu_frame);
	av_frame_free(&planar_frame);
}

bool HardwareDecoder::eligible(const AVCodecParameters *p) {
	if (!p || p->codec_id != AV_CODEC_ID_H264 || !opaque_420((AVPixelFormat)p->format)) {
		return false;
	}
	// No alpha/high-bit-depth/4:2:2/4:4:4 or unknown-profile experimentation.
	return p->profile == AV_PROFILE_H264_BASELINE ||
			p->profile == AV_PROFILE_H264_CONSTRAINED_BASELINE ||
			p->profile == AV_PROFILE_H264_MAIN || p->profile == AV_PROFILE_H264_HIGH;
}

AVPixelFormat HardwareDecoder::choose_format(const AVPixelFormat *offered,
		AVPixelFormat hardware, AVPixelFormat software) {
	if (opaque_420(software) && hardware != AV_PIX_FMT_NONE) {
		for (const AVPixelFormat *p = offered; *p != AV_PIX_FMT_NONE; ++p) {
			if (*p == hardware) {
				return *p;
			}
		}
	}
	// Never return another hardware type just because it is the first offered.
	for (const AVPixelFormat *p = offered; *p != AV_PIX_FMT_NONE; ++p) {
		const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(*p);
		if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
			return *p;
		}
	}
	return AV_PIX_FMT_NONE;
}

AVPixelFormat HardwareDecoder::get_format(AVCodecContext *context, const AVPixelFormat *offered) {
	auto *self = static_cast<HardwareDecoder *>(context->opaque);
	AVPixelFormat selected = choose_format(offered, self->hardware_format, context->sw_pix_fmt);
	if (self->hardware_format != AV_PIX_FMT_NONE && selected != self->hardware_format) {
		self->demote("hardware_format_not_usable");
	}
	return selected;
}

bool HardwareDecoder::hardware_context(const AVCodecContext *context) const {
	return context && context->hw_device_ctx;
}

void HardwareDecoder::report_open(const AVCodec *codec, bool hardware) const {
	const char *device = hardware ? av_hwdevice_get_type_name(platform_device()) : "software";
	av_log(nullptr, AV_LOG_INFO, "[idx-ffmpeg] path=%s mode=%s selected=%s codec=%s reason=%s\n",
			path, software_requested() ? "software" : "auto", device, codec->name, reason);
}

int HardwareDecoder::open(AVCodecContext **context, const AVCodec *codec,
		const AVCodecParameters *parameters, AVRational time_base, int flags) {
	// Free-before-open is explicit: no partially configured codec is reused.
	avcodec_free_context(context);
	hardware_format = AV_PIX_FMT_NONE;
	reported_frame = false;
	active_device.store(-1);
	auto allocate = [&]() {
		*context = avcodec_alloc_context3(codec);
		if (!*context) {
			return AVERROR(ENOMEM);
		}
		int result = avcodec_parameters_to_context(*context, parameters);
		(*context)->pkt_timebase = time_base;
		(*context)->flags |= flags;
		(*context)->thread_count = 0;
		return result;
	};
	int result = allocate();
	if (result < 0) {
		avcodec_free_context(context);
		return result;
	}
	AVHWDeviceType device = platform_device();
	if (software_requested()) {
		reason = "software_requested_or_invalid_mode";
	} else if (software_only) {
		// Retain the original per-stream demotion reason.
	} else if (!eligible(parameters)) {
		reason = "codec_profile_or_format_not_eligible";
	} else if (device == AV_HWDEVICE_TYPE_NONE) {
		reason = "platform_not_supported";
	} else {
		reason = "decoder_has_no_matching_hw_config";
		for (int i = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i); ++i) {
			if (config->device_type != device ||
					!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
				continue;
			}
			AVBufferRef *reference = nullptr;
			result = operations.create_device(&reference, device, device_path(device), nullptr, 0);
			if (result < 0 || !reference) {
				av_buffer_unref(&reference);
				software_only = true;
				reason = "device_unavailable";
				break;
			}
			hardware_format = config->pix_fmt;
			(*context)->hw_device_ctx = reference; // context takes this owned reference
			(*context)->opaque = this;
			(*context)->get_format = get_format;
			(*context)->thread_count = 1; // format callback/state stay on the decode thread
			reason = "device_and_codec_available";
			break;
		}
	}
	const bool attempted_hardware = hardware_context(*context);
	result = operations.open_codec(*context, codec, nullptr);
	if (result < 0 && attempted_hardware) {
		demote("hardware_codec_open_failed");
		avcodec_free_context(context);
		result = allocate();
		if (result >= 0) {
			result = operations.open_codec(*context, codec, nullptr);
		}
	}
	if (result < 0) {
		avcodec_free_context(context);
		return result;
	}
	report_open(codec, hardware_context(*context));
	return 0;
}

void HardwareDecoder::demote(const char *p_reason) {
	software_only = true;
	hardware_format = AV_PIX_FMT_NONE;
	reason = p_reason; // callers pass static, non-sensitive reason codes
	reported_frame = false;
	active_device.store(-1);
	av_log(nullptr, AV_LOG_INFO, "[idx-ffmpeg] path=%s fallback=software reason=%s\n", path, reason);
}

void HardwareDecoder::reset() {
	software_only = false;
	hardware_format = AV_PIX_FMT_NONE;
	reported_frame = false;
	reason = "not_selected";
	active_device.store(-1);
	if (cpu_frame) {
		av_frame_unref(cpu_frame);
	}
	if (planar_frame) {
		av_frame_unref(planar_frame);
	}
}

const char *HardwareDecoder::active_path() const {
	const int device = active_device.load();
	if (device < 0) {
		return "unopened";
	}
	return device == AV_HWDEVICE_TYPE_NONE ? "software" :
			av_hwdevice_get_type_name((AVHWDeviceType)device);
}

int HardwareDecoder::download(AVFrame *frame) {
	const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
	if (!descriptor) {
		return AVERROR(EINVAL);
	}
	const bool hardware = descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL;
	if (hardware) {
		if (!cpu_frame || !frame->hw_frames_ctx) {
			return AVERROR(EINVAL);
		}
		auto *frames = (AVHWFramesContext *)frame->hw_frames_ctx->data;
		if (!opaque_420(frames->sw_format)) {
			return AVERROR(ENOSYS); // do not silently lose alpha or precision
		}
		AVPixelFormat *formats = nullptr;
		int result = operations.transfer_formats(frame->hw_frames_ctx,
				AV_HWFRAME_TRANSFER_DIRECTION_FROM, &formats, 0);
		if (result < 0) {
			av_free(formats);
			return result;
		}
		AVPixelFormat selected = AV_PIX_FMT_NONE;
		for (AVPixelFormat *p = formats; p && *p != AV_PIX_FMT_NONE; ++p) {
			if (opaque_420(*p)) {
				selected = *p;
				break;
			}
		}
		av_free(formats);
		if (selected == AV_PIX_FMT_NONE) {
			return AVERROR(ENOSYS);
		}
		av_frame_unref(cpu_frame);
		cpu_frame->format = selected;
		cpu_frame->width = frame->width;
		cpu_frame->height = frame->height;
		result = operations.transfer(cpu_frame, frame, 0);
		if (result >= 0) {
			result = av_frame_copy_props(cpu_frame, frame);
		}
		if (result < 0) {
			av_frame_unref(cpu_frame);
			return result;
		}
		// NV12->RGB and YUV420P->RGB can use different chroma interpolation in
		// swscale. Normalize by lossless plane deinterleaving, not another color
		// transform, so hardware uses the SAME upload conversion as software.
		if (cpu_frame->format == AV_PIX_FMT_NV12) {
			if (!planar_frame) {
				return AVERROR(ENOMEM);
			}
			av_frame_unref(planar_frame);
			planar_frame->format = AV_PIX_FMT_YUV420P;
			planar_frame->width = cpu_frame->width;
			planar_frame->height = cpu_frame->height;
			result = av_frame_get_buffer(planar_frame, 32);
			if (result >= 0) {
				result = av_frame_copy_props(planar_frame, cpu_frame);
			}
			if (result < 0) {
				return result;
			}
			const int width = cpu_frame->width;
			const int height = cpu_frame->height;
			for (int y = 0; y < height; ++y) {
				std::memcpy(planar_frame->data[0] + (ptrdiff_t)y * planar_frame->linesize[0],
						cpu_frame->data[0] + (ptrdiff_t)y * cpu_frame->linesize[0], width);
			}
			for (int y = 0; y < height / 2 + height % 2; ++y) {
				const uint8_t *uv = cpu_frame->data[1] + (ptrdiff_t)y * cpu_frame->linesize[1];
				uint8_t *u = planar_frame->data[1] + (ptrdiff_t)y * planar_frame->linesize[1];
				uint8_t *v = planar_frame->data[2] + (ptrdiff_t)y * planar_frame->linesize[2];
				for (int x = 0; x < width / 2 + width % 2; ++x) {
					u[x] = uv[2 * x];
					v[x] = uv[2 * x + 1];
				}
			}
			av_frame_unref(cpu_frame);
			av_frame_move_ref(cpu_frame, planar_frame);
		}
		result = av_frame_apply_cropping(cpu_frame, AV_FRAME_CROP_UNALIGNED);
		if (result < 0) {
			return result;
		}
		av_frame_unref(frame);
		av_frame_move_ref(frame, cpu_frame);
	}
	const int active = hardware ? platform_device() : AV_HWDEVICE_TYPE_NONE;
	if (!reported_frame || active_device.load() != active) {
		active_device.store(active);
		av_log(nullptr, AV_LOG_INFO, "[idx-ffmpeg] path=%s active=%s cpu_format=%s\n", path,
				hardware ? av_hwdevice_get_type_name(platform_device()) : "software",
				av_get_pix_fmt_name((AVPixelFormat)frame->format));
		reported_frame = true; // only a successfully downloaded frame confirms hardware
	}
	return 0;
}
} // namespace eirteam
