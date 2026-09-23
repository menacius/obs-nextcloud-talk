#include "source-registry.hpp"

#include "participant-source.hpp"

#include <obs.h>

#include <QCryptographicHash>
#include <QSet>

namespace nextcloud_talk {
namespace {

struct SourceTargetLookup {
	QStringList targets;
	obs_source_t *source = nullptr;
};

bool findSourceByTarget(void *data, obs_source_t *source)
{
	auto *lookup = static_cast<SourceTargetLookup *>(data);
	const char *id = obs_source_get_id(source);
	if (!id || QString::fromUtf8(id) != QString::fromUtf8(UnifiedSourceId))
		return true;
	obs_data_t *settings = obs_source_get_settings(source);
	const QString target = QString::fromUtf8(obs_data_get_string(settings, "target"));
	obs_data_release(settings);
	if (!lookup->targets.contains(target))
		return true;
	lookup->source = obs_source_get_ref(source);
	return false;
}

} // namespace

SourceRegistry::~SourceRegistry()
{
	for (auto &[id, entry] : entries_) {
		Q_UNUSED(id);
		obs_source_release(entry.videoSource);
		obs_source_release(entry.audioSource);
		obs_source_release(entry.screenSource);
	}
}

void SourceRegistry::sync(const std::vector<Participant> &participants, const QString &localUsername)
{
	setParticipantSourceChoices(participants, localUsername);
	QSet<QString> seen;
	for (const Participant &participant : participants) {
		if (participant.stableId == QStringLiteral("users:") + localUsername)
			continue;

		seen.insert(participant.stableId);
		auto [iterator, inserted] = entries_.try_emplace(participant.stableId);
		Entry &entry = iterator->second;
		entry.participant = participant;
		const auto mediaState = mediaStates_.find(participant.stableId);
		if (mediaState != mediaStates_.end()) {
			if (mediaState->second.audioEnabled)
				entry.participant.audioAvailable = *mediaState->second.audioEnabled;
			if (mediaState->second.videoEnabled)
				entry.participant.videoAvailable = *mediaState->second.videoEnabled;
			if (mediaState->second.screenSharing)
				entry.participant.screenSharing = *mediaState->second.screenSharing;
			if (mediaState->second.speaking)
				entry.participant.speaking = *mediaState->second.speaking;
		}
		entry.online = true;
		if (inserted || !entry.videoSource) {
			entry.videoSource = findOrCreate(sourceName(participant, QStringLiteral("V")),
						       participant.stableId, QStringLiteral("video"));
		}
		if (inserted || !entry.audioSource) {
			entry.audioSource = findOrCreate(sourceName(participant, QStringLiteral("A")),
						       participant.stableId, QStringLiteral("audio"));
		}
		if (inserted || !entry.screenSource) {
			entry.screenSource = findOrCreate(sourceName(participant, QStringLiteral("S")),
							participant.stableId, QStringLiteral("screen"));
		}
		setParticipantMediaOnline(participant.stableId, true);
	}

	for (auto &[id, entry] : entries_) {
		if (!seen.contains(id)) {
			entry.online = false;
			setParticipantMediaOnline(id, false);
			setParticipantScreenOnline(id, false);
			mediaStates_.erase(id);
		}
	}

	if (onChanged)
		onChanged();
}

void SourceRegistry::updateMediaState(const ParticipantMediaState &state)
{
	if (state.participantId.isEmpty())
		return;
	ParticipantMediaState &stored = mediaStates_[state.participantId];
	stored.participantId = state.participantId;
	if (state.audioEnabled)
		stored.audioEnabled = state.audioEnabled;
	if (state.videoEnabled)
		stored.videoEnabled = state.videoEnabled;
	if (state.screenSharing)
		stored.screenSharing = state.screenSharing;
	if (state.speaking)
		stored.speaking = state.speaking;
	if (state.audioEnabled && !*state.audioEnabled)
		stored.speaking = false;

	auto entry = entries_.find(state.participantId);
	if (entry == entries_.end())
		return;
	if (state.audioEnabled)
		entry->second.participant.audioAvailable = *state.audioEnabled;
	if (state.videoEnabled)
		entry->second.participant.videoAvailable = *state.videoEnabled;
	if (state.videoEnabled)
		setParticipantVideoAvailable(state.participantId, *state.videoEnabled);
	if (state.screenSharing)
		entry->second.participant.screenSharing = *state.screenSharing;
	if (state.speaking)
		entry->second.participant.speaking = *state.speaking;
	if (state.audioEnabled && !*state.audioEnabled)
		entry->second.participant.speaking = false;
	if (onChanged)
		onChanged();
}

void SourceRegistry::markAllOffline()
{
	for (auto &[id, entry] : entries_) {
		entry.online = false;
		setParticipantMediaOnline(id, false);
		setParticipantScreenOnline(id, false);
	}
	mediaStates_.clear();
	if (onChanged)
		onChanged();
}

std::vector<Participant> SourceRegistry::onlineParticipants() const
{
	std::vector<Participant> result;
	for (const auto &[id, entry] : entries_) {
		Q_UNUSED(id);
		if (entry.online)
			result.push_back(entry.participant);
	}
	return result;
}

obs_source_t *SourceRegistry::sourceFor(const QString &participantId, ParticipantSourceKind kind) const
{
	const auto iterator = entries_.find(participantId);
	if (iterator == entries_.end())
		return nullptr;
	switch (kind) {
	case ParticipantSourceKind::Video:
		return iterator->second.videoSource;
	case ParticipantSourceKind::Audio:
		return iterator->second.audioSource;
	case ParticipantSourceKind::Screen:
		return iterator->second.screenSource;
	}
	return nullptr;
}

obs_source_t *SourceRegistry::findOrCreate(const QString &requestedName, const QString &participantId,
					   const QString &kind)
{
	QString name = requestedName;
	const QString target = kind + QLatin1Char('|') + participantId;
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	if (source) {
		const char *existingId = obs_source_get_id(source);
		obs_data_t *settings = obs_source_get_settings(source);
		const QString existingTarget = QString::fromUtf8(obs_data_get_string(settings, "target"));
		obs_data_release(settings);
		if (existingId && QString::fromUtf8(existingId) == QString::fromUtf8(UnifiedSourceId) &&
		    existingTarget == target)
			return source;
		obs_source_release(source);
		source = nullptr;
		const QByteArray suffix =
			QCryptographicHash::hash(participantId.toUtf8(), QCryptographicHash::Sha1).toHex().left(6);
		name += QStringLiteral(" [%1]").arg(QString::fromLatin1(suffix));

		source = obs_get_source_by_name(name.toUtf8().constData());
		if (source) {
			const char *suffixedId = obs_source_get_id(source);
			obs_data_t *suffixedSettings = obs_source_get_settings(source);
			const QString suffixedTarget = QString::fromUtf8(obs_data_get_string(suffixedSettings, "target"));
			obs_data_release(suffixedSettings);
			if (suffixedId && QString::fromUtf8(suffixedId) == QString::fromUtf8(UnifiedSourceId) &&
			    suffixedTarget == target)
				return source;
			obs_source_release(source);
			source = nullptr;
			name += QStringLiteral(" (Talk)");
		}
	}
	if (!source) {
		SourceTargetLookup lookup{{target}};
		if (kind == QStringLiteral("video"))
			lookup.targets.push_back(QStringLiteral("participant|") + participantId);
		obs_enum_sources(findSourceByTarget, &lookup);
		if (lookup.source) {
			obs_data_t *settings = obs_source_get_settings(lookup.source);
			obs_data_set_string(settings, "target", target.toUtf8().constData());
			obs_source_update(lookup.source, settings);
			obs_data_release(settings);
			obs_source_set_name(lookup.source, name.toUtf8().constData());
			return lookup.source;
		}
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "target", target.toUtf8().constData());
	source = obs_source_create(UnifiedSourceId, name.toUtf8().constData(), settings, nullptr);
	obs_data_release(settings);
	if (!source)
		blog(LOG_ERROR, "[nextcloud-talk] Could not create source '%s'", name.toUtf8().constData());
	return source;
}

QString SourceRegistry::sourceName(const Participant &participant, const QString &kind)
{
	return QStringLiteral("%1 - %2 - Talk").arg(kind, participant.displayName);
}

} // namespace nextcloud_talk
