// Deterministic libavcodec tests: no Godot, services or physical GPU required.
#include "../hardware_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
int failures = 0;
int device_calls = 0;
int transfer_calls = 0;
bool fail_transfer = false;
int opened_hardware = 0;
int freed_devices = 0;
#define CHECK(condition) do { if (!(condition)) { \
	std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

void mode(const char *value) {
#ifdef _WIN32
	_putenv_s("IDX_FFMPEG_HW_DECODE", value);
#else
	setenv("IDX_FFMPEG_HW_DECODE", value, 1);
#endif
}

int no_device(AVBufferRef **, AVHWDeviceType, const char *, AVDictionary *, int) {
	++device_calls;
	return AVERROR(ENODEV);
}

int fake_device(AVBufferRef **out, AVHWDeviceType type, const char *, AVDictionary *, int) {
	*out = av_hwdevice_ctx_alloc(type);
	if (!*out) {
		return AVERROR(ENOMEM);
	}
	((AVHWDeviceContext *)(*out)->data)->free = [](AVHWDeviceContext *) { ++freed_devices; };
	return 0;
}

int reject_hardware_open(AVCodecContext *context, const AVCodec *codec, AVDictionary **options) {
	if (context->hw_device_ctx) {
		++opened_hardware;
		av_buffer_unref(&context->hw_device_ctx); // even partial-open cleanup must retry software
		return AVERROR(EIO);
	}
	CHECK(context->opaque == nullptr); // fresh software context, no stale callback state
	return avcodec_open2(context, codec, options);
}

int formats(AVBufferRef *, AVHWFrameTransferDirection, AVPixelFormat **out, int) {
	*out = (AVPixelFormat *)av_malloc(2 * sizeof(AVPixelFormat));
	(*out)[0] = AV_PIX_FMT_NV12;
	(*out)[1] = AV_PIX_FMT_NONE;
	return 0;
}

int transfer(AVFrame *out, const AVFrame *, int) {
	++transfer_calls;
	if (fail_transfer) {
		return AVERROR(EIO);
	}
	int result = av_frame_get_buffer(out, 32);
	if (result < 0) {
		return result;
	}
	for (int y = 0; y < out->height; ++y) {
		for (int x = 0; x < out->width; ++x) {
			out->data[0][y * out->linesize[0] + x] = x & 255;
		}
	}
	for (int y = 0; y < (out->height + 1) / 2; ++y) {
		for (int x = 0; x < (out->width + 1) / 2; ++x) {
			out->data[1][y * out->linesize[1] + x * 2] = 40 + x;
			out->data[1][y * out->linesize[1] + x * 2 + 1] = 100 + x;
		}
	}
	return 0;
}

