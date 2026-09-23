#pragma once

#include "talk-types.hpp"

#include <QString>

namespace nextcloud_talk {

inline constexpr const char *UnifiedSourceId = "nextcloud_talk";

void registerParticipantSourceTypes();
void setParticipantSourceChoices(const std::vector<Participant> &participants, const QString &localUsername);
void setParticipantMediaOnline(const QString &participantId, bool online);
void setParticipantVideoAvailable(const QString &participantId, bool available);
void setParticipantScreenOnline(const QString &participantId, bool online);
void publishParticipantVideo(const QString &participantId, const VideoFrame &frame,
			     bool screenShare = false);
void publishParticipantAudio(const QString &participantId, const AudioFrame &frame);

} // namespace nextcloud_talk
