#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nextcloud_talk {

// Implements the key exchange and encoded-frame transform used by the Talk web
// client when call end-to-end encryption is enabled.
class TalkE2ee final {
public:
	TalkE2ee();
	~TalkE2ee();

	TalkE2ee(const TalkE2ee &) = delete;
	TalkE2ee &operator=(const TalkE2ee &) = delete;

	void start(const QString &localSessionId);
	void stop();
	void participantJoined(const QString &sessionId);
	void participantLeft(const QString &sessionId);
	bool handleMessage(const QString &sender, const QJsonObject &message);

	bool encryptFrame(std::vector<std::uint8_t> &frame, bool video, bool keyframe,
			  std::uint32_t ssrc, std::uint32_t timestamp);
	bool decryptFrame(const QString &sender, std::vector<std::uint8_t> &frame, bool video,
			  bool keyframe);

	std::function<void(const QString &, const QJsonObject &)> sendCallMessage;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace nextcloud_talk
