#include "incoming-media-decoder.hpp"
#include "obs-log.hpp"

#include "participant-source.hpp"

#include <obs.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QDebug>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace nextcloud_talk {
namespace {

constexpr int outputAudioRate = 48000;

QString codecName(IncomingCodec codec)
{
	switch (codec) {
	case IncomingCodec::H264:
		return QStringLiteral("H.264");
	case IncomingCodec::Vp8:
		return QStringLiteral("VP8");
	case IncomingCodec::Opus:
		return QStringLiteral("Opus");
	case IncomingCodec::Unsupported:
		return QStringLiteral("unsupported");
	}
	return QStringLiteral("unknown");
}

AVCodecID codecId(IncomingCodec codec)
{
	switch (codec) {
	case IncomingCodec::H264:
		return AV_CODEC_ID_H264;
	case IncomingCodec::Vp8:
		return AV_CODEC_ID_VP8;
	case IncomingCodec::Opus:
		return AV_CODEC_ID_OPUS;
	case IncomingCodec::Unsupported:
		return AV_CODEC_ID_NONE;
	}
	return AV_CODEC_ID_NONE;
}

QString ffmpegError(int result)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(result, text, sizeof(text));
	return QString::fromUtf8(text);
}

} // namespace

class IncomingMediaDecoder::Impl {
public:
	~Impl() { reset(); }

	bool decode(IncomingCodec codec, const std::uint8_t *data, std::size_t size,
		    const QString &participantId, bool resetBeforeDecode, std::uint64_t videoTimestampNs,
		    bool screenShare)
	{
		if (!data || size == 0 || participantId.isEmpty() || codec == IncomingCodec::Unsupported)
			return false;
		std::lock_guard<std::mutex> lock(mutex_);
		if (codec != codec_ && !open(codec))
			return false;
		if (resetBeforeDecode)
			avcodec_flush_buffers(context_);

		AVPacket *packet = av_packet_alloc();
		if (!packet)
			return false;
		const int allocated = av_new_packet(packet, static_cast<int>(size));
		if (allocated < 0) {
			av_packet_free(&packet);
			return false;
		}
		std::memcpy(packet->data, data, size);
		const int sent = avcodec_send_packet(context_, packet);
		av_packet_free(&packet);
		if (sent < 0 && sent != AVERROR(EAGAIN)) {
			const unsigned int failures = rejectedPackets_.fetch_add(1) + 1;
			if (failures == 1 || failures % 120 == 0)
				ObsLogLine(LOG_WARNING) << "[nextcloud-talk]" << codecName(codec_)
						     << "decoder rejected a packet:" << ffmpegError(sent)
						     << "count=" << failures;
			return false;
		}

		// A video packet can be accepted without immediately producing a visible
		// frame (VP8 uses invisible reference frames). That is still successful
		// decoder continuity and must not trigger a flush/keyframe request.
		while (true) {
			const int received = avcodec_receive_frame(context_, decoded_);
			if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
				break;
			if (received < 0) {
				ObsLogLine(LOG_WARNING) << "[nextcloud-talk]" << codecName(codec_)
						     << "decoder failed:" << ffmpegError(received);
				return false;
			}
			if (codec_ == IncomingCodec::Opus)
				publishAudio(participantId, decoded_);
			else
				publishVideo(participantId, decoded_, videoTimestampNs, screenShare);
			av_frame_unref(decoded_);
		}
		return true;
	}

private:
	bool open(IncomingCodec codec)
	{
		reset();
		const AVCodecID id = codecId(codec);
		const AVCodec *decoder = avcodec_find_decoder(id);
		if (!decoder) {
			ObsLogLine(LOG_WARNING) << "[nextcloud-talk] No FFmpeg decoder for" << codecName(codec);
			return false;
		}
		context_ = avcodec_alloc_context3(decoder);
		decoded_ = av_frame_alloc();
		if (!context_ || !decoded_) {
			ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Could not allocate" << codecName(codec)
					     << "decoder state";
			reset();
			return false;
		}
		context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
		context_->thread_count = 1;
		const int result = avcodec_open2(context_, decoder, nullptr);
		if (result < 0) {
			ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Could not open" << codecName(codec)
					     << "decoder:" << ffmpegError(result);
			reset();
			return false;
		}
		codec_ = codec;
		return true;
	}

