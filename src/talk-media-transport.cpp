#include "talk-media-transport.hpp"
#include "incoming-media-decoder.hpp"
#include "obs-log.hpp"
#include "participant-source.hpp"
#include "talk-e2ee.hpp"
#include "winhttp-websocket.hpp"

#include <rtc/rtc.hpp>
#include <util/platform.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QDebug>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nextcloud_talk {
namespace {

std::string utf8(const QString &value)
{
	const QByteArray bytes = value.toUtf8();
	return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString fromUtf8(const std::string &value)
{
	return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

bool hasPublishedMedia(const QJsonValue &value)
{
	if (value.isBool())
		return value.toBool();
	const int flags = value.toInt(0);
	return (flags & 2) != 0 || (flags & 4) != 0;
}

QString participantStableId(const QJsonObject &user, const QString &fallback)
{
	const QString actorType = user.value(QStringLiteral("actorType")).toString(
		user.value(QStringLiteral("actortype")).toString());
	const QString actorId = user.value(QStringLiteral("actorId")).toString(
		user.value(QStringLiteral("actorid")).toString(
			user.value(QStringLiteral("userId")).toString(
				user.value(QStringLiteral("userid")).toString())));
	return actorType.isEmpty() || actorId.isEmpty() ? fallback : actorType + QLatin1Char(':') + actorId;
}

IncomingCodec codecFromName(const std::string &name)
{
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	if (lower == "h264")
		return IncomingCodec::H264;
	if (lower == "vp8")
		return IncomingCodec::Vp8;
	if (lower == "opus")
		return IncomingCodec::Opus;
	return IncomingCodec::Unsupported;
}

bool isH264Keyframe(const rtc::binary &frame)
{
	for (std::size_t index = 0; index + 4 < frame.size(); ++index) {
		const auto byte = [&frame](std::size_t offset) {
			return std::to_integer<std::uint8_t>(frame[offset]);
		};
		std::size_t nalOffset = 0;
		if (byte(index) == 0 && byte(index + 1) == 0 && byte(index + 2) == 1)
			nalOffset = index + 3;
		else if (index + 4 < frame.size() && byte(index) == 0 && byte(index + 1) == 0 &&
			 byte(index + 2) == 0 && byte(index + 3) == 1)
			nalOffset = index + 4;
		if (nalOffset != 0 && nalOffset < frame.size() && (byte(nalOffset) & 0x1f) == 5)
			return true;
	}
	return false;
}

std::optional<std::uint8_t> rtpExtensionValue(const rtc::message_ptr &message, int wantedId)
{
	if (!message || wantedId <= 0 || wantedId > 255 || message->type != rtc::Message::Binary ||
	    message->size() < sizeof(rtc::RtpHeader))
		return std::nullopt;
	const auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
	if (!header->extension())
		return std::nullopt;
	const auto *extension = header->getExtensionHeader();
	if (!extension)
		return std::nullopt;
	const std::size_t bodySize = static_cast<std::size_t>(extension->headerLength()) * 4;
	if (header->getSize() + sizeof(rtc::RtpExtensionHeader) + bodySize > message->size())
		return std::nullopt;
	const auto *body = reinterpret_cast<const std::uint8_t *>(extension->getBody());
	std::size_t offset = 0;
	const std::uint16_t profile = extension->profileSpecificId();
	if (profile == 0xbede) {
		while (offset < bodySize) {
			const std::uint8_t descriptor = body[offset++];
			if (descriptor == 0)
				continue;
			const int id = descriptor >> 4;
			if (id == 15)
				break;
			const std::size_t length = (descriptor & 0x0f) + 1;
			if (offset + length > bodySize)
				break;
			if (id == wantedId && length > 0)
				return body[offset];
			offset += length;
		}
	} else if ((profile & 0xfff0) == 0x1000) {
		while (offset < bodySize) {
			const int id = body[offset++];
			if (id == 0)
				continue;
			if (offset >= bodySize)
				break;
			const std::size_t length = body[offset++];
			if (offset + length > bodySize)
				break;
			if (id == wantedId && length > 0)
				return body[offset];
			offset += length;
		}
	}
	return std::nullopt;
}

class Vp8RtpDepacketizer final : public rtc::MediaHandler {
public:
	void reset()
	{
		frames_.clear();
		frameOrder_.clear();
	}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &) override
	{
		rtc::message_vector output;
		for (const auto &message : messages) {
			PacketFragment fragment;
			if (!parsePacket(message, fragment))
				continue;

			auto [position, inserted] = frames_.try_emplace(fragment.timestamp);
			FrameState &state = position->second;
			if (inserted) {
				state.timestamp = fragment.timestamp;
				state.payloadType = fragment.payloadType;
				frameOrder_.push_back(fragment.timestamp);
			}
			if (fragment.start) {
				state.hasStart = true;
				state.startSequence = fragment.sequence;
			}
			if (fragment.marker) {
				state.hasMarker = true;
				state.markerSequence = fragment.sequence;
			}
			state.payloads.insert_or_assign(fragment.sequence, std::move(fragment.payload));

			if (!state.completeFrame)
				state.completeFrame = assembleIfComplete(state);
			drainReadyFrames(output);
			pruneOldFrames(output);
		}
		messages = std::move(output);
	}

private:
	struct PacketFragment {
		std::uint32_t timestamp = 0;
		std::uint16_t sequence = 0;
		std::uint8_t payloadType = 0;
		bool start = false;
		bool marker = false;
		rtc::binary payload;
	};

	struct FrameState {
		std::uint32_t timestamp = 0;
		std::uint16_t startSequence = 0;
		std::uint16_t markerSequence = 0;
		std::uint8_t payloadType = 0;
		bool hasStart = false;
		bool hasMarker = false;
		std::unordered_map<std::uint16_t, rtc::binary> payloads;
		rtc::message_ptr completeFrame;
	};

	bool parsePacket(const rtc::message_ptr &message, PacketFragment &result)
	{
		if (!message || message->type != rtc::Message::Binary || message->size() < sizeof(rtc::RtpHeader))
			return false;
		const auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
		const std::size_t headerSize = header->getSize() + header->getExtensionHeaderSize();
		const std::size_t padding = header->padding() ? std::to_integer<std::uint8_t>(message->back()) : 0;
		if (message->size() <= headerSize + padding)
			return false;

		const std::byte *payload = message->data() + headerSize;
		const std::size_t payloadSize = message->size() - headerSize - padding;
		if (payloadSize < 1)
			return false;
		const std::uint8_t descriptor = std::to_integer<std::uint8_t>(payload[0]);
		std::size_t descriptorSize = 1;
		if (descriptor & 0x80) {
			if (payloadSize <= descriptorSize)
				return false;
			const std::uint8_t extension = std::to_integer<std::uint8_t>(payload[descriptorSize++]);
			if (extension & 0x80) {
				if (payloadSize <= descriptorSize)
					return false;
				const std::uint8_t pictureId = std::to_integer<std::uint8_t>(payload[descriptorSize++]);
				if (pictureId & 0x80) {
					if (payloadSize <= descriptorSize)
						return false;
					++descriptorSize;
				}
			}
			if (extension & 0x40) {
				if (payloadSize <= descriptorSize)
					return false;
				++descriptorSize;
			}
			if (extension & 0x30) {
				if (payloadSize <= descriptorSize)
					return false;
				++descriptorSize;
			}
		}
		if (payloadSize <= descriptorSize)
			return false;

		result.timestamp = header->timestamp();
		result.sequence = header->seqNumber();
		result.payloadType = header->payloadType();
		result.start = (descriptor & 0x10) != 0 && (descriptor & 0x0f) == 0;
		result.marker = header->marker();
		result.payload.assign(payload + descriptorSize, payload + payloadSize);
		if (!reportedPacket_) {
			reportedPacket_ = true;
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] First VP8 RTP packet: bytes=" << message->size()
					     << "header=" << headerSize << "descriptor=" << descriptorSize
					     << "marker=" << result.marker << "ssrc=" << header->ssrc();
		}
		return true;
	}

	rtc::message_ptr assembleIfComplete(const FrameState &state)
	{
		if (!state.hasStart || !state.hasMarker)
			return nullptr;
		const std::uint32_t packetCount =
			static_cast<std::uint16_t>(state.markerSequence - state.startSequence) + 1u;
		constexpr std::uint32_t maximumPacketsPerFrame = 2048;
		if (packetCount > maximumPacketsPerFrame || state.payloads.size() < packetCount)
			return nullptr;

		std::size_t frameSize = 0;
		for (std::uint32_t index = 0; index < packetCount; ++index) {
			const std::uint16_t sequence = static_cast<std::uint16_t>(state.startSequence + index);
			const auto found = state.payloads.find(sequence);
			if (found == state.payloads.end())
				return nullptr;
			frameSize += found->second.size();
		}
		rtc::binary frame;
		frame.reserve(frameSize);
		for (std::uint32_t index = 0; index < packetCount; ++index) {
			const std::uint16_t sequence = static_cast<std::uint16_t>(state.startSequence + index);
			const rtc::binary &payload = state.payloads.at(sequence);
			frame.insert(frame.end(), payload.begin(), payload.end());
		}

		auto info = std::make_shared<rtc::FrameInfo>(state.timestamp);
		info->payloadType = state.payloadType;
		const std::uint64_t assembled = ++assembledFrames_;
		if (assembled == 1 || assembled % 100 == 0)
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Assembled" << assembled
					     << "complete VP8 frames; latest=" << frame.size() << "bytes from"
					     << packetCount << "RTP packets";
		return rtc::make_message(std::move(frame), std::move(info));
	}

	void removeFrame(std::uint32_t timestamp)
	{
		frames_.erase(timestamp);
		frameOrder_.erase(std::remove(frameOrder_.begin(), frameOrder_.end(), timestamp), frameOrder_.end());
	}

	void drainReadyFrames(rtc::message_vector &output)
	{
		while (!frameOrder_.empty()) {
			const auto found = frames_.find(frameOrder_.front());
			if (found == frames_.end()) {
				frameOrder_.erase(frameOrder_.begin());
				continue;
			}
			if (!found->second.completeFrame)
				break;
			output.push_back(std::move(found->second.completeFrame));
			removeFrame(found->first);
		}
	}

	void pruneOldFrames(rtc::message_vector &output)
	{
		constexpr std::size_t maximumBufferedFrames = 16;
		while (frameOrder_.size() > maximumBufferedFrames) {
			const std::uint32_t timestamp = frameOrder_.front();
			frameOrder_.erase(frameOrder_.begin());
			const auto found = frames_.find(timestamp);
			if (found != frames_.end() && !found->second.completeFrame) {
				const std::uint64_t dropped = ++droppedFrames_;
				if (dropped == 1 || dropped % 120 == 0)
					ObsLogLine(LOG_WARNING)
						<< "[nextcloud-talk] Dropped incomplete VP8 frame after reorder window; count="
						<< dropped;
			}
			frames_.erase(timestamp);
			drainReadyFrames(output);
		}
	}

	std::unordered_map<std::uint32_t, FrameState> frames_;
	std::vector<std::uint32_t> frameOrder_;
	std::uint64_t assembledFrames_ = 0;
	std::uint64_t droppedFrames_ = 0;
	bool reportedPacket_ = false;
};

class RtpVideoClock final {
public:
	void restart()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		initialized_ = false;
		lastArrivalTimestampNs_ = 0;
	}

