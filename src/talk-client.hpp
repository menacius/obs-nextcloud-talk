#pragma once

#include "talk-types.hpp"

#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <functional>

class QNetworkReply;

namespace nextcloud_talk {

class TalkClient final : public QObject {
public:
	using StateHandler = std::function<void(ConnectionState, const QString &)>;
	using ConversationsHandler = std::function<void(const std::vector<Conversation> &)>;
	using ParticipantsHandler = std::function<void(const std::vector<Participant> &)>;
	using ChatMessagesHandler = std::function<void(const std::vector<ChatMessage> &)>;
	using MediaSessionHandler =
		std::function<void(const QString &, const QString &, const QString &, const SignalingSettings &)>;

	explicit TalkClient(QObject *parent = nullptr);

	void configure(const QString &serverUrl, const QString &username, const QString &appPassword);
	void listConversations();
	void joinCall(const QString &token, bool callAlreadyActive);
	void leaveCall();
	void updateCallFlags(int flags);
	void pollParticipants();
	void sendChatMessage(const QString &message);

	ConnectionState state() const { return state_; }
	QString activeToken() const { return activeToken_; }

	StateHandler onStateChanged;
	ConversationsHandler onConversationsChanged;
	ParticipantsHandler onParticipantsChanged;
	ChatMessagesHandler onChatMessages;
	std::function<void()> onChatReset;
	std::function<void(bool, const QString &)> onChatSendFinished;
	MediaSessionHandler onMediaSessionReady;
	std::function<void()> onMediaSessionEnded;

	static bool parseOcsData(const QByteArray &body, QJsonValue &data, QString &error);
	static std::vector<Conversation> parseConversations(const QJsonValue &data);
	static std::vector<Participant> parseParticipants(const QJsonValue &data);
	static std::vector<ChatMessage> parseChatMessages(const QJsonValue &data);
	static SignalingSettings parseSignalingSettings(const QJsonValue &data);

private:
	enum class Method { Get, Post, Put, Delete };
	using ReplyHandler = std::function<void(bool, const QJsonValue &, const QString &)>;

	void setState(ConnectionState state, const QString &detail = {});
	void joinConversation(const QString &token, bool callAlreadyActive);
	void joinCallRequest(const QString &token, bool callAlreadyActive);
	void loadSignalingSettings(const QString &token);
	void loadChatHistory(const QString &token);
	void pollChat();
	void deliverChatMessages(const QJsonValue &data);
	void leaveConversation(const QString &token, std::function<void()> finished);
	void request(Method method, const QString &path, const QByteArray &form, ReplyHandler handler,
		     int version = 4);
	QUrl apiUrl(const QString &path, int version = 4) const;

	QNetworkAccessManager network_;
	QTimer participantPollTimer_;
	QTimer chatPollTimer_;
	QString serverUrl_;
	QString username_;
	QString appPassword_;
	QString activeToken_;
	QString conversationSessionId_;
	qint64 lastChatMessageId_ = 0;
	bool chatHistoryLoaded_ = false;
	bool chatPollPending_ = false;
	bool conversationSessionActive_ = false;
	ConnectionState state_ = ConnectionState::Disconnected;
};

} // namespace nextcloud_talk