void selection() {
	AVCodecParameters *parameters = avcodec_parameters_alloc();
	parameters->codec_type = AVMEDIA_TYPE_VIDEO;
	parameters->codec_id = AV_CODEC_ID_H264;
	parameters->format = AV_PIX_FMT_YUV420P;
	parameters->profile = AV_PROFILE_H264_HIGH;
	parameters->width = 128;
	parameters->height = 72;
	CHECK(eirteam::HardwareDecoder::eligible(parameters));
	for (AVCodecID codec : {AV_CODEC_ID_PRORES, AV_CODEC_ID_CFHD, AV_CODEC_ID_HEVC, AV_CODEC_ID_VP9}) {
		parameters->codec_id = codec;
		CHECK(!eirteam::HardwareDecoder::eligible(parameters));
	}
	parameters->codec_id = AV_CODEC_ID_H264;
	for (AVPixelFormat format : {AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV444P, AV_PIX_FMT_NONE}) {
		parameters->format = format;
		CHECK(!eirteam::HardwareDecoder::eligible(parameters));
	}
	parameters->format = AV_PIX_FMT_YUV420P;
	parameters->profile = AV_PROFILE_UNKNOWN;
	CHECK(!eirteam::HardwareDecoder::eligible(parameters));
	parameters->profile = AV_PROFILE_H264_HIGH;
	AVPixelFormat offered[] = {AV_PIX_FMT_CUDA, AV_PIX_FMT_VAAPI, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
	CHECK(eirteam::HardwareDecoder::choose_format(offered, AV_PIX_FMT_VAAPI, AV_PIX_FMT_YUV420P) == AV_PIX_FMT_VAAPI);
	CHECK(eirteam::HardwareDecoder::choose_format(offered, AV_PIX_FMT_D3D11, AV_PIX_FMT_YUV420P) == AV_PIX_FMT_YUV420P);
	CHECK(eirteam::HardwareDecoder::choose_format(offered, AV_PIX_FMT_VAAPI, AV_PIX_FMT_YUV420P10LE) == AV_PIX_FMT_YUV420P);
	AVPixelFormat hardware_only[] = {AV_PIX_FMT_CUDA, AV_PIX_FMT_NONE};
	CHECK(eirteam::HardwareDecoder::choose_format(hardware_only, AV_PIX_FMT_VAAPI, AV_PIX_FMT_YUV420P) == AV_PIX_FMT_NONE);

	eirteam::HardwareDecoder::Operations operations;
	operations.create_device = no_device;
	eirteam::HardwareDecoder decoder("unit", operations);
	AVCodecContext *context = nullptr;
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	mode("software");
	CHECK(decoder.open(&context, codec, parameters, AVRational{1, 90000}) == 0);
	CHECK(!context->hw_device_ctx && device_calls == 0);
	mode("auto");
	CHECK(decoder.open(&context, codec, parameters, AVRational{1, 90000}) == 0);
	CHECK(!context->hw_device_ctx && device_calls == 1);
	// Device failure and explicit runtime demotion are per-stream sticky.
	CHECK(decoder.open(&context, codec, parameters, AVRational{1, 90000}) == 0);
	CHECK(device_calls == 1);
	decoder.demote("unit_decode_failure");
	CHECK(decoder.open(&context, codec, parameters, AVRational{1, 90000}) == 0);
	CHECK(device_calls == 1);
	avcodec_free_context(&context);
	decoder.reset();
	CHECK(decoder.open(&context, codec, parameters, AVRational{1, 90000}) == 0);
	CHECK(device_calls == 2);
	avcodec_free_context(&context);
	operations.create_device = fake_device;
	operations.open_codec = reject_hardware_open;
	eirteam::HardwareDecoder open_failure("unit", operations);
	CHECK(open_failure.open(&context, codec, parameters, AVRational{1, 90000}) == 0);
	CHECK(opened_hardware == 1 && freed_devices == 1 && !context->hw_device_ctx);
	avcodec_free_context(&context);
	avcodec_parameters_free(&parameters);
}

void download() {
	eirteam::HardwareDecoder::Operations operations;
	operations.transfer = transfer;
	operations.transfer_formats = formats;
	eirteam::HardwareDecoder decoder("unit", operations);
	AVFrame *frame = av_frame_alloc();
	frame->format = AV_PIX_FMT_VAAPI;
	frame->width = 128;
	frame->height = 72;
	frame->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
	frame->buf[0] = av_buffer_alloc(1); // synthetic opaque surface, never a real GPU claim
	frame->data[0] = frame->buf[0]->data;
	auto *frames = (AVHWFramesContext *)frame->hw_frames_ctx->data;
	frames->sw_format = AV_PIX_FMT_NV12;
	frame->pts = 90000;
	frame->best_effort_timestamp = 89999;
	frame->duration = 3000;
	frame->colorspace = AVCOL_SPC_BT709;
	frame->color_range = AVCOL_RANGE_MPEG;
	frame->crop_left = 2;
	AVFrame *saved = av_frame_clone(frame);
	CHECK(decoder.download(frame) == 0);
	CHECK(frame->format == AV_PIX_FMT_YUV420P && frame->data[0] && frame->buf[0]);
	CHECK(frame->pts == 90000 && frame->best_effort_timestamp == 89999 && frame->duration == 3000);
	CHECK(frame->colorspace == AVCOL_SPC_BT709 && frame->color_range == AVCOL_RANGE_MPEG);
	CHECK(frame->crop_left == 0 && frame->width == 126); // crop applied to owned CPU pixels
	CHECK(frame->data[0][0] == 2 && frame->data[1][0] == 41 && frame->data[2][0] == 101);
	CHECK(std::strcmp(decoder.active_path(), "software") != 0);
	// The consumer's downloaded buffer survives the next download (no aliasing).
	frame->data[0][0] = 42;
	CHECK(decoder.download(saved) == 0);
	CHECK(frame->data[0][0] == 42 && frame->data[0] != saved->data[0]);
	av_frame_unref(saved);
	saved->format = AV_PIX_FMT_VAAPI;
	saved->width = 65;
	saved->height = 33;
	saved->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
	((AVHWFramesContext *)saved->hw_frames_ctx->data)->sw_format = AV_PIX_FMT_YUVA420P;
	int before = transfer_calls;
	CHECK(decoder.download(saved) < 0 && transfer_calls == before); // alpha cannot reach NV12
	((AVHWFramesContext *)saved->hw_frames_ctx->data)->sw_format = AV_PIX_FMT_NV12;
	fail_transfer = true;
	CHECK(decoder.download(saved) < 0 && saved->format == AV_PIX_FMT_VAAPI);
	fail_transfer = false;
	CHECK(decoder.download(saved) == 0 && saved->width == 65 && saved->height == 33);
	av_frame_free(&frame);
	av_frame_free(&saved);
}
}

int main() {
	selection();
	download();
	std::printf("%s hardware selection, fresh software fallback, transfer properties and ownership\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
