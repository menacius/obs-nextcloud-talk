#include "outgoing-media.hpp"
#include "obs-log.hpp"

#include <obs-frontend-api.h>
#include <obs.h>
#include <media-io/audio-io.h>
#include <media-io/audio-resampler.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <QDebug>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace nextcloud_talk {
namespace {

constexpr int audioSampleRate = 48000;
constexpr int audioChannels = 2;
constexpr int audioFrameSamples = 960;
constexpr int videoFps = 30;
constexpr int maximumVideoWidth = 1280;
constexpr int maximumVideoHeight = 720;
constexpr const char *captureOutputId = "nextcloud_talk_capture_output";

struct CaptureTarget {
	void *parameter = nullptr;
	void (*callback)(void *, video_data *) = nullptr;
};

struct CaptureOutputContext {
	obs_output_t *output = nullptr;
	CaptureTarget *target = nullptr;
};

const char *captureOutputName(void *)
{
	return "Nextcloud Talk selected-camera capture";
}

void *captureOutputCreate(obs_data_t *settings, obs_output_t *output)
{
	auto *context = new CaptureOutputContext;
	context->output = output;
	context->target = reinterpret_cast<CaptureTarget *>(
		static_cast<std::uintptr_t>(obs_data_get_int(settings, "capture_target")));
	return context;
}

void captureOutputDestroy(void *data)
{
	delete static_cast<CaptureOutputContext *>(data);
}

bool captureOutputStart(void *data)
{
	auto *context = static_cast<CaptureOutputContext *>(data);
	return context && context->output && context->target && context->target->callback &&
	       obs_output_begin_data_capture(context->output, 0);
}

void captureOutputStop(void *data, std::uint64_t)
{
	auto *context = static_cast<CaptureOutputContext *>(data);
	if (context && context->output)
		obs_output_end_data_capture(context->output);
}

void captureOutputVideo(void *data, video_data *frame)
{
	auto *context = static_cast<CaptureOutputContext *>(data);
	if (context && context->target && context->target->callback)
		context->target->callback(context->target->parameter, frame);
}

QString ffmpegError(int result)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(result, text, sizeof(text));
	return QString::fromUtf8(text);
}

uint32_t evenDimension(uint32_t value)
{
	return std::max(2u, value & ~1u);
}

bool isVideoCaptureDevice(const char *id)
{
	if (!id)
		return false;
	const std::string value(id);
	return value == "dshow_input" || value == "v4l2_input" || value == "av_capture_input" ||
	       value == "macos-avcapture" || value == "macos-avcapture-fast";
}

bool isAudioInputCapture(const char *id)
{
	if (!id)
		return false;
	const std::string value(id);
	return value == "wasapi_input_capture" || value == "coreaudio_input_capture" ||
	       value == "pulse_input_capture" || value == "alsa_input_capture";
}

void fitVideoSize(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t &width, uint32_t &height)
{
	width = sourceWidth;
	height = sourceHeight;
	if (width <= maximumVideoWidth && height <= maximumVideoHeight)
		return;

	const double scale = std::min(static_cast<double>(maximumVideoWidth) / width,
				      static_cast<double>(maximumVideoHeight) / height);
	width = evenDimension(static_cast<uint32_t>(width * scale));
	height = evenDimension(static_cast<uint32_t>(height * scale));
}

} // namespace

QString outgoingMediaModeId(OutgoingMediaMode mode)
{
	switch (mode) {
	case OutgoingMediaMode::Devices:
		return QStringLiteral("devices");
	case OutgoingMediaMode::Scene:
		return QStringLiteral("scene");
	case OutgoingMediaMode::Preview:
		return QStringLiteral("preview");
	case OutgoingMediaMode::Program:
		return QStringLiteral("program");
	}
	return QStringLiteral("devices");
}

OutgoingMediaMode outgoingMediaModeFromId(const QString &id)
{
	if (id == QStringLiteral("scene"))
		return OutgoingMediaMode::Scene;
	if (id == QStringLiteral("preview"))
		return OutgoingMediaMode::Preview;
	if (id == QStringLiteral("program"))
		return OutgoingMediaMode::Program;
	return OutgoingMediaMode::Devices;
}