	std::uint64_t map(std::uint32_t rtpTimestamp)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const std::uint64_t now = os_gettime_ns();
		// These are separate OBS video and audio sources. Keeping an extra
		// timestamped video queue here cannot synchronize them and only delays
		// camera recovery, so anchor decoded video at its current playout time.
		constexpr std::uint64_t playoutDelayNs = 0;
		constexpr std::uint64_t maximumLatenessNs = 200'000'000ULL;
		constexpr std::uint64_t restartGapNs = 500'000'000ULL;
		if (initialized_ && lastArrivalTimestampNs_ != 0 && now - lastArrivalTimestampNs_ > restartGapNs) {
			initialized_ = false;
			const std::uint64_t resets = ++resets_;
			if (resets == 1 || resets % 30 == 0)
				ObsLogLine(LOG_INFO)
					<< "[nextcloud-talk] Restarting incoming video playout after an RTP gap; count="
					<< resets;
		}
		if (!initialized_) {
			initialized_ = true;
			baseRtpTimestamp_ = rtpTimestamp;
			baseOutputTimestampNs_ = now + playoutDelayNs;
		}
		lastArrivalTimestampNs_ = now;

		const std::uint32_t rtpDelta = rtpTimestamp - baseRtpTimestamp_;
		std::uint64_t output = baseOutputTimestampNs_ +
			static_cast<std::uint64_t>(rtpDelta) * 1'000'000'000ULL / 90'000ULL;
		if (output + maximumLatenessNs < now || output > now + 2'000'000'000ULL) {
			baseRtpTimestamp_ = rtpTimestamp;
			baseOutputTimestampNs_ = now + playoutDelayNs;
			output = baseOutputTimestampNs_;
			const std::uint64_t resets = ++resets_;
			if (resets == 1 || resets % 30 == 0)
				ObsLogLine(LOG_WARNING)
					<< "[nextcloud-talk] Resynchronised incoming video playout clock; count="
					<< resets;
		}
		if (lastOutputTimestampNs_ != 0 && output <= lastOutputTimestampNs_)
			output = lastOutputTimestampNs_ + 1;
		lastOutputTimestampNs_ = output;
		return output;
	}

private:
	std::mutex mutex_;
	bool initialized_ = false;
	std::uint32_t baseRtpTimestamp_ = 0;
	std::uint64_t baseOutputTimestampNs_ = 0;
	std::uint64_t lastOutputTimestampNs_ = 0;
	std::uint64_t lastArrivalTimestampNs_ = 0;
	std::uint64_t resets_ = 0;
};

class Vp8RtpPacketizer final : public rtc::RtpPacketizer {
public:
	explicit Vp8RtpPacketizer(const std::shared_ptr<rtc::RtpPacketizationConfig> &config,
				 std::size_t maximumFragmentSize = 1200)
		: rtc::RtpPacketizer(config), maximumFragmentSize_(std::max<std::size_t>(2, maximumFragmentSize))
	{
	}

protected:
	std::vector<rtc::binary> fragment(rtc::binary frame) override
	{
		std::vector<rtc::binary> fragments;
		const std::size_t payloadCapacity = maximumFragmentSize_ - 1;
		for (std::size_t offset = 0; offset < frame.size(); offset += payloadCapacity) {
			const std::size_t count = std::min(payloadCapacity, frame.size() - offset);
			rtc::binary payload(count + 1);
			payload.front() = offset == 0 ? std::byte{0x10} : std::byte{0x00};
			std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(offset), count, payload.begin() + 1);
			fragments.push_back(std::move(payload));
		}
		return fragments;
	}

private:
	std::size_t maximumFragmentSize_;
};

class CodecRtpDepacketizer final : public rtc::MediaHandler {
public:
	explicit CodecRtpDepacketizer(std::unordered_map<std::uint8_t, IncomingCodec> codecs,
				       std::unordered_map<std::uint8_t, std::uint8_t> rtxPayloadTypes,
				       int audioLevelExtensionId = 0,
				       std::function<void(bool)> onSpeakingChanged = {})
		: codecs_(std::move(codecs)), h264_(std::make_shared<rtc::H264RtpDepacketizer>(
						     rtc::H264RtpDepacketizer::Separator::StartSequence)),
		  vp8_(std::make_shared<Vp8RtpDepacketizer>()), opus_(std::make_shared<rtc::RtpDepacketizer>(48000)),
		  rtxPayloadTypes_(std::move(rtxPayloadTypes)), audioLevelExtensionId_(audioLevelExtensionId),
		  onSpeakingChanged_(std::move(onSpeakingChanged))
	{
	}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override
	{
		if (videoRestartRequested_.exchange(false)) {
			videoSsrcSelected_ = false;
			reportedAlternateVideoSsrc_ = false;
			vp8_->reset();
		}
		rtc::message_vector output;
		for (const auto &message : messages) {
			if (!message || message->type != rtc::Message::Binary || message->size() < sizeof(rtc::RtpHeader))
				continue;
			const auto &packet = static_cast<const rtc::binary &>(*message);
			if (rtc::IsRtcp(packet))
				continue;
			rtc::message_ptr mediaMessage = message;
			const auto *header = reinterpret_cast<const rtc::RtpHeader *>(mediaMessage->data());
			const auto rtx = rtxPayloadTypes_.find(header->payloadType());
			if (rtx != rtxPayloadTypes_.end()) {
				if (!videoSsrcSelected_)
					continue;
				const std::size_t headerSize = header->getSize() + header->getExtensionHeaderSize();
				if (mediaMessage->size() < headerSize + 2)
					continue;
				auto normalized = std::make_shared<rtc::Message>(*mediaMessage);
				auto *rtxPacket = reinterpret_cast<rtc::RtpRtx *>(normalized->data());
				const std::size_t normalizedSize =
					rtxPacket->normalizePacket(normalized->size(), videoSsrc_, rtx->second);
				if (normalizedSize < sizeof(rtc::RtpHeader) || normalizedSize > normalized->size())
					continue;
				normalized->resize(normalizedSize);
				mediaMessage = std::move(normalized);
				header = reinterpret_cast<const rtc::RtpHeader *>(mediaMessage->data());
				const std::uint64_t recovered = ++recoveredRtxPackets_;
				if (recovered == 1 || recovered % 250 == 0)
					ObsLogLine(LOG_INFO) << "[nextcloud-talk] Recovered" << recovered
							     << "VP8 RTP packets from RTX";
			}
			const auto found = codecs_.find(header->payloadType());
			if (found == codecs_.end() || found->second == IncomingCodec::Unsupported)
				continue;
			if (found->second == IncomingCodec::Opus)
				observeAudioLevel(mediaMessage);
			if (found->second == IncomingCodec::H264 || found->second == IncomingCodec::Vp8) {
				if (!videoSsrcSelected_) {
					videoSsrcSelected_ = true;
					videoSsrc_ = header->ssrc();
					ObsLogLine(LOG_INFO) << "[nextcloud-talk] Selected incoming video SSRC"
							     << videoSsrc_;
				} else if (header->ssrc() != videoSsrc_) {
					if (!reportedAlternateVideoSsrc_) {
						reportedAlternateVideoSsrc_ = true;
						ObsLogLine(LOG_INFO)
							<< "[nextcloud-talk] Ignoring alternate simulcast video SSRC"
							<< header->ssrc();
					}
					continue;
				}
			}
			rtc::message_vector one{mediaMessage};
			switch (found->second) {
			case IncomingCodec::H264:
				h264_->incoming(one, send);
				break;
			case IncomingCodec::Vp8:
				vp8_->incoming(one, send);
				break;
			case IncomingCodec::Opus:
				opus_->incoming(one, send);
				break;
			case IncomingCodec::Unsupported:
				one.clear();
				break;
			}
			for (auto &frame : one)
				output.push_back(std::move(frame));
		}
		messages = std::move(output);
	}