	void reset()
	{
		if (scale_)
			sws_freeContext(scale_);
		scale_ = nullptr;
		if (resampler_)
			swr_free(&resampler_);
		if (convertedVideo_)
			av_frame_free(&convertedVideo_);
		if (decoded_)
			av_frame_free(&decoded_);
		if (context_)
			avcodec_free_context(&context_);
		av_channel_layout_uninit(&resamplerInputLayout_);
		convertedWidth_ = 0;
		convertedHeight_ = 0;
		resamplerInputFormat_ = AV_SAMPLE_FMT_NONE;
		resamplerInputRate_ = 0;
		audioNextTimestampNs_ = 0;
		codec_ = IncomingCodec::Unsupported;
		rejectedPackets_ = 0;
	}

	bool prepareVideoConversion(const AVFrame *input)
	{
		if (convertedVideo_ && convertedWidth_ == input->width && convertedHeight_ == input->height) {
			scale_ = sws_getCachedContext(scale_, input->width, input->height,
						      static_cast<AVPixelFormat>(input->format), input->width,
						      input->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
						      nullptr, nullptr);
			return scale_ != nullptr;
		}

		if (convertedVideo_)
			av_frame_free(&convertedVideo_);
		convertedVideo_ = av_frame_alloc();
		if (!convertedVideo_)
			return false;
		convertedVideo_->format = AV_PIX_FMT_BGRA;
		convertedVideo_->width = input->width;
		convertedVideo_->height = input->height;
		if (av_frame_get_buffer(convertedVideo_, 32) < 0)
			return false;
		convertedWidth_ = input->width;
		convertedHeight_ = input->height;
		scale_ = sws_getCachedContext(scale_, input->width, input->height,
					      static_cast<AVPixelFormat>(input->format), input->width,
					      input->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
					      nullptr);
		return scale_ != nullptr;
	}

	void publishVideo(const QString &participantId, const AVFrame *input, std::uint64_t timestampNs,
			  bool screenShare)
	{
		if (!prepareVideoConversion(input) || av_frame_make_writable(convertedVideo_) < 0)
			return;
		sws_scale(scale_, input->data, input->linesize, 0, input->height, convertedVideo_->data,
			  convertedVideo_->linesize);

		VideoFrame frame;
		frame.planes[0] = convertedVideo_->data[0];
		frame.linesize[0] = static_cast<std::uint32_t>(convertedVideo_->linesize[0]);
		frame.width = static_cast<std::uint32_t>(input->width);
		frame.height = static_cast<std::uint32_t>(input->height);
		frame.timestampNs = timestampNs != 0 ? timestampNs : os_gettime_ns();
		frame.obsFormat = VIDEO_FORMAT_BGRA;
		publishParticipantVideo(participantId, frame, screenShare);
		const std::uint64_t decodedFrames = ++decodedVideoFrames_;
		if (!reportedVideo_.exchange(true))
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Decoded first" << codecName(codec_)
					  << (screenShare ? "screen frame for" : "video frame for")
					  << participantId << input->width << "x"
					  << input->height;
		else if (decodedFrames % 100 == 0)
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Decoded" << decodedFrames << codecName(codec_)
					  << "video frames for" << participantId;
	}

	bool prepareAudioResampler(const AVFrame *input)
	{
		const AVSampleFormat format = static_cast<AVSampleFormat>(input->format);
		const bool layoutMatches = resamplerInputLayout_.nb_channels != 0 &&
					   av_channel_layout_compare(&resamplerInputLayout_, &input->ch_layout) == 0;
		if (resampler_ && resamplerInputFormat_ == format && resamplerInputRate_ == input->sample_rate &&
		    layoutMatches)
			return true;

		if (resampler_)
			swr_free(&resampler_);
		av_channel_layout_uninit(&resamplerInputLayout_);
		const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		const int allocated = swr_alloc_set_opts2(&resampler_, &stereo, AV_SAMPLE_FMT_FLTP,
						  outputAudioRate, &input->ch_layout, format,
						  input->sample_rate, 0, nullptr);
		if (allocated < 0 || !resampler_ || swr_init(resampler_) < 0) {
			if (resampler_)
				swr_free(&resampler_);
			return false;
		}
		av_channel_layout_copy(&resamplerInputLayout_, &input->ch_layout);
		resamplerInputFormat_ = format;
		resamplerInputRate_ = input->sample_rate;
		return true;
	}

