#include "winhttp-websocket.hpp"

#include <rtc/websocket.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace nextcloud_talk {

struct WebSocketCallbackState {
	std::mutex mutex;
	bool active = true;
	std::function<void()> opened;
	std::function<void(const QByteArray &)> textMessage;
	std::function<void(const QString &)> error;
	std::function<void()> closed;
};

namespace {

template<typename Callback>
Callback activeCallback(const std::shared_ptr<WebSocketCallbackState> &state,
			Callback WebSocketCallbackState::*member)
{
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->active ? state.get()->*member : Callback{};
}

} // namespace

WinHttpWebSocket::~WinHttpWebSocket()
{
	close();
}

void WinHttpWebSocket::open(const QString &url)
{
	close();
	if (url.trimmed().isEmpty())
		throw std::invalid_argument("The signaling WebSocket URL is empty");

	auto state = std::make_shared<WebSocketCallbackState>();
	state->opened = onOpen;
	state->textMessage = onTextMessage;
	state->error = onError;
	state->closed = onClosed;

	rtc::WebSocket::Configuration configuration;
	configuration.connectionTimeout = std::chrono::seconds(10);
	configuration.pingInterval = std::chrono::seconds(30);
	configuration.maxOutstandingPings = 2;
	auto socket = std::make_shared<rtc::WebSocket>(configuration);

	socket->onOpen([state] {
		if (auto callback = activeCallback(state, &WebSocketCallbackState::opened))
			callback();
	});
	socket->onMessage(
		[state](rtc::binary) {
			if (auto callback = activeCallback(state, &WebSocketCallbackState::error))
				callback(QStringLiteral("The signaling server sent an unexpected binary message."));
		},
		[state](std::string message) {
			if (auto callback = activeCallback(state, &WebSocketCallbackState::textMessage))
				callback(QByteArray(message.data(), static_cast<qsizetype>(message.size())));
		});
	socket->onError([state](std::string error) {
		if (auto callback = activeCallback(state, &WebSocketCallbackState::error))
			callback(QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size())));
	});
	socket->onClosed([state] {
		std::function<void()> callback;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!state->active)
				return;
			state->active = false;
			callback = state->closed;
		}
		if (callback)
			callback();
	});

	{
		std::lock_guard<std::mutex> lock(socketMutex_);
		callbackState_ = state;
		websocket_ = socket;
		active_ = true;
	}
	try {
		socket->open(url.toStdString());
	} catch (...) {
		close();
		throw;
	}
}

void WinHttpWebSocket::close()
{
	active_ = false;
	std::shared_ptr<rtc::WebSocket> socket;
	std::shared_ptr<WebSocketCallbackState> state;
	{
		std::lock_guard<std::mutex> lock(socketMutex_);
		socket = std::move(websocket_);
		state = std::move(callbackState_);
	}
	if (state) {
		std::lock_guard<std::mutex> lock(state->mutex);
		state->active = false;
	}
	if (socket) {
		socket->resetCallbacks();
		if (!socket->isClosed())
			socket->close();
	}
}

bool WinHttpWebSocket::sendText(const QByteArray &message)
{
	std::shared_ptr<rtc::WebSocket> socket;
	{
		std::lock_guard<std::mutex> lock(socketMutex_);
		socket = websocket_;
	}
	if (!active_ || !socket || !socket->isOpen())
		return false;
	return socket->send(std::string(message.constData(), static_cast<std::size_t>(message.size())));
}

bool WinHttpWebSocket::isOpen() const
{
	std::lock_guard<std::mutex> lock(socketMutex_);
	return active_ && websocket_ && websocket_->isOpen();
}

} // namespace nextcloud_talk