	void restartVideo() { videoRestartRequested_.store(true); }

private:
	void observeAudioLevel(const rtc::message_ptr &message)
	{
		const std::optional<std::uint8_t> value = rtpExtensionValue(message, audioLevelExtensionId_);
		if (!value)
			return;
		const std::uint8_t level = *value & 0x7f;
		const bool voiceActivity = (*value & 0x80) != 0 || level <= 55;
		const std::uint64_t now = os_gettime_ns();
		constexpr std::uint64_t speakingHoldNs = 300'000'000ULL;
		if (voiceActivity)
			lastVoiceActivityNs_ = now;
		const bool speaking = lastVoiceActivityNs_ != 0 && now - lastVoiceActivityNs_ <= speakingHoldNs;
		if (speaking == speaking_)
			return;
		speaking_ = speaking;
		if (onSpeakingChanged_)
			onSpeakingChanged_(speaking);
	}

	std::unordered_map<std::uint8_t, IncomingCodec> codecs_;
	std::shared_ptr<rtc::MediaHandler> h264_;
	std::shared_ptr<Vp8RtpDepacketizer> vp8_;
	std::shared_ptr<rtc::MediaHandler> opus_;
	std::unordered_map<std::uint8_t, std::uint8_t> rtxPayloadTypes_;
	int audioLevelExtensionId_ = 0;
	std::function<void(bool)> onSpeakingChanged_;
	std::uint64_t lastVoiceActivityNs_ = 0;
	bool speaking_ = false;
	std::uint32_t videoSsrc_ = 0;
	bool videoSsrcSelected_ = false;
	bool reportedAlternateVideoSsrc_ = false;
	std::atomic_bool videoRestartRequested_{false};
	std::uint64_t recoveredRtxPackets_ = 0;
};

} // namespace

TalkMediaTransport::TalkMediaTransport(QObject *parent) : QObject(parent)
{
	rtcConfiguration_ = new rtc::Configuration;
	e2ee_ = std::make_unique<TalkE2ee>();
	e2ee_->sendCallMessage = [this](const QString &recipientId, const QJsonObject &payload) {
		QJsonObject recipient{{QStringLiteral("type"), QStringLiteral("session")},
				      {QStringLiteral("sessionid"), recipientId}};
		QJsonObject data{{QStringLiteral("type"), QStringLiteral("message")},
				 {QStringLiteral("to"), recipientId},
				 {QStringLiteral("payload"), payload}};
		sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
				     {QStringLiteral("message"), QJsonObject{{QStringLiteral("recipient"), recipient},
								       {QStringLiteral("data"), data}}}});
	};
	outgoingMedia_.onPacket = [this](EncodedMediaPacket packet) { sendEncodedPacket(std::move(packet)); };
}

TalkMediaTransport::~TalkMediaTransport()
{
	stop();
	delete rtcConfiguration_;
}

void TalkMediaTransport::start(const QString &nextcloudUrl, const QString &roomToken,
			       const QString &roomSessionId, const SignalingSettings &settings,
			       const OutgoingMediaSelection &outgoingSelection)
{
	stop();
	nextcloudUrl_ = nextcloudUrl;
	roomToken_ = roomToken;
	roomSessionId_ = roomSessionId;
	settings_ = settings;
	signalingSessionId_.clear();
	hasMcu_ = false;
	serverFeatures_.clear();
	remoteParticipantIds_.clear();
	nextMessageId_ = 1;
	helloSent_ = false;
	outgoingVideoSeen_ = false;
	outgoingAudioSeen_ = false;
	outgoingVideoSent_ = false;
	outgoingAudioSent_ = false;
	publishingReadyNotified_ = false;
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Media transport requested: mode=" << settings.mode
			  << "roomSessionPresent=" << !roomSessionId.isEmpty() << "outgoingMode="
			  << outgoingMediaModeId(outgoingSelection.mode) << "video="
			  << outgoingSelection.videoSourceName << "audio=" << outgoingSelection.audioSourceName
			  << "scene=" << outgoingSelection.sceneName;

	if (settings.mode.compare(QStringLiteral("external"), Qt::CaseInsensitive) != 0) {
		postStatus(QStringLiteral("In call — internal Talk signaling is not yet supported by the media transport"));
		return;
	}
	if (settings.server.isEmpty() || roomSessionId.isEmpty() || settings.helloAuthV1.isEmpty()) {
		postStatus(QStringLiteral("In call — signaling settings are incomplete"));
		return;
	}

	configureIce(settings);
	QString captureError;
	if (!outgoingMedia_.start(outgoingSelection, captureError)) {
		postStatus(QStringLiteral("In call — outgoing capture failed: %1").arg(captureError));
		return;
	}
	QString websocketUrl = settings.server.trimmed();
	if (websocketUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
		websocketUrl.replace(0, 8, QStringLiteral("wss://"));
	else if (websocketUrl.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive))
		websocketUrl.replace(0, 7, QStringLiteral("ws://"));
	while (websocketUrl.endsWith('/'))
		websocketUrl.chop(1);
	websocketUrl += QStringLiteral("/spreed");

	try {
		auto socket = std::make_shared<WinHttpWebSocket>();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			websocket_ = socket;
		}
		active_ = true;
		socket->onOpen = [this] {
			if (!active_)
				return;
			postStatus(QStringLiteral("In call — media signaling socket connected…"));
			QMetaObject::invokeMethod(this, [this] {
				QTimer::singleShot(500, this, [this] {
					if (active_ && !helloSent_.exchange(true)) {
						ObsLogLine(LOG_INFO) << "[nextcloud-talk] No welcome received before timeout; sending hello";
						postStatus(QStringLiteral("In call — authenticating media transport…"));
						sendHello();
					}
				});
			}, Qt::QueuedConnection);
		};
		socket->onTextMessage = [this](const QByteArray &message) {
			if (active_)
				handleTextMessage(std::string(message.constData(), static_cast<std::size_t>(message.size())));
		};
		socket->onError = [this](const QString &error) {
			if (active_)
				postStatus(QStringLiteral("In call — signaling error: %1").arg(error));
		};
		socket->onClosed = [this] {
			if (active_)
				postStatus(QStringLiteral("In call — signaling connection closed"));
		};
		postStatus(QStringLiteral("In call — connecting media signaling…"));
		socket->open(websocketUrl);
	} catch (const std::exception &error) {
		active_ = false;
		postStatus(QStringLiteral("In call — could not start signaling: %1")
				   .arg(QString::fromUtf8(error.what())));
	}
}

void TalkMediaTransport::stop()
{
	outgoingMedia_.stop();
	if (e2ee_)
		e2ee_->stop();
	if (active_)
		sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("bye")},
				     {QStringLiteral("bye"), QJsonObject{}}});
	active_ = false;
	std::shared_ptr<WinHttpWebSocket> socket;
	std::vector<std::shared_ptr<PeerSession>> peers;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		socket = std::move(websocket_);
		for (auto &entry : peers_)
			peers.push_back(std::move(entry.second));
		peers_.clear();
		requestedOffers_.clear();
		initiatedPeers_.clear();
		serverFeatures_.clear();
		remotelyStoppedVideo_.clear();
		remoteParticipantIds_.clear();
	}
	for (const auto &peer : peers) {
		if (peer && peer->connection) {
			for (const auto &track : peer->tracks) {
				if (track)
					track->resetCallbacks();
			}
			for (const auto &channel : peer->dataChannels) {
				if (channel)
					channel->resetCallbacks();
			}
			peer->connection->resetCallbacks();
			peer->connection->close();
		}
	}
	if (socket) {
		socket->close();
	}
	signalingSessionId_.clear();
}

void TalkMediaTransport::postStatus(const QString &status)
{
	QMetaObject::invokeMethod(this, [this, status] {
		ObsLogLine(LOG_INFO) << "[nextcloud-talk]" << status;
		if (onStatusChanged)
			onStatusChanged(status);
	}, Qt::QueuedConnection);
}

void TalkMediaTransport::postParticipantMediaState(ParticipantMediaState state)
{
	if (state.participantId.isEmpty())
		return;
	QMetaObject::invokeMethod(this, [this, state = std::move(state)] {
		if (active_ && onParticipantMediaState)
			onParticipantMediaState(state);
	}, Qt::QueuedConnection);
}