obs_source_t *resolveOutgoingVideoSource(const OutgoingMediaSelection &selection)
{
	switch (selection.mode) {
	case OutgoingMediaMode::Devices:
		return obs_get_source_by_name(selection.videoSourceName.toUtf8().constData());
	case OutgoingMediaMode::Scene:
		return obs_get_source_by_name(selection.sceneName.toUtf8().constData());
	case OutgoingMediaMode::Preview: {
		obs_source_t *source = obs_frontend_get_current_preview_scene();
		return source ? source : obs_frontend_get_current_scene();
	}
	case OutgoingMediaMode::Program:
		return obs_frontend_get_current_scene();
	}
	return nullptr;
}

class OutgoingMedia::Impl {
public:
	explicit Impl(OutgoingMedia *owner) : owner_(owner)
	{
		captureTarget_.parameter = this;
		captureTarget_.callback = videoCallback;
	}
	~Impl() { stop(); }

	bool start(const OutgoingMediaSelection &selection, QString &error)
	{
		stop();
		selection_ = selection;

		videoSource_ = resolveOutgoingVideoSource(selection_);
		if (!videoSource_) {
			error = QStringLiteral("The selected outgoing video source is not available.");
			stop();
			return false;
		}
		if (selection_.mode == OutgoingMediaMode::Devices) {
			audioSource_ = obs_get_source_by_name(selection_.audioSourceName.toUtf8().constData());
			if (!audioSource_) {
				error = QStringLiteral("The selected OBS audio capture source no longer exists.");
				stop();
				return false;
			}
			if (!isVideoCaptureDevice(obs_source_get_unversioned_id(videoSource_)) ||
			    !isAudioInputCapture(obs_source_get_unversioned_id(audioSource_))) {
				error = QStringLiteral(
					"Device mode requires an OBS Video Capture Device and Audio Input Capture source.");
				stop();
				return false;
			}
		} else if (obs_source_get_type(videoSource_) != OBS_SOURCE_TYPE_SCENE) {
			error = QStringLiteral("The selected outgoing scene is not available.");
			stop();
			return false;
		}

		obs_video_info mainVideo = {};
		if (!obs_get_video_info(&mainVideo)) {
			error = QStringLiteral("OBS video is not initialized.");
			stop();
			return false;
		}
		const uint32_t sourceWidth = evenDimension(
			obs_source_get_width(videoSource_) ? obs_source_get_width(videoSource_) : mainVideo.base_width);
		const uint32_t sourceHeight = evenDimension(
			obs_source_get_height(videoSource_) ? obs_source_get_height(videoSource_) : mainVideo.base_height);
		fitVideoSize(sourceWidth, sourceHeight, videoWidth_, videoHeight_);

		if (!openVideoEncoder(error) || !openAudioEncoder(error)) {
			stop();
			return false;
		}

		obs_audio_info audioInfo = {};
		if (!obs_get_audio_info(&audioInfo)) {
			error = QStringLiteral("OBS audio is not initialized.");
			stop();
			return false;
		}
		resample_info from = {};
		from.samples_per_sec = audioInfo.samples_per_sec;
		from.format = AUDIO_FORMAT_FLOAT_PLANAR;
		from.speakers = audioInfo.speakers;
		resample_info to = {};
		to.samples_per_sec = audioSampleRate;
		to.format = AUDIO_FORMAT_FLOAT;
		to.speakers = SPEAKERS_STEREO;
		audioResampler_ = audio_resampler_create(&to, &from);
		if (!audioResampler_) {
			error = QStringLiteral("Could not create the microphone resampler.");
			stop();
			return false;
		}

		// Mirror OBS Virtual Camera's Source Output path: a private scene wraps
		// the selected source before the auxiliary view renders it. This keeps
		// asynchronous device sources active and renderable outside Program.
		videoScene_ = obs_scene_create_private("nextcloud-talk-outgoing-video");
		if (!videoScene_) {
			error = QStringLiteral("Could not create the private camera scene.");
			stop();
			return false;
		}
		videoItem_ = obs_scene_add(videoScene_, videoSource_);
		if (!videoItem_) {
			error = QStringLiteral("Could not add the selected video to the private scene.");
			stop();
			return false;
		}
		obs_sceneitem_set_bounds_type(videoItem_, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(videoItem_, OBS_ALIGN_CENTER);
		const vec2 bounds{static_cast<float>(sourceWidth), static_cast<float>(sourceHeight)};
		obs_sceneitem_set_bounds(videoItem_, &bounds);

		view_ = obs_view_create();
		if (!view_) {
			error = QStringLiteral("Could not create a private OBS camera view.");
			stop();
			return false;
		}
		obs_view_set_source(view_, 0, obs_scene_get_source(videoScene_));
		obs_video_info privateVideo = mainVideo;
		privateVideo.fps_num = videoFps;
		privateVideo.fps_den = 1;
		privateVideo.base_width = sourceWidth;
		privateVideo.base_height = sourceHeight;
		privateVideo.output_width = videoWidth_;
		privateVideo.output_height = videoHeight_;
		privateVideo.output_format = VIDEO_FORMAT_BGRA;
		// A CPU-backed auxiliary mix is more broadly supported than OBS's
		// optional NV12/P010 GPU conversion path and still converts to I420 at
		// the video-output callback boundary.
		privateVideo.gpu_conversion = false;
		privateVideo.scale_type = OBS_SCALE_BICUBIC;
		videoOutput_ = obs_view_add2(view_, &privateVideo);
		if (!videoOutput_) {
			error = QStringLiteral("Could not activate the private OBS camera view.");
			stop();
			return false;
		}

		video_scale_info conversion = {};
		conversion.format = VIDEO_FORMAT_BGRA;
		conversion.width = videoWidth_;
		conversion.height = videoHeight_;
		conversion.colorspace = privateVideo.colorspace;
		conversion.range = privateVideo.range;
		obs_data_t *captureSettings = obs_data_create();
		obs_data_set_int(captureSettings, "capture_target",
				 static_cast<long long>(reinterpret_cast<std::uintptr_t>(&captureTarget_)));
		captureOutput_ = obs_output_create(captureOutputId, "nextcloud-talk-camera-output",
					  captureSettings, nullptr);
		obs_data_release(captureSettings);
		if (!captureOutput_) {
			error = QStringLiteral("Could not create the private OBS camera output.");
			stop();
			return false;
		}
		obs_output_set_video_conversion(captureOutput_, &conversion);
		obs_output_set_media(captureOutput_, videoOutput_, nullptr);
		active_ = true;
		if (!obs_output_start(captureOutput_)) {
			error = QStringLiteral("Could not start the selected camera output.");
			stop();
			return false;
		}

		if (selection_.mode == OutgoingMediaMode::Devices) {
			obs_source_add_audio_capture_callback(audioSource_, audioCallback, this);
			audioConnected_ = true;
		} else {
			audioOutput_ = obs_get_audio();
			audio_convert_info audioConversion = {};
			audioConversion.samples_per_sec = audioInfo.samples_per_sec;
			audioConversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
			audioConversion.speakers = audioInfo.speakers;
			if (!audioOutput_ ||
			    !audio_output_connect(audioOutput_, 0, &audioConversion, mixedAudioCallback, this)) {
				error = QStringLiteral("Could not capture the selected scene audio mix.");
				stop();
				return false;
			}
			mixedAudioConnected_ = true;
		}
		refreshProgramAudioFlag(videoSource_);
		if (selection_.mode != OutgoingMediaMode::Devices) {
			obs_frontend_add_event_callback(frontendEvent, this);
			frontendCallbackConnected_ = true;
		}
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Outgoing capture started:"
				  << outgoingMediaModeId(selection_.mode) << obs_source_get_name(videoSource_)
				  << QStringLiteral("%1x%2 VP8").arg(videoWidth_).arg(videoHeight_)
				  << (audioSource_ ? obs_source_get_name(audioSource_) : "selected scene mix")
				  << "48 kHz stereo Opus";
		return true;
	}

	void stop()
	{
		active_ = false;
		if (frontendCallbackConnected_)
			obs_frontend_remove_event_callback(frontendEvent, this);
		frontendCallbackConnected_ = false;
		if (audioConnected_ && audioSource_)
			obs_source_remove_audio_capture_callback(audioSource_, audioCallback, this);
		audioConnected_ = false;
		if (mixedAudioConnected_ && audioOutput_)
			audio_output_disconnect(audioOutput_, 0, mixedAudioCallback, this);
		mixedAudioConnected_ = false;
		audioOutput_ = nullptr;
		if (captureOutput_) {
			obs_output_stop(captureOutput_);
			obs_output_release(captureOutput_);
			captureOutput_ = nullptr;
		}
		videoOutput_ = nullptr;
		if (view_) {
			obs_view_remove(view_);
			obs_view_set_source(view_, 0, nullptr);
			obs_view_destroy(view_);
			view_ = nullptr;
		}
		if (videoScene_) {
			videoItem_ = nullptr;
			obs_scene_release(videoScene_);
			videoScene_ = nullptr;
		}
		if (audioResampler_) {
			audio_resampler_destroy(audioResampler_);
			audioResampler_ = nullptr;
		}
		if (videoFrame_)
			av_frame_free(&videoFrame_);
		if (audioFrame_)
			av_frame_free(&audioFrame_);
		if (videoCodec_)
			avcodec_free_context(&videoCodec_);
		if (videoScaler_)
			sws_freeContext(videoScaler_);
		videoScaler_ = nullptr;
		if (audioCodec_)
			avcodec_free_context(&audioCodec_);
		if (videoSource_) {
			obs_source_release(videoSource_);
			videoSource_ = nullptr;
		}
		if (audioSource_) {
			obs_source_release(audioSource_);
			audioSource_ = nullptr;
		}
		audioBuffer_.clear();
		videoPts_ = 0;
		audioPts_ = 0;
	}

	bool active() const { return active_; }

private:
	static void frontendEvent(enum obs_frontend_event event, void *parameter)
	{
		if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED && event != OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED)
			return;
		static_cast<Impl *>(parameter)->refreshFrontendSource();
	}

