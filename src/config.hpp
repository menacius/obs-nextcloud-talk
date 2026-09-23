#pragma once

#include <QString>

namespace nextcloud_talk {

struct PluginConfig {
	QString serverUrl;
	QString username;
	QString conversationToken;
	QString outgoingMode = QStringLiteral("devices");
	QString outgoingVideoSource;
	QString outgoingAudioSource;
	QString outgoingScene;
	bool feedbackGuard = true;

	static PluginConfig load();
	void save() const;
};

} // namespace nextcloud_talk