void TalkMediaTransport::handleRemoteStatusMessage(const QString &participantId, const QJsonObject &message)
{
	if (participantId.isEmpty())
		return;
	const QString type = message.value(QStringLiteral("type")).toString();
	ParticipantMediaState state;
	state.participantId = participantId;
	if (type == QStringLiteral("audioOn"))
		state.audioEnabled = true;
	else if (type == QStringLiteral("audioOff"))
		state.audioEnabled = false;
	else if (type == QStringLiteral("videoOn"))
		state.videoEnabled = true;
	else if (type == QStringLiteral("videoOff"))
		state.videoEnabled = false;
	else if (type == QStringLiteral("speaking"))
		state.speaking = true;
	else if (type == QStringLiteral("stoppedSpeaking"))
		state.speaking = false;
	else if (type == QStringLiteral("mute") || type == QStringLiteral("unmute")) {
		const QString media = message.value(QStringLiteral("payload")).toObject()
					      .value(QStringLiteral("name")).toString();
		const bool enabled = type == QStringLiteral("unmute");
		if (media == QStringLiteral("audio"))
			state.audioEnabled = enabled;
		else if (media == QStringLiteral("video"))
			state.videoEnabled = enabled;
	}
	if (state.videoEnabled) {
		std::vector<std::function<void()>> restartCallbacks;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const std::string participantKey = utf8(participantId);
			const bool restart = *state.videoEnabled && remotelyStoppedVideo_.erase(participantKey) != 0;
			if (!*state.videoEnabled)
				remotelyStoppedVideo_.insert(participantKey);
			if (restart) {
				for (const auto &entry : peers_) {
					const auto &peer = entry.second;
					if (peer && !peer->publishesLocalMedia && peer->roomType != QStringLiteral("screen") &&
					    peer->participantId == participantId)
						restartCallbacks.insert(restartCallbacks.end(), peer->restartVideoPipelines.begin(),
								peer->restartVideoPipelines.end());
				}
			}
		}
		if (!restartCallbacks.empty()) {
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Restarting incoming video pipeline for"
					     << participantId;
			for (const auto &restart : restartCallbacks)
				restart();
		}
	}
	if (state.speaking)
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Remote speaking state" << participantId
				     << (*state.speaking ? "speaking" : "not speaking");
	if (state.audioEnabled || state.videoEnabled || state.speaking)
		postParticipantMediaState(std::move(state));
}

void TalkMediaTransport::observeDataChannel(const std::shared_ptr<PeerSession> &peer,
					    const std::shared_ptr<rtc::DataChannel> &channel,
					    bool statusChannel)
{
	if (!peer || !channel)
		return;
	const QString label = fromUtf8(channel->label());
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (std::find(peer->dataChannels.begin(), peer->dataChannels.end(), channel) == peer->dataChannels.end())
			peer->dataChannels.push_back(channel);
		if (statusChannel && !peer->statusDataChannel)
			peer->statusDataChannel = channel;
	}
	channel->onMessage(
		[this, peer, label](rtc::binary payload) {
			handleDataChannelPayload(peer, label,
				QByteArray(reinterpret_cast<const char *>(payload.data()),
					   static_cast<qsizetype>(payload.size())));
		},
		[this, peer, label](std::string payload) {
			handleDataChannelPayload(
				peer, label, QByteArray(payload.data(), static_cast<qsizetype>(payload.size())));
		});
	channel->onOpen([this, peer, label] {
		QString participantId;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			participantId = peer->participantId;
		}
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Data channel opened:" << label << "for"
				     << participantId;
	});
}

void TalkMediaTransport::handleDataChannelPayload(const std::shared_ptr<PeerSession> &peer,
						  const QString &label, const QByteArray &payload)
{
	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject())
		return;
	QString participantId;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		participantId = peer->participantId;
	}
	const QJsonObject message = document.object();
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Data channel message:" << label
			     << message.value(QStringLiteral("type")).toString() << "from" << participantId;
	handleRemoteStatusMessage(participantId, message);
}

void TalkMediaTransport::sendJson(QJsonObject message, bool addId)
{
	if (addId && !message.contains(QStringLiteral("id")))
		message.insert(QStringLiteral("id"), QString::number(nextMessageId_++));
	const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
	std::shared_ptr<WinHttpWebSocket> socket;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		socket = websocket_;
	}
	if (active_ && socket && socket->isOpen())
		socket->sendText(json);
}

void TalkMediaTransport::sendHello()
{
	QString backendUrl = nextcloudUrl_;
	while (backendUrl.endsWith('/'))
		backendUrl.chop(1);
	backendUrl += QStringLiteral("/ocs/v2.php/apps/spreed/api/v3/signaling/backend");

	QJsonObject auth;
	auth.insert(QStringLiteral("url"), backendUrl);
	auth.insert(QStringLiteral("params"), settings_.helloAuthV1);
	QJsonObject hello;
	hello.insert(QStringLiteral("version"), QStringLiteral("1.0"));
	hello.insert(QStringLiteral("features"), QJsonArray{QStringLiteral("chat-relay")});
	hello.insert(QStringLiteral("auth"), auth);
	sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("hello")},
			     {QStringLiteral("hello"), hello}});
}

void TalkMediaTransport::sendRoomJoin()
{
	QJsonObject room{{QStringLiteral("roomid"), roomToken_},
			 {QStringLiteral("sessionid"), roomSessionId_}};
	sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("room")},
			     {QStringLiteral("room"), room}});
}

void TalkMediaTransport::handleTextMessage(const std::string &message)
{
	QJsonParseError error;
	const QJsonDocument document =
		QJsonDocument::fromJson(QByteArray(message.data(), static_cast<qsizetype>(message.size())), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Invalid signaling JSON:" << error.errorString();
		return;
	}
	handleMessageObject(document.object());
}

void TalkMediaTransport::handleMessageObject(const QJsonObject &message)
{
	const QString type = message.value(QStringLiteral("type")).toString();
	if (type == QStringLiteral("welcome")) {
		updateServerFeatures(message.value(QStringLiteral("welcome")).toObject()
					     .value(QStringLiteral("features")).toArray());
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Signaling welcome received; MCU=" << hasMcu_;
		if (!helloSent_.exchange(true)) {
			postStatus(QStringLiteral("In call — authenticating media transport…"));
			sendHello();
		}
		return;
	}
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Signaling message received:" << type;
	if (type == QStringLiteral("hello")) {
		const QJsonObject hello = message.value(QStringLiteral("hello")).toObject();
		signalingSessionId_ = hello.value(QStringLiteral("sessionid")).toString();
		if (e2ee_)
			e2ee_->start(signalingSessionId_);
		const QJsonArray features = hello.value(QStringLiteral("server")).toObject()
						.value(QStringLiteral("features")).toArray();
		updateServerFeatures(features);
		if (signalingSessionId_.isEmpty()) {
			postStatus(QStringLiteral("In call — signaling authentication returned no session"));
			return;
		}
		postStatus(QStringLiteral("In call — joining media room…"));
		sendRoomJoin();
		return;
	}
	if (type == QStringLiteral("room")) {
		const QString joinedRoom = message.value(QStringLiteral("room")).toObject()
						   .value(QStringLiteral("roomid")).toString();
		if (joinedRoom == roomToken_) {
			postStatus(hasMcu_ ? QStringLiteral("In call — media signaling connected (MCU)")
					   : QStringLiteral("In call — media signaling connected (peer-to-peer)"));
			if (hasMcu_) {
				// The signaling room acknowledgement can arrive just before the
				// backend has created this session's MCU client. Match the web
				// client by waiting for that registration window before offering.
				QTimer::singleShot(750, this, [this] {
					if (active_)
						startMcuPublisher();
				});
			}
			notifyPublishingReady();
		}
		return;
	}
	if (type == QStringLiteral("event")) {
		handleEvent(message.value(QStringLiteral("event")).toObject());
		return;
	}
	if (type == QStringLiteral("message")) {
		const QJsonObject envelope = message.value(QStringLiteral("message")).toObject();
		const QString sender = envelope.value(QStringLiteral("sender")).toObject()
						.value(QStringLiteral("sessionid")).toString();
		handleRtcMessage(sender, envelope.value(QStringLiteral("data")).toObject());
		return;
	}
	if (type == QStringLiteral("error")) {
		const QJsonObject error = message.value(QStringLiteral("error")).toObject();
		const QString code = error.value(QStringLiteral("code")).toString(QStringLiteral("unknown error"));
		const QString explanation = error.value(QStringLiteral("message")).toString();
		postStatus(explanation.isEmpty()
				   ? QStringLiteral("In call — signaling rejected the request: %1").arg(code)
				   : QStringLiteral("In call — signaling rejected the request: %1 (%2)")
					     .arg(code, explanation));
	}
}

void TalkMediaTransport::handleEvent(const QJsonObject &event)
{
	const QString target = event.value(QStringLiteral("target")).toString();
	const QString eventType = event.value(QStringLiteral("type")).toString();
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Signaling event:" << target << eventType;
	if (target == QStringLiteral("room") && eventType == QStringLiteral("leave")) {
		for (const QJsonValue &value : event.value(QStringLiteral("leave")).toArray()) {
			const QString sessionId = value.isString()
						  ? value.toString()
						  : value.toObject().value(QStringLiteral("sessionid")).toString();
			resetRemoteSession(sessionId, true);
		}
		return;
	}
	if (target == QStringLiteral("participants") && eventType == QStringLiteral("flags")) {
		const QJsonValue flagsValue = event.value(QStringLiteral("flags"));
		const QJsonArray flagUpdates = flagsValue.isArray() ? flagsValue.toArray() : QJsonArray{flagsValue};
		for (const QJsonValue &value : flagUpdates) {
			const QJsonObject update = value.toObject();
			const QString sessionId = update.value(QStringLiteral("sessionid")).toString(
				update.value(QStringLiteral("sessionId")).toString());
			QString participantId;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				const auto identity = remoteParticipantIds_.find(utf8(sessionId));
				if (identity != remoteParticipantIds_.end())
					participantId = identity->second;
			}
			if (participantId.isEmpty())
				continue;
			const int flags = update.value(QStringLiteral("flags")).toInt();
			ParticipantMediaState state;
			state.participantId = participantId;
			state.audioEnabled = (flags & 1) == 0;
			state.speaking = (flags & 4) != 0;
			postParticipantMediaState(std::move(state));
		}
		return;
	}
	if (target != QStringLiteral("participants") || eventType != QStringLiteral("update"))
		return;
	const QJsonValue updateValue = event.value(QStringLiteral("update"));
	const QJsonArray updates = updateValue.isArray() ? updateValue.toArray() : QJsonArray{updateValue};
	for (const QJsonValue &value : updates) {
		const QJsonObject update = value.toObject();
		if (update.value(QStringLiteral("all")).toBool() &&
		    (update.value(QStringLiteral("incall")).toInt() & 1) == 0) {
			std::vector<QString> sessions;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				for (const auto &entry : remoteParticipantIds_)
					sessions.push_back(fromUtf8(entry.first));
			}
			for (const QString &sessionId : sessions)
				resetRemoteSession(sessionId, true);
			continue;
		}
		QJsonArray users = update.value(QStringLiteral("users")).toArray();
		if (users.isEmpty())
			users = update.value(QStringLiteral("changed")).toArray();
		handleParticipants(users);
	}
}

