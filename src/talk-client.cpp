#include "talk-client.hpp"
#include "obs-log.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>

namespace nextcloud_talk {

TalkClient::TalkClient(QObject *parent) : QObject(parent)
{
	participantPollTimer_.setInterval(2000);
	connect(&participantPollTimer_, &QTimer::timeout, this, [this] { pollParticipants(); });
	chatPollTimer_.setInterval(1000);
	connect(&chatPollTimer_, &QTimer::timeout, this, [this] { pollChat(); });
}

void TalkClient::configure(const QString &serverUrl, const QString &username, const QString &appPassword)
{
	serverUrl_ = serverUrl.trimmed();
	while (serverUrl_.endsWith('/'))
		serverUrl_.chop(1);
	username_ = username.trimmed();
	appPassword_ = appPassword;
}

void TalkClient::listConversations()
{
	if (serverUrl_.isEmpty() || username_.isEmpty() || appPassword_.isEmpty()) {
		setState(ConnectionState::Error, QStringLiteral("Server URL, username and app password are required."));
		return;
	}

	setState(ConnectionState::LoadingConversations, QStringLiteral("Loading conversations…"));
	request(Method::Get, QStringLiteral("/room?includeStatus=false"), {},
		[this](bool ok, const QJsonValue &data, const QString &error) {
			if (!ok) {
				setState(ConnectionState::Error, error);
				return;
			}
			const auto conversations = parseConversations(data);
			if (onConversationsChanged)
				onConversationsChanged(conversations);
			setState(ConnectionState::Ready,
				 QStringLiteral("Connected — %1 conversation(s)").arg(conversations.size()));
		});
}

void TalkClient::joinCall(const QString &token, bool callAlreadyActive)
{
	if (token.isEmpty()) {
		setState(ConnectionState::Error, QStringLiteral("Select a conversation first."));
		return;
	}

	setState(ConnectionState::Joining, callAlreadyActive ? QStringLiteral("Joining conversation…")
							     : QStringLiteral("Preparing to start call…"));
	joinConversation(token, callAlreadyActive);
}

void TalkClient::joinConversation(const QString &token, bool callAlreadyActive)
{
	QUrlQuery form;
	form.addQueryItem(QStringLiteral("force"), QStringLiteral("true"));
	request(Method::Post,
		QStringLiteral("/room/%1/participants/active").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))),
		form.toString(QUrl::FullyEncoded).toUtf8(),
		[this, token, callAlreadyActive](bool ok, const QJsonValue &data, const QString &error) {
			if (!ok) {
				setState(ConnectionState::Error,
					 QStringLiteral("Could not join conversation: %1").arg(error));
				return;
			}
			conversationSessionActive_ = true;
			conversationSessionId_ = data.toObject().value(QStringLiteral("sessionId")).toString();
			if (conversationSessionId_.isEmpty()) {
				leaveConversation(token, [this] {
					setState(ConnectionState::Error,
						 QStringLiteral("Could not join conversation: the server returned no session ID."));
				});
				return;
			}
			joinCallRequest(token, callAlreadyActive);
		});
}

