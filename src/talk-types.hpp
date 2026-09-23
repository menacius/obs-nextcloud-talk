#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>

#include <cstdint>
#include <optional>
#include <vector>

namespace nextcloud_talk {

enum class ConnectionState {
	Disconnected,
	LoadingConversations,
	Ready,
	Joining,
	InCall,
	Leaving,
	Error,
};

struct Conversation {
	QString token;
	QString displayName;
	bool callActive = false;
	bool canStartCall = true;
};

struct Participant {
	QString stableId;
	QString sessionId;
	QString displayName;
	bool audioAvailable = true;
	bool videoAvailable = true;
	bool screenSharing = false;
	bool speaking = false;
};

struct ParticipantMediaState {
	QString participantId;
	std::optional<bool> audioEnabled;
	std::optional<bool> videoEnabled;
	std::optional<bool> screenSharing;
	std::optional<bool> speaking;
};

struct ChatMessage {
	qint64 id = 0;
	qint64 timestamp = 0;
	QString actorDisplayName;
	QString message;
	QString messageType;
	QString systemMessage;
};

struct IceServerConfig {
	QStringList urls;
	QString username;
	QString credential;
};

struct SignalingSettings {
	QString mode;
	QString server;
	QString userId;
	QJsonObject helloAuthV1;
	QJsonObject helloAuthV2;
	std::vector<IceServerConfig> stunServers;
	std::vector<IceServerConfig> turnServers;
};

struct VideoFrame {
	const std::uint8_t *planes[8]{};
	std::uint32_t linesize[8]{};
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint64_t timestampNs = 0;
	int obsFormat = 0;
};

struct AudioFrame {
	const std::uint8_t *planes[8]{};
	std::uint32_t frames = 0;
	std::uint32_t sampleRate = 48000;
	std::uint64_t timestampNs = 0;
	int obsFormat = 0;
	int obsSpeakers = 0;
};

} // namespace nextcloud_talk