void TalkMediaTransport::handleParticipants(const QJsonArray &users)
{
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Signaling participant update:" << users.size()
			  << "entries; MCU=" << hasMcu_;
	for (const QJsonValue &value : users) {
		const QJsonObject user = value.toObject();
		const QString sessionId = user.value(QStringLiteral("sessionId")).toString(
			user.value(QStringLiteral("sessionid")).toString());
		if (sessionId.isEmpty() || sessionId == signalingSessionId_)
			continue;
		const QString stableId = participantStableId(user, sessionId);
		if (user.contains(QStringLiteral("inCall"))) {
			const int flags = user.value(QStringLiteral("inCall")).toInt();
			if ((flags & 1) == 0) {
				pauseRemoteSession(sessionId);
				continue;
			}
		}

		// A browser rejoin can create a new signaling session for the same Talk
		// actor. Retire its old subscriber before binding the stable OBS sources
		// to the replacement session.
		std::vector<QString> supersededSessions;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (const auto &entry : remoteParticipantIds_) {
				if (entry.second == stableId && fromUtf8(entry.first) != sessionId)
					supersededSessions.push_back(fromUtf8(entry.first));
			}
		}
		for (const QString &superseded : supersededSessions)
			resetRemoteSession(superseded, true);

		if (e2ee_)
			e2ee_->participantJoined(sessionId);
		bool screenShareActive = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			remoteParticipantIds_[utf8(sessionId)] = stableId;
			for (auto &entry : peers_) {
				if (entry.second->remoteSessionId == sessionId) {
					entry.second->participantId = stableId;
					if (entry.second->roomType == QStringLiteral("screen"))
						screenShareActive = true;
				}
			}
		}
		if (screenShareActive) {
			ParticipantMediaState state;
			state.participantId = stableId;
			state.screenSharing = true;
			postParticipantMediaState(std::move(state));
		}
		const bool remotePublishes = hasPublishedMedia(user.value(QStringLiteral("inCall")));
		if (hasMcu_) {
			if (remotePublishes)
				requestOffer(sessionId);
			else
				pauseRemoteSession(sessionId);
		} else if (!remotePublishes || sessionId < signalingSessionId_) {
			offerToParticipant(sessionId);
		}
	}
}

void TalkMediaTransport::pauseRemoteSession(const QString &sessionId)
{
	if (sessionId.isEmpty() || sessionId == signalingSessionId_)
		return;
	if (!hasMcu_) {
		resetRemoteSession(sessionId, false);
		return;
	}

	QString participantId;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const std::string sessionKey = utf8(sessionId);
		requestedOffers_.erase(sessionKey);
		const auto identity = remoteParticipantIds_.find(sessionKey);
		if (identity != remoteParticipantIds_.end())
			participantId = identity->second;
	}
	if (!participantId.isEmpty())
		setParticipantMediaOnline(participantId, false);
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Paused MCU media session" << sessionId.left(8)
				  << "and enabled offer refresh";
}

void TalkMediaTransport::resetRemoteStream(const QString &sessionId, const QString &roomType)
{
	if (sessionId.isEmpty())
		return;
	QString participantId;
	std::vector<std::shared_ptr<PeerSession>> peers;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto identity = remoteParticipantIds_.find(utf8(sessionId));
		if (identity != remoteParticipantIds_.end())
			participantId = identity->second;
		for (auto iterator = peers_.begin(); iterator != peers_.end();) {
			if (iterator->second && iterator->second->remoteSessionId == sessionId &&
			    iterator->second->roomType == roomType) {
				peers.push_back(std::move(iterator->second));
				iterator = peers_.erase(iterator);
			} else {
				++iterator;
			}
		}
	}
	for (const auto &peer : peers) {
		for (const auto &track : peer->tracks) {
			if (track)
				track->resetCallbacks();
		}
		for (const auto &channel : peer->dataChannels) {
			if (channel)
				channel->resetCallbacks();
		}
		peer->connection->resetCallbacks();
		peer->connection->close();
	}
	if (roomType == QStringLiteral("screen") && !participantId.isEmpty())
		setParticipantScreenOnline(participantId, false);
	if (roomType == QStringLiteral("screen") && !participantId.isEmpty()) {
		ParticipantMediaState state;
		state.participantId = participantId;
		state.screenSharing = false;
		postParticipantMediaState(std::move(state));
	}
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Retired remote" << roomType << "stream for"
				  << sessionId.left(8) << "peers=" << peers.size();
}

void TalkMediaTransport::resetRemoteSession(const QString &sessionId, bool participantLeft)
{
	if (sessionId.isEmpty() || sessionId == signalingSessionId_)
		return;

	const std::string sessionKey = utf8(sessionId);
	QString participantId;
	std::vector<std::shared_ptr<PeerSession>> peers;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto identity = remoteParticipantIds_.find(sessionKey);
		if (identity != remoteParticipantIds_.end()) {
			participantId = identity->second;
			if (participantLeft)
				remoteParticipantIds_.erase(identity);
		}
		requestedOffers_.erase(sessionKey);
		initiatedPeers_.erase(sessionKey);
		for (auto iterator = peers_.begin(); iterator != peers_.end();) {
			if (iterator->second && iterator->second->remoteSessionId == sessionId) {
				peers.push_back(std::move(iterator->second));
				iterator = peers_.erase(iterator);
			} else {
				++iterator;
			}
		}
	}

	for (const auto &peer : peers) {
		if (!peer || !peer->connection)
			continue;
		for (const auto &track : peer->tracks) {
			if (track)
				track->resetCallbacks();
		}
		for (const auto &channel : peer->dataChannels) {
			if (channel)
				channel->resetCallbacks();
		}
		peer->connection->resetCallbacks();
		peer->connection->close();
	}

	if (participantLeft && e2ee_)
		e2ee_->participantLeft(sessionId);
	if (!participantId.isEmpty())
		setParticipantMediaOnline(participantId, false);
	if (!participantId.isEmpty()) {
		setParticipantScreenOnline(participantId, false);
		ParticipantMediaState state;
		state.participantId = participantId;
		state.screenSharing = false;
		postParticipantMediaState(std::move(state));
	}
	if (!peers.empty() || participantLeft)
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Retired remote media session"
					  << sessionId.left(8) << "peers=" << peers.size();
}

void TalkMediaTransport::retireOtherPeers(const std::shared_ptr<PeerSession> &activePeer)
{
	if (!activePeer || activePeer->publishesLocalMedia)
		return;
	std::vector<std::shared_ptr<PeerSession>> retired;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto iterator = peers_.begin(); iterator != peers_.end();) {
			const auto &candidate = iterator->second;
			if (candidate && candidate != activePeer && !candidate->publishesLocalMedia &&
			    candidate->remoteSessionId == activePeer->remoteSessionId &&
			    candidate->roomType == activePeer->roomType) {
				retired.push_back(std::move(iterator->second));
				iterator = peers_.erase(iterator);
			} else {
				++iterator;
			}
		}
	}
	for (const auto &peer : retired) {
		for (const auto &track : peer->tracks) {
			if (track)
				track->resetCallbacks();
		}
		for (const auto &channel : peer->dataChannels) {
			if (channel)
				channel->resetCallbacks();
		}
		peer->connection->resetCallbacks();
		peer->connection->close();
	}
}

