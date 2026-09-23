#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace nextcloud_talk {

class WinHttpWebSocket final {
public:
	WinHttpWebSocket() = default;
	~WinHttpWebSocket();

	void open(const QString &url);
	void close();
	bool sendText(const QByteArray &message);
	bool isOpen() const { return websocketHandle_.load() != nullptr; }

	std::function<void()> onOpen;
	std::function<void(const QByteArray &)> onTextMessage;
	std::function<void(const QString &)> onError;
	std::function<void()> onClosed;

private:
	void run(QString url);
	void reportWindowsError(const QString &operation, unsigned long code);

	std::atomic_bool active_{false};
	std::atomic<void *> websocketHandle_{nullptr};
	std::mutex sendMutex_;
	std::thread worker_;
};

} // namespace nextcloud_talk
