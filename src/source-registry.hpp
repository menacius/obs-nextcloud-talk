#pragma once

#include "talk-types.hpp"

#include <QString>

#include <functional>
#include <map>
#include <vector>

struct obs_source;
using obs_source_t = struct obs_source;

namespace nextcloud_talk {

enum class ParticipantSourceKind { Video, Audio, Screen };

class SourceRegistry final {
public:
	SourceRegistry() = default;
	~SourceRegistry();

	SourceRegistry(const SourceRegistry &) = delete;
	SourceRegistry &operator=(const SourceRegistry &) = delete;

	void sync(const std::vector<Participant> &participants, const QString &localUsername);
	void updateMediaState(const ParticipantMediaState &state);
	void markAllOffline();
	std::vector<Participant> onlineParticipants() const;
	obs_source_t *sourceFor(const QString &participantId, ParticipantSourceKind kind) const;

	std::function<void()> onChanged;

private:
	struct Entry {
		Participant participant;
		obs_source_t *videoSource = nullptr;
		obs_source_t *audioSource = nullptr;
		obs_source_t *screenSource = nullptr;
		bool online = false;
	};

	obs_source_t *findOrCreate(const QString &name, const QString &participantId, const QString &kind);
	static QString sourceName(const Participant &participant, const QString &kind);

	std::map<QString, Entry> entries_;
	std::map<QString, ParticipantMediaState> mediaStates_;
};

} // namespace nextcloud_talk