void TalkMediaTransport::sendLocalMediaState(const QString &sessionId)
{
	if (!sessionId.isEmpty()) {
		const auto sendState = [this, &sessionId](const QString &type, const QJsonObject &payload) {
			QJsonObject data{{QStringLiteral("to"), sessionId},
					 {QStringLiteral("roomType"), QStringLiteral("video")},
					 {QStringLiteral("type"), type},
					 {QStringLiteral("payload"), payload}};
			QJsonObject recipient{{QStringLiteral("type"), QStringLiteral("session")},
					      {QStringLiteral("sessionid"), sessionId}};
			sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
					     {QStringLiteral("message"),
					      QJsonObject{{QStringLiteral("recipient"), recipient},
							  {QStringLiteral("data"), data}}}});
		};
		sendState(QStringLiteral("unmute"), QJsonObject{{QStringLiteral("name"), QStringLiteral("audio")}});
		sendState(QStringLiteral("unmute"), QJsonObject{{QStringLiteral("name"), QStringLiteral("video")}});
		sendState(QStringLiteral("nickChanged"), QJsonObject{{QStringLiteral("name"), settings_.userId}});
	}

	std::vector<std::shared_ptr<rtc::DataChannel>> channels;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &entry : peers_) {
			if (entry.second && entry.second->statusDataChannel)
				channels.push_back(entry.second->statusDataChannel);
		}
	}
	for (const auto &channel : channels) {
		if (!channel || !channel->isOpen())
			continue;
		channel->send(std::string(R"({"type":"audioOn"})"));
		channel->send(std::string(R"({"type":"videoOn"})"));
		const QJsonObject nickname{{QStringLiteral("type"), QStringLiteral("nickChanged")},
					    {QStringLiteral("payload"),
					     QJsonObject{{QStringLiteral("name"), settings_.userId}}}};
		channel->send(utf8(QJsonDocument(nickname).toJson(QJsonDocument::Compact)));
	}
}

void TalkMediaTransport::updateServerFeatures(const QJsonArray &features)
{
	for (const QJsonValue &feature : features) {
		const QString value = feature.toString();
		if (!value.isEmpty())
			serverFeatures_.insert(utf8(value));
	}
	hasMcu_ = serverFeatures_.find("mcu") != serverFeatures_.end();
}

void TalkMediaTransport::requestOffer(const QString &sessionId)
{
	const std::string key = utf8(sessionId);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!requestedOffers_.insert(key).second)
			return;
	}
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Requesting MCU subscriber offer for"
			  << sessionId.left(8);
	QJsonObject data{{QStringLiteral("type"), QStringLiteral("requestoffer")},
			 {QStringLiteral("roomType"), QStringLiteral("video")}};
	QJsonObject recipient{{QStringLiteral("type"), QStringLiteral("session")},
			      {QStringLiteral("sessionid"), sessionId}};
	sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
			     {QStringLiteral("message"), QJsonObject{{QStringLiteral("recipient"), recipient},
							       {QStringLiteral("data"), data}}}});
}

void TalkMediaTransport::offerToParticipant(const QString &sessionId)
{
	const std::string key = utf8(sessionId);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!initiatedPeers_.insert(key).second)
			return;
	}
	const QString sid = QUuid::createUuid().toString(QUuid::WithoutBraces);
	try {
		auto peer = createPeer(sessionId, sid, QStringLiteral("video"), true);
		peer->connection->setLocalDescription();
	} catch (const std::exception &error) {
		std::lock_guard<std::mutex> lock(mutex_);
		initiatedPeers_.erase(key);
		ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Could not offer local media:" << error.what();
	}
}

void TalkMediaTransport::startMcuPublisher()
{
	if (signalingSessionId_.isEmpty())
		return;
	const std::string marker = utf8(QStringLiteral("mcu-publisher"));
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!initiatedPeers_.insert(marker).second)
			return;
	}
	try {
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Creating MCU publisher peer";
		const QString sid = QUuid::createUuid().toString(QUuid::WithoutBraces);
		auto peer = createPeer(signalingSessionId_, sid, QStringLiteral("video"), true);
		peer->connection->setLocalDescription();
		postStatus(QStringLiteral("In call — publishing selected camera and microphone to the MCU…"));
	} catch (const std::exception &error) {
		std::lock_guard<std::mutex> lock(mutex_);
		initiatedPeers_.erase(marker);
		postStatus(QStringLiteral("In call — could not start MCU publication: %1")
				   .arg(QString::fromUtf8(error.what())));
	}
}

void TalkMediaTransport::notifyPublishingReady()
{
	if (publishingReadyNotified_.exchange(true))
		return;
	QMetaObject::invokeMethod(this, [this] {
		if (active_ && onPublishingReady)
			onPublishingReady();
	}, Qt::QueuedConnection);
}

void TalkMediaTransport::handleRtcMessage(const QString &sender, const QJsonObject &data)
{
	if (e2ee_ && e2ee_->handleMessage(sender, data))
		return;
	const QString type = data.value(QStringLiteral("type")).toString();
	const QString sid = data.value(QStringLiteral("sid")).toString();
	const QString roomType = data.value(QStringLiteral("roomType")).toString(QStringLiteral("video"));
	if (sender.isEmpty() || (roomType != QStringLiteral("video") && roomType != QStringLiteral("screen")))
		return;
	if (type == QStringLiteral("mute") || type == QStringLiteral("unmute")) {
		QString participantId;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto identity = remoteParticipantIds_.find(utf8(sender));
			if (identity != remoteParticipantIds_.end())
				participantId = identity->second;
		}
		handleRemoteStatusMessage(participantId, data);
		return;
	}
	if (type == QStringLiteral("unshareScreen") && roomType == QStringLiteral("screen")) {
		resetRemoteStream(sender, roomType);
		return;
	}
	if (sid.isEmpty())
		return;
	const std::string key = peerKey(sender, sid, roomType);

	try {
		if (type == QStringLiteral("offer")) {
			auto peer = createPeer(sender, sid, roomType,
					       !hasMcu_ && roomType == QStringLiteral("video"));
			if (roomType == QStringLiteral("screen")) {
				QString participantId;
				{
					std::lock_guard<std::mutex> lock(mutex_);
					participantId = peer->participantId;
				}
				ParticipantMediaState state;
				state.participantId = participantId;
				state.screenSharing = true;
				postParticipantMediaState(std::move(state));
			}
			peer->preferredQualityRequested = false;
			const QJsonObject payload = data.value(QStringLiteral("payload")).toObject();
			const QString remoteSdp = payload.value(QStringLiteral("sdp")).toString();
			peer->connection->setRemoteDescription(rtc::Description(utf8(remoteSdp), "offer"));
			if (!peer->publishesLocalMedia && roomType == QStringLiteral("video") &&
			    remoteSdp.contains(QStringLiteral("m=application"))) {
				// Browser negotiationneeded events are asynchronous, so Talk can
				// create this channel before applying the offer. libdatachannel
				// negotiates immediately; create it only after accepting the
				// remote application m-line to avoid an invalid glare offer.
				observeDataChannel(peer, peer->connection->createDataChannel("status"), true);
			}
			QMetaObject::invokeMethod(this, [this, peer] {
				QTimer::singleShot(500, this, [this, peer] {
					if (active_)
						requestPreferredIncomingQuality(peer);
				});
			}, Qt::QueuedConnection);
			return;
		}

		std::shared_ptr<PeerSession> peer;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto found = peers_.find(key);
			if (found != peers_.end())
				peer = found->second;
		}
		if (!peer)
			return;
		const QJsonObject payload = data.value(QStringLiteral("payload")).toObject();
		if (type == QStringLiteral("answer")) {
			peer->connection->setRemoteDescription(
				rtc::Description(utf8(payload.value(QStringLiteral("sdp")).toString()), "answer"));
		} else if (type == QStringLiteral("candidate")) {
			const QJsonObject candidate = payload.value(QStringLiteral("candidate")).toObject();
			peer->connection->addRemoteCandidate(
				rtc::Candidate(utf8(candidate.value(QStringLiteral("candidate")).toString()),
					       utf8(candidate.value(QStringLiteral("sdpMid")).toString())));
		}
	} catch (const std::exception &error) {
		ObsLogLine(LOG_WARNING) << "[nextcloud-talk] WebRTC signaling failed:" << error.what();
		postStatus(QStringLiteral("In call — WebRTC negotiation failed: %1")
				   .arg(QString::fromUtf8(error.what())));
	}
}

