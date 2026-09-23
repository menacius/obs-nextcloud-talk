#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct obs_source;
using obs_source_t = struct obs_source;

namespace nextcloud_talk {

enum class OutgoingMediaMode { Devices, Scene, Preview, Program };

struct OutgoingMediaSelection {
	OutgoingMediaMode mode = OutgoingMediaMode::Devices;
	QString videoSourceName;
	QString audioSourceName;
	QString sceneName;
};

QString outgoingMediaModeId(OutgoingMediaMode mode);
OutgoingMediaMode outgoingMediaModeFromId(const QString &id);

// Returns a retained reference to the currently selected video source. The
// caller must release it with obs_source_release().
obs_source_t *resolveOutgoingVideoSource(const OutgoingMediaSelection &selection);

// Registers the private raw-video output used to activate an auxiliary OBS
// view. Call once while the module is loading, before an OutgoingMedia starts.
void registerOutgoingMediaOutput();

struct EncodedMediaPacket {
	enum class Kind { Audio, Video };

	Kind kind = Kind::Audio;
	std::vector<std::uint8_t> data;
	std::uint64_t durationUs = 0;
	bool keyframe = false;
};

// Captures the selected OBS devices, scene, Preview, or Program and encodes it
// as WebRTC-compatible Opus and VP8 access units.
class OutgoingMedia final {
public:
	OutgoingMedia();
	~OutgoingMedia();

	OutgoingMedia(const OutgoingMedia &) = delete;
	OutgoingMedia &operator=(const OutgoingMedia &) = delete;

	bool start(const OutgoingMediaSelection &selection, QString &error);
	void stop();
	bool active() const;

	std::function<void(EncodedMediaPacket)> onPacket;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace nextcloud_talk
