#include "talk-e2ee.hpp"

#include <QJsonObject>

#include <cstdint>
#include <iostream>
#include <vector>

using nextcloud_talk::TalkE2ee;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

QJsonObject envelope(const QJsonObject &payload)
{
	return QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
			   {QStringLiteral("payload"), payload}};
}

} // namespace

int main()
{
	TalkE2ee first;
	TalkE2ee second;

	first.sendCallMessage = [&](const QString &target, const QJsonObject &payload) {
		check(target == QStringLiteral("alpha"), "initiator should address the alpha session");
		check(second.handleMessage(QStringLiteral("bravo"), envelope(payload)),
		      "responder should accept the initiator message");
	};
	second.sendCallMessage = [&](const QString &target, const QJsonObject &payload) {
		check(target == QStringLiteral("bravo"), "responder should address the bravo session");
		check(first.handleMessage(QStringLiteral("alpha"), envelope(payload)),
		      "initiator should accept the responder message");
	};

	first.start(QStringLiteral("bravo"));
	second.start(QStringLiteral("alpha"));
	first.participantJoined(QStringLiteral("alpha"));

	const std::vector<std::uint8_t> originalFirst{0x78, 'h', 'e', 'l', 'l', 'o'};
	auto firstFrame = originalFirst;
	check(first.encryptFrame(firstFrame, false, false, 0x10203040u, 0x50607080u),
	      "first peer should encrypt an audio frame");
	check(firstFrame != originalFirst, "encrypted frame should differ from plaintext");
	check(second.decryptFrame(QStringLiteral("bravo"), firstFrame, false, false),
	      "second peer should decrypt the first peer frame");
	check(firstFrame == originalFirst, "first peer frame should round-trip exactly");

	const std::vector<std::uint8_t> originalSecond{0x79, 'w', 'o', 'r', 'l', 'd'};
	auto secondFrame = originalSecond;
	check(second.encryptFrame(secondFrame, false, false, 0x11223344u, 0x55667788u),
	      "second peer should encrypt an audio frame");
	check(first.decryptFrame(QStringLiteral("alpha"), secondFrame, false, false),
	      "first peer should decrypt the second peer frame");
	check(secondFrame == originalSecond, "second peer frame should round-trip exactly");

	if (failures == 0)
		std::cout << "E2EE key exchange and AES-GCM round trip passed\n";
	return failures == 0 ? 0 : 1;
}
