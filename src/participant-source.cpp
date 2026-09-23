#include "participant-source.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <map>
#include <string>
#include <vector>

namespace nextcloud_talk {
namespace {

struct ParticipantSource {
	obs_source_t *source = nullptr;
	QString participantId;
	enum class Kind { Video, Audio, Screen } kind = Kind::Video;
	bool online = false;
	bool hasVideoFrame = false;
	bool cameraEnabled = true;
	std::vector<std::uint8_t> placeholder;
};

bool isVisual(const ParticipantSource *instance)
{
	return instance && instance->kind != ParticipantSource::Kind::Audio;
}

std::mutex instancesMutex;
std::vector<ParticipantSource *> instances;
std::mutex choicesMutex;
std::map<QString, QString> participantChoices;

void renderPlaceholder(ParticipantSource *instance)
{
	if (!isVisual(instance))
		return;

	constexpr std::uint32_t width = 640;
	constexpr std::uint32_t height = 360;
	instance->placeholder.resize(width * height * 4);
	std::uint32_t hash = 2166136261u;
	for (const char byte : instance->participantId.toUtf8()) {
		hash ^= static_cast<std::uint8_t>(byte);
		hash *= 16777619u;
	}
	const std::uint8_t baseR = instance->online ? static_cast<std::uint8_t>(48 + (hash & 0x5f)) : 42;
	const std::uint8_t baseG = instance->online ? static_cast<std::uint8_t>(52 + ((hash >> 8) & 0x5f)) : 42;
	const std::uint8_t baseB = instance->online ? static_cast<std::uint8_t>(68 + ((hash >> 16) & 0x5f)) : 46;

	for (std::uint32_t y = 0; y < height; ++y) {
		for (std::uint32_t x = 0; x < width; ++x) {
			const bool accent = ((x + y) / 48) % 2 == 0;
			const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
			instance->placeholder[offset + 0] = static_cast<std::uint8_t>(baseB + (accent ? 8 : 0));
			instance->placeholder[offset + 1] = static_cast<std::uint8_t>(baseG + (accent ? 8 : 0));
			instance->placeholder[offset + 2] = static_cast<std::uint8_t>(baseR + (accent ? 8 : 0));
			instance->placeholder[offset + 3] = 255;
		}
	}

	obs_source_frame frame{};
	frame.data[0] = instance->placeholder.data();
	frame.linesize[0] = width * 4;
	frame.width = width;
	frame.height = height;
	frame.format = VIDEO_FORMAT_BGRA;
	frame.timestamp = os_gettime_ns();
	frame.full_range = true;
	obs_source_output_video(instance->source, &frame);
}

void update(void *data, obs_data_t *settings)
{
	auto *instance = static_cast<ParticipantSource *>(data);
	QString participantId = QString::fromUtf8(obs_data_get_string(settings, "participant_id"));
	const QString target = QString::fromUtf8(obs_data_get_string(settings, "target"));
	if (!target.isEmpty()) {
		const qsizetype separator = target.indexOf('|');
		if (separator > 0) {
			const QString kind = target.left(separator);
			instance->kind = kind == QStringLiteral("audio")
						 ? ParticipantSource::Kind::Audio
						 : kind == QStringLiteral("screen") ? ParticipantSource::Kind::Screen
										      : ParticipantSource::Kind::Video;
			participantId = target.mid(separator + 1);
		}
	}
	obs_source_set_audio_active(instance->source, instance->kind == ParticipantSource::Kind::Audio);
	if (isVisual(instance))
		obs_source_set_async_unbuffered(instance->source, true);
	const bool identityChanged = instance->participantId != participantId;
	instance->participantId = participantId;
	if (identityChanged)
		instance->hasVideoFrame = false;
	if (isVisual(instance) && (identityChanged || instance->placeholder.empty()))
		renderPlaceholder(instance);
}

void *createCommon(obs_data_t *settings, obs_source_t *source, ParticipantSource::Kind kind)
{
	auto *instance = new ParticipantSource;
	instance->source = source;
	instance->kind = kind;
	update(instance, settings);
	{
		std::lock_guard<std::mutex> lock(instancesMutex);
		instances.push_back(instance);
	}
	return instance;
}

void *createUnified(obs_data_t *settings, obs_source_t *source)
{
	return createCommon(settings, source, ParticipantSource::Kind::Video);
}

void destroy(void *data)
{
	auto *instance = static_cast<ParticipantSource *>(data);
	{
		std::lock_guard<std::mutex> lock(instancesMutex);
		instances.erase(std::remove(instances.begin(), instances.end(), instance), instances.end());
	}
	delete instance;
}

const char *unifiedName(void *)
{
	return obs_module_text("Source.Unified");
}

obs_properties_t *unifiedProperties(void *)
{
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *target = obs_properties_add_list(properties, "target", "Source", OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(target, "Select a participant source", "");
	std::lock_guard<std::mutex> lock(choicesMutex);
	for (const auto &[participantId, displayName] : participantChoices) {
		const QByteArray videoTarget = (QStringLiteral("video|") + participantId).toUtf8();
		const QByteArray audioTarget = (QStringLiteral("audio|") + participantId).toUtf8();
		const QByteArray screenTarget = (QStringLiteral("screen|") + participantId).toUtf8();
		const QByteArray videoLabel = QStringLiteral("Video — %1").arg(displayName).toUtf8();
		const QByteArray audioLabel = QStringLiteral("Audio — %1").arg(displayName).toUtf8();
		const QByteArray screenLabel = QStringLiteral("Screenshare — %1").arg(displayName).toUtf8();
		obs_property_list_add_string(target, videoLabel.constData(), videoTarget.constData());
		obs_property_list_add_string(target, audioLabel.constData(), audioTarget.constData());
		obs_property_list_add_string(target, screenLabel.constData(), screenTarget.constData());
	}
	return properties;
}

void unifiedDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "target", "");
}

} // namespace