void TalkClient::joinCallRequest(const QString &token, bool callAlreadyActive)
{
	setState(ConnectionState::Joining,
		 callAlreadyActive ? QStringLiteral("Joining call…") : QStringLiteral("Starting call…"));
	QUrlQuery form;
	// Enter the call without advertising streams until the signaling room and
	// publisher peer are ready. The transport upgrades this to audio+video (7).
	form.addQueryItem(QStringLiteral("flags"), QStringLiteral("1"));
	form.addQueryItem(QStringLiteral("silent"),
			  callAlreadyActive ? QStringLiteral("true") : QStringLiteral("false"));
	request(Method::Post, QStringLiteral("/call/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))),
		form.toString(QUrl::FullyEncoded).toUtf8(),
		[this, token](bool ok, const QJsonValue &, const QString &error) {
			if (!ok) {
				leaveConversation(token, [this, error] {
					setState(ConnectionState::Error,
						 QStringLiteral("Could not join or start call: %1").arg(error));
				});
				return;
			}
			activeToken_ = token;
			participantPollTimer_.start();
			lastChatMessageId_ = 0;
			chatHistoryLoaded_ = false;
			chatPollPending_ = false;
			chatPollTimer_.start();
			if (onChatReset)
				onChatReset();
			setState(ConnectionState::InCall, QStringLiteral("In call — discovering media transport…"));
			pollParticipants();
			loadChatHistory(token);
			loadSignalingSettings(token);
		});
}

void TalkClient::loadChatHistory(const QString &token)
{
	chatPollPending_ = true;
	request(Method::Get,
		QStringLiteral("/chat/%1?lookIntoFuture=0&limit=100&setReadMarker=1")
			.arg(QString::fromUtf8(QUrl::toPercentEncoding(token))),
		{}, [this, token](bool ok, const QJsonValue &data, const QString &error) {
			chatPollPending_ = false;
			if (token != activeToken_ || state_ != ConnectionState::InCall)
				return;
			if (!ok) {
				if (onChatSendFinished)
					onChatSendFinished(false, QStringLiteral("Could not load chat: %1").arg(error));
				return;
			}
			chatHistoryLoaded_ = true;
			deliverChatMessages(data);
		}, 1);
}

void TalkClient::pollChat()
{
	if (activeToken_.isEmpty() || chatPollPending_ || state_ != ConnectionState::InCall)
		return;
	if (!chatHistoryLoaded_) {
		loadChatHistory(activeToken_);
		return;
	}

	const QString token = activeToken_;
	chatPollPending_ = true;
	request(Method::Get,
		QStringLiteral("/chat/%1?lookIntoFuture=1&limit=100&lastKnownMessageId=%2&setReadMarker=1")
			.arg(QString::fromUtf8(QUrl::toPercentEncoding(token)))
			.arg(lastChatMessageId_),
		{}, [this, token](bool ok, const QJsonValue &data, const QString &) {
			chatPollPending_ = false;
			if (token != activeToken_ || state_ != ConnectionState::InCall || !ok)
				return;
			deliverChatMessages(data);
		}, 1);
}

void TalkClient::sendChatMessage(const QString &message)
{
	const QString trimmed = message.trimmed();
	if (activeToken_.isEmpty() || state_ != ConnectionState::InCall || trimmed.isEmpty())
		return;

	const QString token = activeToken_;
	QUrlQuery form;
	form.addQueryItem(QStringLiteral("message"), trimmed);
	request(Method::Post,
		QStringLiteral("/chat/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))),
		form.toString(QUrl::FullyEncoded).toUtf8(),
		[this, token](bool ok, const QJsonValue &data, const QString &error) {
			if (token != activeToken_)
				return;
			if (ok)
				deliverChatMessages(data);
			if (onChatSendFinished)
				onChatSendFinished(ok, ok ? QString() : error);
		}, 1);
}

void TalkClient::deliverChatMessages(const QJsonValue &data)
{
	auto messages = parseChatMessages(data);
	std::sort(messages.begin(), messages.end(), [](const ChatMessage &a, const ChatMessage &b) {
		return a.id < b.id;
	});
	for (const ChatMessage &message : messages)
		lastChatMessageId_ = std::max(lastChatMessageId_, message.id);
	if (!messages.empty() && onChatMessages)
		onChatMessages(messages);
}

void TalkClient::loadSignalingSettings(const QString &token)
{
	request(Method::Get,
		QStringLiteral("/signaling/settings?token=%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))),
		{}, [this, token](bool ok, const QJsonValue &data, const QString &error) {
			if (token != activeToken_)
				return;
			if (!ok) {
				setState(ConnectionState::InCall,
					 QStringLiteral("In call — media discovery failed: %1").arg(error));
				return;
			}
			const SignalingSettings settings = parseSignalingSettings(data);
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Signaling settings: mode=" << settings.mode
					  << "serverPresent=" << !settings.server.isEmpty()
					  << "helloV1=" << !settings.helloAuthV1.isEmpty()
					  << "helloV2=" << !settings.helloAuthV2.isEmpty()
					  << "STUN=" << settings.stunServers.size()
					  << "TURN=" << settings.turnServers.size();
			if (onMediaSessionReady)
				onMediaSessionReady(serverUrl_, token, conversationSessionId_, settings);
		}, 3);
}

void TalkClient::leaveCall()
{
	if (onMediaSessionEnded)
		onMediaSessionEnded();
	chatPollTimer_.stop();
	chatPollPending_ = false;
	chatHistoryLoaded_ = false;
	if (onChatReset)
		onChatReset();

	if (activeToken_.isEmpty()) {
		participantPollTimer_.stop();
		setState(ConnectionState::Ready, QStringLiteral("Not in a call"));
		return;
	}

	setState(ConnectionState::Leaving, QStringLiteral("Leaving call…"));
	const QString token = activeToken_;
	request(Method::Delete, QStringLiteral("/call/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))), {},
		[this](bool ok, const QJsonValue &, const QString &error) {
			participantPollTimer_.stop();
			const QString token = activeToken_;
			activeToken_.clear();
			conversationSessionId_.clear();
			if (onParticipantsChanged)
				onParticipantsChanged({});
			leaveConversation(token, [this, ok, error] {
				setState(ok ? ConnectionState::Ready : ConnectionState::Error,
					 ok ? QStringLiteral("Left call")
					    : QStringLiteral("Call leave failed: %1").arg(error));
			});
		});
}

void TalkClient::updateCallFlags(int flags)
{
	if (activeToken_.isEmpty())
		return;
	const QString token = activeToken_;
	QUrlQuery form;
	form.addQueryItem(QStringLiteral("flags"), QString::number(flags));
	request(Method::Put,
		QStringLiteral("/call/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))),
		form.toString(QUrl::FullyEncoded).toUtf8(),
		[this, token, flags](bool ok, const QJsonValue &, const QString &error) {
			if (token != activeToken_)
				return;
			if (!ok) {
				setState(ConnectionState::InCall,
					 QStringLiteral("In call — could not advertise outgoing media: %1").arg(error));
				return;
			}
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Updated call media flags to" << flags;
		});
}

void TalkClient::leaveConversation(const QString &token, std::function<void()> finished)
{
	if (!conversationSessionActive_ || token.isEmpty()) {
		conversationSessionActive_ = false;
		conversationSessionId_.clear();
		finished();
		return;
	}

	request(Method::Delete,
		QStringLiteral("/room/%1/participants/active").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))),
		{}, [this, finished = std::move(finished)](bool, const QJsonValue &, const QString &) mutable {
			conversationSessionActive_ = false;
			conversationSessionId_.clear();
			finished();
		});
}

void TalkClient::pollParticipants()
{
	if (activeToken_.isEmpty())
		return;
	const QString token = activeToken_;
	request(Method::Get, QStringLiteral("/call/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(token))), {},
		[this, token](bool ok, const QJsonValue &data, const QString &error) {
			if (token != activeToken_)
				return;
			if (!ok) {
				// Participant polling is advisory while the signaling/WebRTC
				// session remains active. Keep the call state and retry on the
				// next timer tick instead of exposing a second Join action.
				setState(ConnectionState::InCall,
					 QStringLiteral("In call — participant sync failed; retrying: %1").arg(error));
				return;
			}
			if (onParticipantsChanged)
				onParticipantsChanged(parseParticipants(data));
		});
}

bool TalkClient::parseOcsData(const QByteArray &body, QJsonValue &data, QString &error)
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		error = QStringLiteral("Invalid JSON response: %1").arg(parseError.errorString());
		return false;
	}

	const QJsonObject root = document.object();
	const QJsonObject ocs = root.value(QStringLiteral("ocs")).toObject();
	if (ocs.isEmpty()) {
		error = QStringLiteral("Response did not contain an OCS envelope.");
		return false;
	}

	const QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
	const int statusCode = meta.value(QStringLiteral("statuscode")).toInt(100);
	if (statusCode != 100 && statusCode != 200) {
		error = meta.value(QStringLiteral("message")).toString(QStringLiteral("Nextcloud OCS request failed."));
		return false;
	}

	data = ocs.value(QStringLiteral("data"));
	return true;
}

std::vector<Conversation> TalkClient::parseConversations(const QJsonValue &data)
{
	auto jsonBool = [](const QJsonValue &value, bool defaultValue) {
		if (value.isBool())
			return value.toBool();
		if (value.isDouble())
			return value.toInt() != 0;
		if (value.isString())
			return value.toString() == QStringLiteral("1") ||
			       value.toString().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
		return defaultValue;
	};

	std::vector<Conversation> result;
	for (const QJsonValue &value : data.toArray()) {
		const QJsonObject object = value.toObject();
		Conversation conversation;
		conversation.token = object.value(QStringLiteral("token")).toString();
		conversation.displayName = object.value(QStringLiteral("displayName")).toString();
		conversation.callActive = jsonBool(object.value(QStringLiteral("hasCall")), false);
		conversation.canStartCall = jsonBool(object.value(QStringLiteral("canStartCall")), true);
		if (!conversation.token.isEmpty()) {
			if (conversation.displayName.isEmpty())
				conversation.displayName = conversation.token;
			result.emplace_back(std::move(conversation));
		}
	}
	std::sort(result.begin(), result.end(), [](const Conversation &a, const Conversation &b) {
		return a.displayName.localeAwareCompare(b.displayName) < 0;
	});
	return result;
}

std::vector<Participant> TalkClient::parseParticipants(const QJsonValue &data)
{
	std::vector<Participant> result;
	for (const QJsonValue &value : data.toArray()) {
		const QJsonObject object = value.toObject();
		Participant participant;
		const QString actorType = object.value(QStringLiteral("actorType")).toString();
		const QString actorId = object.value(QStringLiteral("actorId")).toString();
		participant.sessionId = object.value(QStringLiteral("sessionId")).toString();
		participant.stableId = actorId.isEmpty() ? participant.sessionId : actorType + ':' + actorId;
		participant.displayName = object.value(QStringLiteral("displayName")).toString();
		const int flags = object.value(QStringLiteral("inCall")).toInt(7);
		participant.audioAvailable = (flags & 2) != 0;
		participant.videoAvailable = (flags & 4) != 0;
		if (!participant.stableId.isEmpty()) {
			if (participant.displayName.isEmpty())
				participant.displayName = actorId.isEmpty() ? QStringLiteral("Guest") : actorId;
			result.emplace_back(std::move(participant));
		}
	}
	return result;
}

std::vector<ChatMessage> TalkClient::parseChatMessages(const QJsonValue &data)
{
	QJsonArray values;
	if (data.isArray())
		values = data.toArray();
	else if (data.isObject())
		values.append(data);

	std::vector<ChatMessage> result;
	for (const QJsonValue &value : values) {
		const QJsonObject object = value.toObject();
		ChatMessage message;
		message.id = object.value(QStringLiteral("id")).toVariant().toLongLong();
		message.timestamp = object.value(QStringLiteral("timestamp")).toVariant().toLongLong();
		message.actorDisplayName = object.value(QStringLiteral("actorDisplayName")).toString();
		message.message = object.value(QStringLiteral("message")).toString();
		message.messageType = object.value(QStringLiteral("messageType")).toString();
		message.systemMessage = object.value(QStringLiteral("systemMessage")).toString();

		const QJsonObject parameters = object.value(QStringLiteral("messageParameters")).toObject();
		for (auto iterator = parameters.constBegin(); iterator != parameters.constEnd(); ++iterator) {
			const QString replacement = iterator.value().toObject().value(QStringLiteral("name")).toString();
			if (!replacement.isEmpty())
				message.message.replace(QStringLiteral("{%1}").arg(iterator.key()), replacement);
		}

		if (message.id > 0 && !message.message.isEmpty() &&
		    message.messageType != QStringLiteral("comment_deleted"))
			result.emplace_back(std::move(message));
	}
	return result;
}

SignalingSettings TalkClient::parseSignalingSettings(const QJsonValue &data)
{
	SignalingSettings settings;
	const QJsonObject object = data.toObject();
	settings.mode = object.value(QStringLiteral("signalingMode")).toString();
	const QJsonValue server = object.value(QStringLiteral("server"));
	if (server.isArray() && !server.toArray().isEmpty())
		settings.server = server.toArray().first().toString();
	else
		settings.server = server.toString();
	settings.userId = object.value(QStringLiteral("userId")).toString();

	const QJsonObject auth = object.value(QStringLiteral("helloAuthParams")).toObject();
	settings.helloAuthV1 = auth.value(QStringLiteral("1.0")).toObject();
	settings.helloAuthV2 = auth.value(QStringLiteral("2.0")).toObject();
	if (settings.helloAuthV1.isEmpty()) {
		settings.helloAuthV1.insert(QStringLiteral("userid"), settings.userId);
		settings.helloAuthV1.insert(QStringLiteral("ticket"), object.value(QStringLiteral("ticket")));
	}

	auto parseIceServers = [](const QJsonValue &value) {
		std::vector<IceServerConfig> result;
		for (const QJsonValue &entryValue : value.toArray()) {
			const QJsonObject entry = entryValue.toObject();
			IceServerConfig ice;
			const QJsonValue urls = entry.value(QStringLiteral("urls"));
			if (urls.isArray()) {
				for (const QJsonValue &url : urls.toArray())
					ice.urls.push_back(url.toString());
			} else if (urls.isString()) {
				ice.urls.push_back(urls.toString());
			}
			ice.username = entry.value(QStringLiteral("username")).toString();
			ice.credential = entry.value(QStringLiteral("credential")).toString();
			if (!ice.urls.isEmpty())
				result.emplace_back(std::move(ice));
		}
		return result;
	};

	settings.stunServers = parseIceServers(object.value(QStringLiteral("stunservers")));
	settings.turnServers = parseIceServers(object.value(QStringLiteral("turnservers")));
	return settings;
}

void TalkClient::setState(ConnectionState state, const QString &detail)
{
	state_ = state;
	ObsLogLine(LOG_INFO) << "[nextcloud-talk] Call state" << static_cast<int>(state) << "—" << detail;
	if (onStateChanged)
		onStateChanged(state, detail);
}

void TalkClient::request(Method method, const QString &path, const QByteArray &form, ReplyHandler handler,
			 int version)
{
	QNetworkRequest request(apiUrl(path, version));
	request.setRawHeader("Accept", "application/json");
	request.setRawHeader("OCS-APIRequest", "true");
	request.setRawHeader("Authorization", "Basic " + (username_ + ':' + appPassword_).toUtf8().toBase64());
	if (!form.isEmpty())
		request.setHeader(QNetworkRequest::ContentTypeHeader,
				  QStringLiteral("application/x-www-form-urlencoded"));

	QNetworkReply *reply = nullptr;
	switch (method) {
	case Method::Get:
		reply = network_.get(request);
		break;
	case Method::Post:
		reply = network_.post(request, form);
		break;
	case Method::Put:
		reply = network_.put(request, form);
		break;
	case Method::Delete:
		reply = network_.deleteResource(request);
		break;
	}

	connect(reply, &QNetworkReply::finished, this, [reply, handler = std::move(handler)] {
		const QByteArray body = reply->readAll();
		const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (httpStatus == 304) {
			reply->deleteLater();
			handler(true, QJsonArray(), {});
			return;
		}
		if (reply->error() != QNetworkReply::NoError) {
			QJsonValue ignoredData;
			QString ocsError;
			parseOcsData(body, ignoredData, ocsError);
			const QString detail = ocsError.isEmpty() ? reply->errorString() : ocsError;
			const QString message = QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(detail);
			ObsLogLine(LOG_WARNING) << "[nextcloud-talk] OCS request failed with status" << httpStatus << detail;
			reply->deleteLater();
			handler(false, {}, message);
			return;
		}

		QJsonValue data;
		QString error;
		const bool ok = parseOcsData(body, data, error);
		reply->deleteLater();
		handler(ok, data, error);
	});
}

QUrl TalkClient::apiUrl(const QString &path, int version) const
{
	return QUrl(serverUrl_ + QStringLiteral("/ocs/v2.php/apps/spreed/api/v%1").arg(version) + path);
}

} // namespace nextcloud_talk
