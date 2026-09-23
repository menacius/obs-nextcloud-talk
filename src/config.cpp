#include "config.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <memory>

namespace nextcloud_talk {
namespace {

using ObsData = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>;

QString readString(obs_data_t *data, const char *name)
{
	return QString::fromUtf8(obs_data_get_string(data, name));
}

} // namespace

PluginConfig PluginConfig::load()
{
	PluginConfig config;
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return config;

	ObsData data(obs_data_create_from_json_file_safe(path, "bak"), obs_data_release);
	bfree(path);
	if (!data)
		return config;

	config.serverUrl = readString(data.get(), "server_url");
	config.username = readString(data.get(), "username");
	config.conversationToken = readString(data.get(), "conversation_token");
	config.outgoingMode = readString(data.get(), "outgoing_mode");
	if (config.outgoingMode.isEmpty())
		config.outgoingMode = QStringLiteral("devices");
	config.outgoingVideoSource = readString(data.get(), "outgoing_video_source");
	config.outgoingAudioSource = readString(data.get(), "outgoing_audio_source");
	config.outgoingScene = readString(data.get(), "outgoing_scene");
	config.feedbackGuard = !obs_data_has_user_value(data.get(), "feedback_guard") ||
			       obs_data_get_bool(data.get(), "feedback_guard");
	return config;
}

void PluginConfig::save() const
{
	char *directory = obs_module_config_path("");
	char *path = obs_module_config_path("settings.json");
	if (!directory || !path) {
		bfree(directory);
		bfree(path);
		return;
	}

	os_mkdirs(directory);
	ObsData data(obs_data_create(), obs_data_release);
	obs_data_set_string(data.get(), "server_url", serverUrl.toUtf8().constData());
	obs_data_set_string(data.get(), "username", username.toUtf8().constData());
	obs_data_set_string(data.get(), "conversation_token", conversationToken.toUtf8().constData());
	obs_data_set_string(data.get(), "outgoing_mode", outgoingMode.toUtf8().constData());
	obs_data_set_string(data.get(), "outgoing_video_source", outgoingVideoSource.toUtf8().constData());
	obs_data_set_string(data.get(), "outgoing_audio_source", outgoingAudioSource.toUtf8().constData());
	obs_data_set_string(data.get(), "outgoing_scene", outgoingScene.toUtf8().constData());
	obs_data_set_bool(data.get(), "feedback_guard", feedbackGuard);
	obs_data_save_json_safe(data.get(), path, "tmp", "bak");
	bfree(directory);
	bfree(path);
}

} // namespace nextcloud_talk
