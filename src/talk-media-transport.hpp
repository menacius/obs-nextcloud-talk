#pragma once

#include "talk-types.hpp"
#include "outgoing-media.hpp"

#include <QByteArray>
#include <QObject>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rtc {
class PeerConnection;
class Track;
class DataChannel;
class RtcpSrReporter;
struct Configuration;
} // namespace rtc

namespace nextcloud_talk {

class WinHttpWebSocket;
class IncomingMediaDecoder;
class TalkE2ee;

// Owns the Talk signaling WebSocket, WebRTC peer connections, and selected-source
// publication. Incoming RTP is depacketized and decoded into persistent OBS sources.
class TalkMediaTransport final : public QObject {
public:
	explicit TalkMediaTransport(QObject *parent = nullptr);
	~TalkMediaTransport() override;

	void start(const QString &nextcloudUrl, const QString &roomToken, const QString &roomSessionId,
		   const SignalingSettings &settings, const OutgoingMediaSelection &outgoingSelection);
	void stop();

	std::function<void(const QString &)> onStatusChanged;
	std::function<void()> onPublishingReady;
	std::function<void(const ParticipantMediaState &)> onParticipantMediaState;

private:
	struct PeerSession {
		QString remoteSessionId;
		QString sid;
		QString roomType;
		QString participantId;
		bool publishesLocalMedia = false;
		std::atomic_bool preferredQualityRequested{false};
		std::shared_ptr<rtc::PeerConnection> connection;
		std::vector<std::shared_ptr<rtc::Track>> tracks;
		std::vector<std::shared_ptr<IncomingMediaDecoder>> decoders;
		std::vector<std::shared_ptr<rtc::DataChannel>> dataChannels;
		std::vector<std::function<void()>> restartVideoPipelines;
		std::shared_ptr<rtc::DataChannel> statusDataChannel;
		struct SendTrack {
			EncodedMediaPacket::Kind kind = EncodedMediaPacket::Kind::Audio;
			std::shared_ptr<rtc::Track> track;
			std::shared_ptr<rtc::RtcpSrReporter> reporter;
			std::mutex mutex;
		};
		std::vector<std::shared_ptr<SendTrack>> sendTracks;
	};

	void postStatus(const QString &status);
	void postParticipantMediaState(ParticipantMediaState state);
	void handleRemoteStatusMessage(const QString &participantId, const QJsonObject &message);
	void observeDataChannel(const std::shared_ptr<PeerSession> &peer,
				const std::shared_ptr<rtc::DataChannel> &channel, bool statusChannel);
	void handleDataChannelPayload(const std::shared_ptr<PeerSession> &peer, const QString &label,
				      const QByteArray &payload);
	void sendJson(QJsonObject message, bool addId = true);
	void sendHello();
	void sendRoomJoin();
	void handleTextMessage(const std::string &message);
	void handleMessageObject(const QJsonObject &message);
	void handleEvent(const QJsonObject &event);
	void handleParticipants(const QJsonArray &users);
	void pauseRemoteSession(const QString &sessionId);
	void resetRemoteStream(const QString &sessionId, const QString &roomType);
	void resetRemoteSession(const QString &sessionId, bool participantLeft);
	void retireOtherPeers(const std::shared_ptr<PeerSession> &activePeer);
	void updateServerFeatures(const QJsonArray &features);
	void requestOffer(const QString &sessionId);
	void offerToParticipant(const QString &sessionId);
	void sendLocalMediaState(const QString &sessionId = {});
	void startMcuPublisher();
	void notifyPublishingReady();
	void handleRtcMessage(const QString &sender, const QJsonObject &data);
	std::shared_ptr<PeerSession> createPeer(const QString &sender, const QString &sid,
						const QString &roomType, bool publishLocalMedia = false);
	void addOutgoingTracks(const std::shared_ptr<PeerSession> &peer, bool sendOnly);
	void sendEncodedPacket(EncodedMediaPacket packet);
	void requestPreferredIncomingQuality(const std::shared_ptr<PeerSession> &peer);
	void sendPeerMessage(const std::shared_ptr<PeerSession> &peer, const QString &type,
			     const QJsonObject &payload);
	std::string peerKey(const QString &sender, const QString &sid, const QString &roomType) const;
	void configureIce(const SignalingSettings &settings);

	std::mutex mutex_;
	std::shared_ptr<WinHttpWebSocket> websocket_;
	std::unordered_map<std::string, std::shared_ptr<PeerSession>> peers_;
	std::unordered_set<std::string> requestedOffers_;
	std::unordered_set<std::string> initiatedPeers_;
	std::unordered_set<std::string> serverFeatures_;
	std::unordered_set<std::string> remotelyStoppedVideo_;
	std::unordered_map<std::string, QString> remoteParticipantIds_;
	OutgoingMedia outgoingMedia_;
	std::unique_ptr<TalkE2ee> e2ee_;
	rtc::Configuration *rtcConfiguration_ = nullptr;
	std::atomic_bool active_{false};
	std::atomic_bool helloSent_{false};
	std::atomic_bool outgoingVideoSeen_{false};
	std::atomic_bool outgoingAudioSeen_{false};
	std::atomic_bool outgoingVideoSent_{false};
	std::atomic_bool outgoingAudioSent_{false};
	std::atomic_bool publishingReadyNotified_{false};
	std::atomic_uint64_t nextMessageId_{1};
	QString nextcloudUrl_;
	QString roomToken_;
	QString roomSessionId_;
	QString signalingSessionId_;
	SignalingSettings settings_;
	bool hasMcu_ = false;
};

} // namespace nextcloud_talk
