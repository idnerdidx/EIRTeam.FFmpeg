// MIT; same license as EIRTeam.FFmpeg. No engine/service dependencies.
#ifndef EIRTEAM_HARDWARE_DECODER_H
#define EIRTEAM_HARDWARE_DECODER_H
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace eirteam {

// One instance per video decoder, confined to its decode thread after open.
// The caller owns/frees the returned codec context BEFORE destroying this object:
// get_format's opaque pointer refers to this instance. CPU frames returned by
// download() own their buffers and retain all timing/color/crop properties.
class HardwareDecoder {
public:
	struct Operations {
		int (*create_device)(AVBufferRef **, AVHWDeviceType, const char *, AVDictionary *, int) = av_hwdevice_ctx_create;
		int (*transfer)(AVFrame *, const AVFrame *, int) = av_hwframe_transfer_data;
		int (*transfer_formats)(AVBufferRef *, AVHWFrameTransferDirection, AVPixelFormat **, int) = av_hwframe_transfer_get_formats;
		int (*open_codec)(AVCodecContext *, const AVCodec *, AVDictionary **) = avcodec_open2;
	};

	explicit HardwareDecoder(const char *path);
	HardwareDecoder(const char *path, Operations operations);
	~HardwareDecoder();
	HardwareDecoder(const HardwareDecoder &) = delete;
	HardwareDecoder &operator=(const HardwareDecoder &) = delete;

	// All failed hardware setup/open attempts retry with a FRESH software context.
	int open(AVCodecContext **context, const AVCodec *codec,
			const AVCodecParameters *parameters, AVRational time_base, int flags = 0);
	void demote(const char *reason); // sticky for this stream; no process-wide disable
	void reset(); // only for a genuinely new stream, after freeing its old context
	int download(AVFrame *frame); // in-place hardware -> owned CPU frame, or negative error
	static bool eligible(const AVCodecParameters *parameters);
	static AVPixelFormat choose_format(const AVPixelFormat *offered, AVPixelFormat hardware,
			AVPixelFormat software);
	bool hardware_context(const AVCodecContext *context) const;
	const char *active_path() const; // thread-safe; "unopened" until a usable frame

private:
	const char *path;
	Operations operations;
	AVPixelFormat hardware_format = AV_PIX_FMT_NONE;
	AVFrame *cpu_frame = nullptr;
	AVFrame *planar_frame = nullptr;
	bool software_only = false;
	bool reported_frame = false;
	const char *reason = "not_selected";
	std::atomic<int> active_device{-1};
	static AVPixelFormat get_format(AVCodecContext *context, const AVPixelFormat *offered);
	void report_open(const AVCodec *codec, bool hardware) const;
};
} // namespace eirteam
#endif
