#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#ifndef Q_OS_WIN
namespace rtc {
class WebSocket;
}
#endif

namespace nextcloud_talk {

#ifndef Q_OS_WIN
struct WebSocketCallbackState;
#endif

class WinHttpWebSocket final {
public:
	WinHttpWebSocket() = default;
	~WinHttpWebSocket();

	void open(const QString &url);
	void close();
	bool sendText(const QByteArray &message);
	bool isOpen() const;

	std::function<void()> onOpen;
	std::function<void(const QByteArray &)> onTextMessage;
	std::function<void(const QString &)> onError;
	std::function<void()> onClosed;

private:
#ifdef Q_OS_WIN
	void run(QString url);
	void reportWindowsError(const QString &operation, unsigned long code);
#endif

	std::atomic_bool active_{false};
#ifdef Q_OS_WIN
	std::atomic<void *> websocketHandle_{nullptr};
	std::mutex sendMutex_;
	std::thread worker_;
#else
	mutable std::mutex socketMutex_;
	std::shared_ptr<rtc::WebSocket> websocket_;
	std::shared_ptr<WebSocketCallbackState> callbackState_;
#endif
};

} // namespace nextcloud_talk
