#include "talk-client.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>

using nextcloud_talk::TalkClient;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

void testConversationParsing()
{
	const QByteArray body =
		R"({"ocs":{"meta":{"status":"ok","statuscode":100,"message":"OK"},"data":[{"token":"abc","displayName":"Production","hasCall":true,"callFlag":4},{"token":"xyz","displayName":"Daily","hasCall":0,"callFlag":4,"canStartCall":false}]}})";
	QJsonValue data;
	QString error;
	check(TalkClient::parseOcsData(body, data, error), "valid OCS response should parse");
	const auto conversations = TalkClient::parseConversations(data);
	check(conversations.size() == 2, "two conversations should be returned");
	check(conversations[0].displayName == QStringLiteral("Daily"), "conversations should be sorted by name");
	check(conversations[1].callActive, "hasCall should be retained");
	check(!conversations[0].callActive, "callFlag must not be treated as an active call");
	check(!conversations[0].canStartCall, "canStartCall should be retained");
}

void testParticipantParsing()
{
	const QByteArray body =
		R"({"ocs":{"meta":{"statuscode":100},"data":[{"actorType":"users","actorId":"alice","displayName":"Alice","sessionId":"s1","inCall":7},{"actorType":"guests","actorId":"g7","displayName":"Guest","sessionId":"s2","inCall":3}]}})";
	QJsonValue data;
	QString error;
	check(TalkClient::parseOcsData(body, data, error), "participant OCS response should parse");
	const auto participants = TalkClient::parseParticipants(data);
	check(participants.size() == 2, "two participants should be returned");
	check(participants[0].stableId == QStringLiteral("users:alice"), "stable actor key should include type");
	check(participants[0].videoAvailable, "video flag should parse");
	check(!participants[1].videoAvailable, "missing video flag should parse");
}

void testChatParsing()
{
	const QJsonDocument document = QJsonDocument::fromJson(
		R"([{"id":42,"timestamp":1710000000,"actorDisplayName":"Alice","messageType":"comment","systemMessage":"","message":"Hello {mention-bob}","messageParameters":{"mention-bob":{"type":"user","id":"bob","name":"Bob"}}}])");
	const auto messages = TalkClient::parseChatMessages(document.array());
	check(messages.size() == 1, "one chat message should be returned");
	check(messages[0].id == 42, "chat message ID should parse");
	check(messages[0].actorDisplayName == QStringLiteral("Alice"), "chat author should parse");
	check(messages[0].message == QStringLiteral("Hello Bob"), "rich-object placeholders should be readable");
}

void testErrors()
{
	QJsonValue data;
	QString error;
	check(!TalkClient::parseOcsData("not-json", data, error), "invalid JSON should fail");
	check(!error.isEmpty(), "invalid JSON should explain the failure");
	error.clear();
	check(!TalkClient::parseOcsData(
		      R"({"ocs":{"meta":{"statuscode":997,"message":"Current user is not logged in"},"data":[]}})",
		      data, error),
	      "OCS error status should fail");
	check(error.contains(QStringLiteral("not logged in")), "OCS message should be exposed");
}

void testSignalingSettingsParsing()
{
	QJsonValue data;
	QString error;
	const QByteArray body =
		R"({"ocs":{"meta":{"statuscode":100},"data":{"signalingMode":"external","server":"https://signal.example.test/","userId":"alice","helloAuthParams":{"1.0":{"userid":"alice","ticket":"secret"},"2.0":{"token":"jwt"}},"stunservers":[{"urls":["stun:stun.example.test:3478"]}],"turnservers":[{"urls":["turn:turn.example.test:3478?transport=udp"],"username":"u","credential":"p"}]}}})";
	check(TalkClient::parseOcsData(body, data, error), "signaling settings OCS response should parse");
	const auto settings = TalkClient::parseSignalingSettings(data);
	check(settings.mode == QStringLiteral("external"), "signaling mode should parse");
	check(settings.helloAuthV1.value(QStringLiteral("ticket")).toString() == QStringLiteral("secret"),
	      "v1 signaling auth should parse");
	check(settings.stunServers.size() == 1, "STUN server should parse");
	check(settings.turnServers.size() == 1, "TURN server should parse");
	check(settings.turnServers[0].credential == QStringLiteral("p"), "TURN credential should parse");
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testConversationParsing();
	testParticipantParsing();
	testChatParsing();
	testErrors();
	testSignalingSettingsParsing();
	if (failures == 0)
		std::cout << "All tests passed\n";
	return failures == 0 ? 0 : 1;
}