void registerParticipantSourceTypes()
{
	obs_source_info unified{};
	unified.id = UnifiedSourceId;
	unified.type = OBS_SOURCE_TYPE_INPUT;
	unified.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	unified.get_name = unifiedName;
	unified.create = createUnified;
	unified.destroy = destroy;
	unified.update = update;
	unified.get_defaults = unifiedDefaults;
	unified.get_properties = unifiedProperties;
	obs_register_source(&unified);
}

void setParticipantSourceChoices(const std::vector<Participant> &participants, const QString &localUsername)
{
	std::lock_guard<std::mutex> lock(choicesMutex);
	participantChoices.clear();
	for (const Participant &participant : participants) {
		if (participant.stableId == QStringLiteral("users:") + localUsername)
			continue;
		participantChoices[participant.stableId] = participant.displayName;
	}
}

void setParticipantMediaOnline(const QString &participantId, bool online)
{
	std::lock_guard<std::mutex> lock(instancesMutex);
	for (ParticipantSource *instance : instances) {
		if (instance->participantId == participantId && instance->kind == ParticipantSource::Kind::Video) {
			const bool stateChanged = instance->online != online;
			instance->online = online;
			if (!online)
				instance->cameraEnabled = true;
			if (!online)
				instance->hasVideoFrame = false;
			if (stateChanged && (!online || !instance->hasVideoFrame))
				renderPlaceholder(instance);
		}
	}
}

void setParticipantVideoAvailable(const QString &participantId, bool available)
{
	std::lock_guard<std::mutex> lock(instancesMutex);
	for (ParticipantSource *instance : instances) {
		if (instance->participantId != participantId || instance->kind != ParticipantSource::Kind::Video)
			continue;
		if (instance->cameraEnabled == available)
			continue;
		instance->cameraEnabled = available;
		if (!available) {
			instance->hasVideoFrame = false;
			// Drop all timestamped frames that OBS may still have queued from
			// before the camera was stopped, then expose the placeholder now.
			obs_source_output_video(instance->source, nullptr);
			renderPlaceholder(instance);
		}
	}
}

void setParticipantScreenOnline(const QString &participantId, bool online)
{
	std::lock_guard<std::mutex> lock(instancesMutex);
	for (ParticipantSource *instance : instances) {
		if (instance->participantId != participantId ||
		    instance->kind != ParticipantSource::Kind::Screen)
			continue;
		const bool stateChanged = instance->online != online;
		instance->online = online;
		if (!online)
			instance->hasVideoFrame = false;
		if (stateChanged && (!online || !instance->hasVideoFrame))
			renderPlaceholder(instance);
	}
}

void publishParticipantVideo(const QString &participantId, const VideoFrame &input, bool screenShare)
{
	std::lock_guard<std::mutex> lock(instancesMutex);
	for (ParticipantSource *instance : instances) {
		const auto expectedKind = screenShare ? ParticipantSource::Kind::Screen
						      : ParticipantSource::Kind::Video;
		if (instance->participantId != participantId || instance->kind != expectedKind)
			continue;
		if (!screenShare && !instance->cameraEnabled)
			continue;
		instance->online = true;
		instance->hasVideoFrame = true;
		obs_source_frame frame{};
		for (std::size_t index = 0; index < 8; ++index) {
			frame.data[index] = const_cast<std::uint8_t *>(input.planes[index]);
			frame.linesize[index] = input.linesize[index];
		}
		frame.width = input.width;
		frame.height = input.height;
		frame.timestamp = input.timestampNs;
		frame.format = static_cast<video_format>(input.obsFormat);
		obs_source_output_video(instance->source, &frame);
	}
}

void publishParticipantAudio(const QString &participantId, const AudioFrame &input)
{
	std::lock_guard<std::mutex> lock(instancesMutex);
	for (ParticipantSource *instance : instances) {
		if (instance->participantId != participantId ||
		    instance->kind != ParticipantSource::Kind::Audio)
			continue;
		obs_source_audio audio{};
		for (std::size_t index = 0; index < 8; ++index)
			audio.data[index] = input.planes[index];
		audio.frames = input.frames;
		audio.samples_per_sec = input.sampleRate;
		audio.timestamp = input.timestampNs;
		audio.format = static_cast<audio_format>(input.obsFormat);
		audio.speakers = static_cast<speaker_layout>(input.obsSpeakers);
		obs_source_output_audio(instance->source, &audio);
	}
}

} // namespace nextcloud_talk