std::shared_ptr<TalkMediaTransport::PeerSession>
TalkMediaTransport::createPeer(const QString &sender, const QString &sid, const QString &roomType,
			       bool publishLocalMedia)
{
	const std::string key = peerKey(sender, sid, roomType);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = peers_.find(key);
		if (found != peers_.end())
			return found->second;
	}

	auto peer = std::make_shared<PeerSession>();
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Creating WebRTC peer for" << sender.left(8)
			  << "publishLocalMedia=" << publishLocalMedia << "MCU=" << hasMcu_;
	peer->remoteSessionId = sender;
	peer->sid = sid;
	peer->roomType = roomType;
	peer->publishesLocalMedia = publishLocalMedia;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto identity = remoteParticipantIds_.find(utf8(sender));
		peer->participantId = identity == remoteParticipantIds_.end() ? sender : identity->second;
	}
	peer->connection = std::make_shared<rtc::PeerConnection>(*rtcConfiguration_);
	peer->connection->onLocalDescription([this, peer](rtc::Description description) {
		const QString sdp = fromUtf8(static_cast<std::string>(description));
		QStringList mediaLines;
		for (const QString &line : sdp.split(QStringLiteral("\r\n"))) {
			if (line.startsWith(QStringLiteral("m=")))
				mediaLines.push_back(line.section(QLatin1Char(' '), 0, 0));
		}
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Local WebRTC" << fromUtf8(description.typeString())
				     << "for" << peer->remoteSessionId.left(8) << "media="
				     << mediaLines.join(QLatin1Char(','));
		QJsonObject payload{{QStringLiteral("type"), fromUtf8(description.typeString())},
				    {QStringLiteral("sdp"), sdp},
				    {QStringLiteral("nick"), settings_.userId}};
		sendPeerMessage(peer, fromUtf8(description.typeString()), payload);
	});
	peer->connection->onLocalCandidate([this, peer](rtc::Candidate candidate) {
		const QString mid = fromUtf8(candidate.mid());
		bool numericMid = false;
		const int parsedMid = mid.toInt(&numericMid);
		const int mLineIndex = numericMid ? parsedMid : (mid == QStringLiteral("video") ? 1 : 0);
		QJsonObject candidateObject{{QStringLiteral("candidate"), fromUtf8(candidate.candidate())},
					{QStringLiteral("sdpMid"), mid},
					{QStringLiteral("sdpMLineIndex"), mLineIndex}};
		sendPeerMessage(peer, QStringLiteral("candidate"),
				QJsonObject{{QStringLiteral("candidate"), candidateObject}});
	});
	peer->connection->onStateChange([this, peer, sender](rtc::PeerConnection::State state) {
		if (state == rtc::PeerConnection::State::Connected) {
			retireOtherPeers(peer);
			postStatus(QStringLiteral("In call — WebRTC connected to participant %1")
					   .arg(sender.left(8)));
			requestPreferredIncomingQuality(peer);
		} else if (state == rtc::PeerConnection::State::Failed) {
			postStatus(QStringLiteral("In call — WebRTC connection failed for participant %1")
					   .arg(sender.left(8)));
		}
	});
	peer->connection->onTrack([this, peer](std::shared_ptr<rtc::Track> track) {
		const QString kind = fromUtf8(track->description().type());
		const bool screenShare = peer->roomType == QStringLiteral("screen");
		std::unordered_map<std::uint8_t, IncomingCodec> codecs;
		std::unordered_map<std::uint8_t, std::uint8_t> rtxPayloadTypes;
		QStringList codecNames;
		rtc::Description::Media description = track->description();
		int audioLevelExtensionId = 0;
		for (const int extensionId : description.extIds()) {
			const auto *extension = description.extMap(extensionId);
			if (extension && extension->uri == "urn:ietf:params:rtp-hdrext:ssrc-audio-level") {
				audioLevelExtensionId = extensionId;
				break;
			}
		}
		for (const int payloadType : description.payloadTypes()) {
			const auto *mapping = description.rtpMap(payloadType);
			if (!mapping || payloadType < 0 || payloadType > 255)
				continue;
			const IncomingCodec codec = codecFromName(mapping->format);
			codecs[static_cast<std::uint8_t>(payloadType)] = codec;
			codecNames.push_back(fromUtf8(mapping->format));
			std::string lowerFormat = mapping->format;
			std::transform(lowerFormat.begin(), lowerFormat.end(), lowerFormat.begin(),
				       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			if (lowerFormat == "rtx") {
				for (const std::string &parameter : mapping->fmtps) {
					const std::size_t position = parameter.find("apt=");
					if (position == std::string::npos)
						continue;
					try {
						const int associatedPayloadType = std::stoi(parameter.substr(position + 4));
						if (associatedPayloadType >= 0 && associatedPayloadType <= 255)
							rtxPayloadTypes[static_cast<std::uint8_t>(payloadType)] =
								static_cast<std::uint8_t>(associatedPayloadType);
					} catch (const std::exception &) {
					}
				}
			}
		}
		auto decoder = std::make_shared<IncomingMediaDecoder>();
		auto codecLookup = std::make_shared<std::unordered_map<std::uint8_t, IncomingCodec>>(codecs);
		const auto onSpeakingChanged = [this, peer](bool speaking) {
			QString participantId;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				participantId = peer->participantId;
			}
			ParticipantMediaState state;
			state.participantId = participantId;
			state.speaking = speaking;
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] RTP audio-level speaking state"
					     << participantId << (speaking ? "speaking" : "not speaking");
			postParticipantMediaState(std::move(state));
		};
		auto depacketizer = std::make_shared<CodecRtpDepacketizer>(
			std::move(codecs), std::move(rtxPayloadTypes),
			kind == QStringLiteral("audio") ? audioLevelExtensionId : 0,
			kind == QStringLiteral("audio") ? onSpeakingChanged : std::function<void(bool)>{});
		auto videoClock = std::make_shared<RtpVideoClock>();
		auto vp8DecoderReady = std::make_shared<std::atomic_bool>(false);
		auto vp8FramesWaiting = std::make_shared<std::atomic_uint>(0);
		auto videoRestartPending = std::make_shared<std::atomic_bool>(false);
		// libdatachannel runs incoming handler chains from the tail towards the
		// root, so the receiving session is appended and sees RTP before the
		// root depacketizer assembles frames.
		depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
		track->setMediaHandler(depacketizer);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			peer->tracks.push_back(track);
			peer->decoders.push_back(decoder);
			if (kind == QStringLiteral("video")) {
				peer->restartVideoPipelines.push_back(
					[track, depacketizer, videoClock, vp8DecoderReady, vp8FramesWaiting,
					 videoRestartPending] {
						depacketizer->restartVideo();
						videoClock->restart();
						vp8DecoderReady->store(false);
						vp8FramesWaiting->store(0);
						videoRestartPending->store(true);
						track->requestKeyframe();
					});
			}
		}
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Incoming" << kind << "track codecs:"
				  << codecNames.join(QStringLiteral(", "));
		if (kind == QStringLiteral("audio"))
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] RTP audio-level extension id:"
					     << audioLevelExtensionId;
		track->onFrame([this, peer, track, decoder, codecLookup, videoClock, kind, screenShare, vp8DecoderReady,
				vp8FramesWaiting, videoRestartPending](rtc::binary frame, rtc::FrameInfo info) {
			QString participantId;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				participantId = peer->participantId;
			}
			const auto found = codecLookup->find(info.payloadType);
			if (found == codecLookup->end() || found->second == IncomingCodec::Unsupported)
				return;
			bool vp8Keyframe = false;
			const bool videoFrame = kind == QStringLiteral("video");
			const auto *encodedBytes = reinterpret_cast<const std::uint8_t *>(frame.data());
			const bool encryptedKeyframe = found->second == IncomingCodec::Vp8
							 ? frame.size() >= 1 && (encodedBytes[0] & 0x01) == 0
							 : found->second == IncomingCodec::H264 && isH264Keyframe(frame);
			std::vector<std::uint8_t> clearFrame(encodedBytes, encodedBytes + frame.size());
			if (e2ee_ && !e2ee_->decryptFrame(peer->remoteSessionId, clearFrame, videoFrame,
							    encryptedKeyframe))
				return;
			frame.resize(clearFrame.size());
			std::memcpy(frame.data(), clearFrame.data(), clearFrame.size());
			const auto *bytes = reinterpret_cast<const std::uint8_t *>(frame.data());
			const bool vp8StartCode = found->second == IncomingCodec::Vp8 && frame.size() >= 6 &&
						  (bytes[0] & 0x01) == 0 && bytes[3] == 0x9d && bytes[4] == 0x01 &&
						  bytes[5] == 0x2a;
			const bool clearKeyframe = vp8StartCode ||
						   (found->second == IncomingCodec::H264 && isH264Keyframe(frame));
			const bool restartPending = videoRestartPending->load();
			if (videoFrame && restartPending && !clearKeyframe) {
				const unsigned int waiting = vp8FramesWaiting->fetch_add(1) + 1;
				if (waiting == 1 || waiting % 30 == 0) {
					ObsLogLine(LOG_INFO)
						<< "[nextcloud-talk] Waiting for a keyframe after camera restart; requesting PLI";
					track->requestKeyframe();
				}
				return;
			}
			if (found->second == IncomingCodec::Vp8 && !vp8DecoderReady->load()) {
				if (!vp8StartCode) {
					const unsigned int waiting = vp8FramesWaiting->fetch_add(1) + 1;
					if (waiting == 1 || waiting % 30 == 0) {
						ObsLogLine(LOG_INFO)
							<< "[nextcloud-talk] Waiting for a valid VP8 keyframe; requesting PLI"
							<< "frameBytes=" << frame.size() << "firstByte="
							<< (frame.empty() ? -1 : static_cast<int>(bytes[0]));
						track->requestKeyframe();
					}
					return;
				}
				vp8Keyframe = true;
				ObsLogLine(LOG_INFO) << "[nextcloud-talk] Received valid VP8 keyframe for"
						     << participantId << frame.size() << "bytes";
			}
			const bool resetDecoder = videoFrame && clearKeyframe && restartPending;
			const bool decoded = decoder->decode(
				found->second, reinterpret_cast<const std::uint8_t *>(frame.data()), frame.size(),
				participantId, resetDecoder || (vp8Keyframe && !vp8DecoderReady->load()),
				videoFrame ? videoClock->map(info.timestamp) : 0, screenShare);
			if (decoded && resetDecoder)
				videoRestartPending->store(false);
			if (found->second == IncomingCodec::Vp8) {
				if (decoded) {
					vp8DecoderReady->store(true);
					vp8FramesWaiting->store(0);
				} else {
					const bool wasReady = vp8DecoderReady->exchange(false);
					if (vp8Keyframe || wasReady) {
						ObsLogLine(LOG_INFO)
							<< "[nextcloud-talk] VP8 decode continuity lost; requesting a new keyframe";
						track->requestKeyframe();
					}
				}
			}
		});
		if (kind == QStringLiteral("video")) {
			if (track->isOpen())
				track->requestKeyframe();
			else
				track->onOpen([track] { track->requestKeyframe(); });
		}
	});
	peer->connection->onDataChannel([this, peer](std::shared_ptr<rtc::DataChannel> channel) {
		if (!channel)
			return;
		const bool statusChannel = channel->label() == "status";
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] Incoming data channel:"
				     << fromUtf8(channel->label());
		observeDataChannel(peer, channel, statusChannel);
	});
	// Register every PeerConnection callback before adding tracks or data
	// channels: libdatachannel may start automatic negotiation immediately.
	if (publishLocalMedia)
		addOutgoingTracks(peer, hasMcu_);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		peers_[key] = peer;
	}
	return peer;
}