	void publishAudio(const QString &participantId, const AVFrame *input)
	{
		if (!prepareAudioResampler(input))
			return;
		const int capacity = static_cast<int>(av_rescale_rnd(
			swr_get_delay(resampler_, input->sample_rate) + input->nb_samples, outputAudioRate,
			input->sample_rate, AV_ROUND_UP));
		AVFrame *output = av_frame_alloc();
		if (!output)
			return;
		output->format = AV_SAMPLE_FMT_FLTP;
		output->sample_rate = outputAudioRate;
		output->nb_samples = capacity;
		const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		av_channel_layout_copy(&output->ch_layout, &stereo);
		if (av_frame_get_buffer(output, 0) < 0) {
			av_frame_free(&output);
			return;
		}
		const int frames = swr_convert(resampler_, output->data, capacity,
					       const_cast<const std::uint8_t **>(input->extended_data),
					       input->nb_samples);
		if (frames <= 0) {
			av_frame_free(&output);
			return;
		}

		AudioFrame frame;
		frame.planes[0] = output->data[0];
		frame.planes[1] = output->data[1];
		frame.frames = static_cast<std::uint32_t>(frames);
		frame.sampleRate = outputAudioRate;
		const std::uint64_t duration = static_cast<std::uint64_t>(frames) * 1'000'000'000ULL /
					       outputAudioRate;
		const std::uint64_t now = os_gettime_ns();
		// Network packets do not arrive at perfectly even intervals. Feeding their
		// arrival time straight into OBS creates overlaps/gaps that sound metallic
		// or distorted. Keep a sample-accurate playout clock and only resynchronise
		// after a large discontinuity.
		constexpr std::uint64_t maximumClockDriftNs = 250'000'000ULL;
		if (audioNextTimestampNs_ == 0 || audioNextTimestampNs_ + maximumClockDriftNs < now ||
		    audioNextTimestampNs_ > now + maximumClockDriftNs)
			audioNextTimestampNs_ = now > duration ? now - duration : now;
		frame.timestampNs = audioNextTimestampNs_;
		audioNextTimestampNs_ += duration;
		frame.obsFormat = AUDIO_FORMAT_FLOAT_PLANAR;
		frame.obsSpeakers = SPEAKERS_STEREO;
		publishParticipantAudio(participantId, frame);
		av_frame_free(&output);
		const std::uint64_t decodedFrames = ++decodedAudioFrames_;
		if (!reportedAudio_.exchange(true))
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Decoded first Opus audio frame for"
					  << participantId;
		else if (decodedFrames % 500 == 0)
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Decoded" << decodedFrames
					  << "continuous Opus audio frames for" << participantId;
	}

	std::mutex mutex_;
	IncomingCodec codec_ = IncomingCodec::Unsupported;
	AVCodecContext *context_ = nullptr;
	AVFrame *decoded_ = nullptr;
	AVFrame *convertedVideo_ = nullptr;
	SwsContext *scale_ = nullptr;
	SwrContext *resampler_ = nullptr;
	AVChannelLayout resamplerInputLayout_ = {};
	AVSampleFormat resamplerInputFormat_ = AV_SAMPLE_FMT_NONE;
	int resamplerInputRate_ = 0;
	std::uint64_t audioNextTimestampNs_ = 0;
	int convertedWidth_ = 0;
	int convertedHeight_ = 0;
	std::atomic_bool reportedVideo_{false};
	std::atomic_bool reportedAudio_{false};
	std::atomic_uint rejectedPackets_{0};
	std::atomic_uint64_t decodedVideoFrames_{0};
	std::atomic_uint64_t decodedAudioFrames_{0};
};

IncomingMediaDecoder::IncomingMediaDecoder() : impl_(std::make_unique<Impl>()) {}
IncomingMediaDecoder::~IncomingMediaDecoder() = default;

bool IncomingMediaDecoder::decode(IncomingCodec codec, const std::uint8_t *data, std::size_t size,
			  const QString &participantId, bool resetBeforeDecode,
			  std::uint64_t videoTimestampNs, bool screenShare)
{
	return impl_->decode(codec, data, size, participantId, resetBeforeDecode, videoTimestampNs,
			     screenShare);
}

} // namespace nextcloud_talk