	static void videoCallback(void *parameter, video_data *frame)
	{
		static_cast<Impl *>(parameter)->encodeVideo(frame);
	}

	static void audioCallback(void *parameter, obs_source_t *, const audio_data *data, bool muted)
	{
		static_cast<Impl *>(parameter)->encodeAudio(data, muted);
	}

	static void mixedAudioCallback(void *parameter, size_t, audio_data *clockData)
	{
		static_cast<Impl *>(parameter)->encodeSelectedSceneAudio(clockData);
	}

	void refreshFrontendSource()
	{
		obs_source_t *source = resolveOutgoingVideoSource(selection_);
		if (!source)
			return;
		refreshProgramAudioFlag(source);
		{
			std::lock_guard<std::mutex> lock(sourceMutex_);
			if (source == videoSource_) {
				obs_source_release(source);
				return;
			}
			if (videoItem_)
				obs_sceneitem_remove(videoItem_);
			videoItem_ = obs_scene_add(videoScene_, source);
			if (videoItem_) {
				obs_sceneitem_set_bounds_type(videoItem_, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds_alignment(videoItem_, OBS_ALIGN_CENTER);
				const vec2 bounds{static_cast<float>(std::max(1u, obs_source_get_width(source))),
						  static_cast<float>(std::max(1u, obs_source_get_height(source)))};
				obs_sceneitem_set_bounds(videoItem_, &bounds);
			}
			obs_source_release(videoSource_);
			videoSource_ = source;
		}
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Outgoing frontend source changed to"
				  << obs_source_get_name(source);
	}

	void encodeSelectedSceneAudio(const audio_data *clockData)
	{
		if (audioFromProgramMix_) {
			encodeAudio(clockData, false);
			return;
		}
		obs_source_t *source = nullptr;
		{
			std::lock_guard<std::mutex> lock(sourceMutex_);
			if (videoSource_)
				source = obs_source_get_ref(videoSource_);
		}
		if (!source)
			return;
		obs_source_audio_mix mix = {};
		obs_source_get_audio_mix(source, &mix);
		audio_data selected = {};
		selected.frames = std::min<uint32_t>(clockData->frames, AUDIO_OUTPUT_FRAMES);
		selected.timestamp = clockData->timestamp;
		for (size_t channel = 0; channel < MAX_AV_PLANES; ++channel)
			selected.data[channel] = reinterpret_cast<uint8_t *>(mix.output[0].data[channel]);
		encodeAudio(&selected, false);
		obs_source_release(source);
	}

	void refreshProgramAudioFlag(obs_source_t *selectedSource)
	{
		if (selection_.mode == OutgoingMediaMode::Program) {
			audioFromProgramMix_ = true;
			return;
		}
		obs_source_t *program = obs_frontend_get_current_scene();
		audioFromProgramMix_ = program && program == selectedSource;
		if (program)
			obs_source_release(program);
	}

	bool openVideoEncoder(QString &error)
	{
		const AVCodec *codec = avcodec_find_encoder_by_name("libvpx");
		if (!codec)
			codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
		if (!codec) {
			error = QStringLiteral("No VP8 encoder is available in the OBS FFmpeg runtime.");
			return false;
		}
		videoCodec_ = avcodec_alloc_context3(codec);
		if (!videoCodec_) {
			error = QStringLiteral("Could not allocate the H.264 encoder.");
			return false;
		}
		videoCodec_->width = static_cast<int>(videoWidth_);
		videoCodec_->height = static_cast<int>(videoHeight_);
		videoCodec_->pix_fmt = AV_PIX_FMT_YUV420P;
		videoCodec_->time_base = AVRational{1, videoFps};
		videoCodec_->framerate = AVRational{videoFps, 1};
		videoCodec_->bit_rate = 2'000'000;
		videoCodec_->gop_size = videoFps * 2;
		videoCodec_->max_b_frames = 0;
		videoCodec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
		av_opt_set(videoCodec_->priv_data, "deadline", "realtime", 0);
		av_opt_set(videoCodec_->priv_data, "cpu-used", "8", 0);
		av_opt_set(videoCodec_->priv_data, "lag-in-frames", "0", 0);
		av_opt_set(videoCodec_->priv_data, "auto-alt-ref", "0", 0);
		av_opt_set(videoCodec_->priv_data, "error-resilient", "1", 0);
		const int opened = avcodec_open2(videoCodec_, codec, nullptr);
		if (opened < 0) {
			error = QStringLiteral("Could not open the VP8 encoder: %1").arg(ffmpegError(opened));
			return false;
		}
		videoFrame_ = av_frame_alloc();
		if (!videoFrame_) {
			error = QStringLiteral("Could not allocate a VP8 video frame.");
			return false;
		}
		videoFrame_->format = videoCodec_->pix_fmt;
		videoFrame_->width = videoCodec_->width;
		videoFrame_->height = videoCodec_->height;
		const int allocated = av_frame_get_buffer(videoFrame_, 32);
		if (allocated < 0) {
			error = QStringLiteral("Could not allocate VP8 frame buffers: %1").arg(ffmpegError(allocated));
			return false;
		}
		return true;
	}

	bool openAudioEncoder(QString &error)
	{
		const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
		if (!codec)
			codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
		if (!codec) {
			error = QStringLiteral("No Opus encoder is available in the OBS FFmpeg runtime.");
			return false;
		}
		audioCodec_ = avcodec_alloc_context3(codec);
		if (!audioCodec_) {
			error = QStringLiteral("Could not allocate the Opus encoder.");
			return false;
		}
		bool supportsFloat = false;
#if LIBAVCODEC_VERSION_MAJOR >= 61
		const void *formats = nullptr;
		int formatCount = 0;
		const int queried = avcodec_get_supported_config(audioCodec_, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
							 0, &formats, &formatCount);
		supportsFloat = queried >= 0 && !formats;
		if (formats) {
			const auto *sampleFormats = static_cast<const AVSampleFormat *>(formats);
			for (int index = 0; index < formatCount; ++index)
				supportsFloat = supportsFloat || sampleFormats[index] == AV_SAMPLE_FMT_FLT;
		}
#else
		if (!codec->sample_fmts) {
			supportsFloat = true;
		} else {
			for (const AVSampleFormat *format = codec->sample_fmts; *format != AV_SAMPLE_FMT_NONE; ++format)
				supportsFloat = supportsFloat || *format == AV_SAMPLE_FMT_FLT;
		}
#endif
		if (!supportsFloat) {
			error = QStringLiteral("The available Opus encoder does not accept float PCM.");
			return false;
		}
		audioCodec_->sample_fmt = AV_SAMPLE_FMT_FLT;
		audioCodec_->sample_rate = audioSampleRate;
		audioCodec_->time_base = AVRational{1, audioSampleRate};
		audioCodec_->bit_rate = 64'000;
		const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		av_channel_layout_copy(&audioCodec_->ch_layout, &stereo);
		const int opened = avcodec_open2(audioCodec_, codec, nullptr);
		if (opened < 0) {
			error = QStringLiteral("Could not open the Opus encoder: %1").arg(ffmpegError(opened));
			return false;
		}
		audioFrame_ = av_frame_alloc();
		if (!audioFrame_) {
			error = QStringLiteral("Could not allocate an Opus audio frame.");
			return false;
		}
		audioFrame_->format = audioCodec_->sample_fmt;
		audioFrame_->sample_rate = audioCodec_->sample_rate;
		audioFrame_->nb_samples = audioFrameSamples;
		av_channel_layout_copy(&audioFrame_->ch_layout, &audioCodec_->ch_layout);
		const int allocated = av_frame_get_buffer(audioFrame_, 0);
		if (allocated < 0) {
			error = QStringLiteral("Could not allocate Opus frame buffers: %1").arg(ffmpegError(allocated));
			return false;
		}
		return true;
	}

	void encodeVideo(const video_data *input)
	{
		if (!active_ || !videoCodec_ || !videoFrame_)
			return;
		if (av_frame_make_writable(videoFrame_) < 0)
			return;
		videoScaler_ = sws_getCachedContext(videoScaler_, videoCodec_->width, videoCodec_->height,
						     AV_PIX_FMT_BGRA, videoCodec_->width, videoCodec_->height,
						     AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		if (!videoScaler_)
			return;
		const std::uint8_t *sourcePlanes[4] = {input->data[0], nullptr, nullptr, nullptr};
		const int sourceStrides[4] = {static_cast<int>(input->linesize[0]), 0, 0, 0};
		if (sws_scale(videoScaler_, sourcePlanes, sourceStrides, 0, videoCodec_->height,
			      videoFrame_->data, videoFrame_->linesize) <= 0)
			return;
		videoFrame_->pts = videoPts_++;
		encodeFrame(videoCodec_, videoFrame_, EncodedMediaPacket::Kind::Video, 1'000'000 / videoFps);
	}

	void encodeAudio(const audio_data *input, bool muted)
	{
		if (!active_ || !audioCodec_ || !audioFrame_ || !audioResampler_)
			return;
		uint8_t *output[MAX_AV_PLANES] = {};
		uint32_t outputFrames = 0;
		uint64_t timestampOffset = 0;
		if (!audio_resampler_resample(audioResampler_, output, &outputFrames, &timestampOffset,
					      const_cast<const uint8_t *const *>(input->data), input->frames))
			return;
		const auto *samples = reinterpret_cast<const float *>(output[0]);
		const std::size_t count = static_cast<std::size_t>(outputFrames) * audioChannels;
		const std::size_t oldSize = audioBuffer_.size();
		audioBuffer_.resize(oldSize + count);
		if (muted)
			std::fill(audioBuffer_.begin() + static_cast<std::ptrdiff_t>(oldSize), audioBuffer_.end(), 0.0f);
		else
			std::copy(samples, samples + count, audioBuffer_.begin() + static_cast<std::ptrdiff_t>(oldSize));

		const std::size_t packetSamples = audioFrameSamples * audioChannels;
		while (audioBuffer_.size() >= packetSamples) {
			if (av_frame_make_writable(audioFrame_) < 0)
				return;
			std::memcpy(audioFrame_->data[0], audioBuffer_.data(), packetSamples * sizeof(float));
			audioBuffer_.erase(audioBuffer_.begin(),
					   audioBuffer_.begin() + static_cast<std::ptrdiff_t>(packetSamples));
			audioFrame_->pts = audioPts_;
			audioPts_ += audioFrameSamples;
			encodeFrame(audioCodec_, audioFrame_, EncodedMediaPacket::Kind::Audio, 20'000);
		}
	}

	void encodeFrame(AVCodecContext *codec, AVFrame *frame, EncodedMediaPacket::Kind kind,
			 std::uint64_t durationUs)
	{
		int result = avcodec_send_frame(codec, frame);
		if (result < 0)
			return;
		AVPacket *packet = av_packet_alloc();
		if (!packet)
			return;
		while ((result = avcodec_receive_packet(codec, packet)) >= 0) {
			EncodedMediaPacket encoded;
			encoded.kind = kind;
			encoded.durationUs = durationUs;
			encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
			encoded.data.resize(static_cast<std::size_t>(packet->size));
			std::memcpy(encoded.data.data(), packet->data, encoded.data.size());
			if (owner_->onPacket)
				owner_->onPacket(std::move(encoded));
			av_packet_unref(packet);
		}
		av_packet_free(&packet);
	}

	OutgoingMedia *owner_ = nullptr;
	obs_source_t *videoSource_ = nullptr;
	obs_source_t *audioSource_ = nullptr;
	obs_view_t *view_ = nullptr;
	obs_scene_t *videoScene_ = nullptr;
	obs_sceneitem_t *videoItem_ = nullptr;
	video_t *videoOutput_ = nullptr;
	obs_output_t *captureOutput_ = nullptr;
	audio_t *audioOutput_ = nullptr;
	CaptureTarget captureTarget_;
	audio_resampler_t *audioResampler_ = nullptr;
	AVCodecContext *videoCodec_ = nullptr;
	SwsContext *videoScaler_ = nullptr;
	AVCodecContext *audioCodec_ = nullptr;
	AVFrame *videoFrame_ = nullptr;
	AVFrame *audioFrame_ = nullptr;
	std::vector<float> audioBuffer_;
	uint32_t videoWidth_ = 0;
	uint32_t videoHeight_ = 0;
	std::int64_t videoPts_ = 0;
	std::int64_t audioPts_ = 0;
	bool audioConnected_ = false;
	bool mixedAudioConnected_ = false;
	bool frontendCallbackConnected_ = false;
	std::atomic_bool audioFromProgramMix_{false};
	OutgoingMediaSelection selection_;
	std::mutex sourceMutex_;
	std::atomic_bool active_{false};
};

OutgoingMedia::OutgoingMedia() : impl_(std::make_unique<Impl>(this)) {}
OutgoingMedia::~OutgoingMedia() = default;

bool OutgoingMedia::start(const OutgoingMediaSelection &selection, QString &error)
{
	return impl_->start(selection, error);
}

void OutgoingMedia::stop()
{
	impl_->stop();
}

bool OutgoingMedia::active() const
{
	return impl_->active();
}

void registerOutgoingMediaOutput()
{
	obs_output_info info = {};
	info.id = captureOutputId;
	info.flags = OBS_OUTPUT_VIDEO;
	info.get_name = captureOutputName;
	info.create = captureOutputCreate;
	info.destroy = captureOutputDestroy;
	info.start = captureOutputStart;
	info.stop = captureOutputStop;
	info.raw_video = captureOutputVideo;
	obs_register_output(&info);
}

} // namespace nextcloud_talk