void TalkMediaTransport::addOutgoingTracks(const std::shared_ptr<PeerSession> &peer, bool sendOnly)
{
	const auto direction = sendOnly ? rtc::Description::Direction::SendOnly
					: rtc::Description::Direction::SendRecv;
	const std::string streamId = "obs-nextcloud-talk";
	const std::string cname = "obs-" + std::to_string(QRandomGenerator::global()->generate());
	const uint32_t baseSsrc = QRandomGenerator::global()->generate();

	rtc::Description::Audio audio("audio", direction);
	audio.addOpusCodec(111);
	audio.addSSRC(baseSsrc, cname, streamId, "obs-audio");
	auto audioTrack = peer->connection->addTrack(audio);
	auto audioConfig = std::make_shared<rtc::RtpPacketizationConfig>(
		baseSsrc, cname, 111, rtc::OpusRtpPacketizer::DefaultClockRate);
	auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioConfig);
	auto audioReporter = std::make_shared<rtc::RtcpSrReporter>(audioConfig);
	audioPacketizer->addToChain(audioReporter);
	audioPacketizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
	audioTrack->setMediaHandler(audioPacketizer);

	auto audioSend = std::make_shared<PeerSession::SendTrack>();
	audioSend->kind = EncodedMediaPacket::Kind::Audio;
	audioSend->track = audioTrack;
	audioSend->reporter = audioReporter;
	peer->tracks.push_back(audioTrack);
	peer->sendTracks.push_back(std::move(audioSend));

	rtc::Description::Video video("video", direction);
	video.addVP8Codec(96);
	video.addSSRC(baseSsrc + 1, cname, streamId, "obs-video");
	auto videoTrack = peer->connection->addTrack(video);
	auto videoConfig = std::make_shared<rtc::RtpPacketizationConfig>(
		baseSsrc + 1, cname, 96, rtc::H264RtpPacketizer::ClockRate);
	auto videoPacketizer = std::make_shared<Vp8RtpPacketizer>(videoConfig, 1200);
	auto videoReporter = std::make_shared<rtc::RtcpSrReporter>(videoConfig);
	videoPacketizer->addToChain(videoReporter);
	videoPacketizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
	videoTrack->setMediaHandler(videoPacketizer);

	auto videoSend = std::make_shared<PeerSession::SendTrack>();
	videoSend->kind = EncodedMediaPacket::Kind::Video;
	videoSend->track = videoTrack;
	videoSend->reporter = videoReporter;
	peer->tracks.push_back(videoTrack);
	peer->sendTracks.push_back(std::move(videoSend));

	// Add data channels after the media tracks so the SDP m-lines match Talk's
	// audio/video/application publisher layout.
	auto simpleWebRtc = peer->connection->createDataChannel("simplewebrtc");
	auto status = peer->connection->createDataChannel("status");
	peer->dataChannels.push_back(simpleWebRtc);
	observeDataChannel(peer, status, true);
	std::weak_ptr<rtc::DataChannel> weakStatus = status;
	status->onOpen([this, weakStatus] {
		if (const auto channel = weakStatus.lock()) {
			channel->send(std::string(R"({"type":"audioOn"})"));
			channel->send(std::string(R"({"type":"videoOn"})"));
			const QJsonObject nickname{{QStringLiteral("type"), QStringLiteral("nickChanged")},
						    {QStringLiteral("payload"),
						     QJsonObject{{QStringLiteral("name"), settings_.userId}}}};
			channel->send(utf8(QJsonDocument(nickname).toJson(QJsonDocument::Compact)));
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] MCU status data channel opened";
		}
	});
}

void TalkMediaTransport::sendEncodedPacket(EncodedMediaPacket packet)
{
	if (!active_ || packet.data.empty())
		return;
	auto &seen = packet.kind == EncodedMediaPacket::Kind::Video ? outgoingVideoSeen_ : outgoingAudioSeen_;
	if (!seen.exchange(true))
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] First encoded"
				  << (packet.kind == EncodedMediaPacket::Kind::Video ? "video" : "audio")
				  << "packet received from selected OBS source";
	std::vector<std::shared_ptr<PeerSession::SendTrack>> targets;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &entry : peers_) {
			for (const auto &track : entry.second->sendTracks) {
				if (track && track->kind == packet.kind)
					targets.push_back(track);
			}
		}
	}

	for (const auto &target : targets) {
		std::lock_guard<std::mutex> sendLock(target->mutex);
		if (!target->track || !target->track->isOpen() || !target->reporter)
			continue;
		auto config = target->reporter->rtpConfig;
		const double elapsedSeconds = static_cast<double>(packet.durationUs) / 1'000'000.0;
		config->timestamp += config->secondsToTimestamp(elapsedSeconds);
		try {
			std::vector<std::uint8_t> outgoing = packet.data;
			if (e2ee_ && !e2ee_->encryptFrame(outgoing,
							  packet.kind == EncodedMediaPacket::Kind::Video,
							  packet.keyframe, config->ssrc, config->timestamp))
				continue;
			rtc::binary sample;
			sample.resize(outgoing.size());
			std::memcpy(sample.data(), outgoing.data(), outgoing.size());
			target->track->send(sample);
			auto &sent = packet.kind == EncodedMediaPacket::Kind::Video ? outgoingVideoSent_
										    : outgoingAudioSent_;
			if (!sent.exchange(true))
				ObsLogLine(LOG_INFO) << "[nextcloud-talk] First encoded"
						  << (packet.kind == EncodedMediaPacket::Kind::Video ? "video" : "audio")
						  << "packet sent over WebRTC";
		} catch (const std::exception &error) {
			ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Could not send encoded media packet:"
					     << error.what();
		}
	}
}

void TalkMediaTransport::requestPreferredIncomingQuality(const std::shared_ptr<PeerSession> &peer)
{
	if (!peer || peer->publishesLocalMedia || peer->roomType != QStringLiteral("video") || !hasMcu_ ||
	    serverFeatures_.find("simulcast") == serverFeatures_.end() ||
	    peer->preferredQualityRequested.exchange(true))
		return;

	// Talk's web client explicitly selects both the highest spatial simulcast
	// stream and the highest temporal layer for a promoted participant. Without
	// this request Janus may leave a subscriber on a ~15 fps temporal layer,
	// which becomes especially visible when multiple cameras publish at once.
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Requesting high simulcast spatial/temporal quality for"
			      << peer->participantId;
	sendPeerMessage(peer, QStringLiteral("selectStream"),
			QJsonObject{{QStringLiteral("substream"), 2},
				    {QStringLiteral("temporal"), 2},
				    {QStringLiteral("audio"), true},
				    {QStringLiteral("video"), true}});
}

void TalkMediaTransport::sendPeerMessage(const std::shared_ptr<PeerSession> &peer, const QString &type,
					 const QJsonObject &payload)
{
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Sending WebRTC" << type << "to"
			     << peer->remoteSessionId.left(8);
	QJsonObject data{{QStringLiteral("to"), peer->remoteSessionId},
			 {QStringLiteral("sid"), peer->sid},
			 {QStringLiteral("roomType"), peer->roomType},
			 {QStringLiteral("type"), type},
			 {QStringLiteral("payload"), payload}};
	if (type == QStringLiteral("offer") && hasMcu_ && peer->remoteSessionId == signalingSessionId_ &&
	    serverFeatures_.find("offer-codecs") != serverFeatures_.end()) {
		data.insert(QStringLiteral("bitrate"), 2'000'000);
		data.insert(QStringLiteral("audiocodec"), QStringLiteral("opus"));
		data.insert(QStringLiteral("videocodec"), QStringLiteral("vp8"));
	}
	QJsonObject recipient{{QStringLiteral("type"), QStringLiteral("session")},
			      {QStringLiteral("sessionid"), peer->remoteSessionId}};
	sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
			     {QStringLiteral("message"), QJsonObject{{QStringLiteral("recipient"), recipient},
							       {QStringLiteral("data"), data}}}});
}

std::string TalkMediaTransport::peerKey(const QString &sender, const QString &sid,
					const QString &roomType) const
{
	return utf8(sender + QLatin1Char('|') + sid + QLatin1Char('|') + roomType);
}

void TalkMediaTransport::configureIce(const SignalingSettings &settings)
{
	*rtcConfiguration_ = rtc::Configuration{};
	auto append = [this](const std::vector<IceServerConfig> &servers) {
		for (const IceServerConfig &source : servers) {
			for (const QString &url : source.urls) {
				try {
					rtc::IceServer server(utf8(url));
					server.username = utf8(source.username);
					server.password = utf8(source.credential);
					rtcConfiguration_->iceServers.emplace_back(std::move(server));
				} catch (const std::exception &error) {
					ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Ignoring invalid ICE server:"
							     << error.what();
				}
			}
		}
	};
	append(settings.stunServers);
	append(settings.turnServers);
}

} // namespace nextcloud_talk
