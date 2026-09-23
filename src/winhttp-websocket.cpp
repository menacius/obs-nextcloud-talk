#include "winhttp-websocket.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <array>
#include <string>

namespace nextcloud_talk {
namespace {

QString windowsMessage(unsigned long code)
{
	wchar_t *buffer = nullptr;
	const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
					    FORMAT_MESSAGE_IGNORE_INSERTS,
					    nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
	QString result = length && buffer ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed()
					  : QStringLiteral("Windows error %1").arg(code);
	if (buffer)
		LocalFree(buffer);
	return result;
}

} // namespace

WinHttpWebSocket::~WinHttpWebSocket()
{
	close();
}

void WinHttpWebSocket::open(const QString &url)
{
	close();
	active_ = true;
	worker_ = std::thread([this, url] { run(url); });
}

void WinHttpWebSocket::close()
{
	active_ = false;
	{
		std::lock_guard<std::mutex> lock(sendMutex_);
		void *handle = websocketHandle_.exchange(nullptr);
		if (handle)
			WinHttpCloseHandle(static_cast<HINTERNET>(handle));
	}
	if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
		worker_.join();
}

bool WinHttpWebSocket::sendText(const QByteArray &message)
{
	std::lock_guard<std::mutex> lock(sendMutex_);
	void *handle = websocketHandle_.load();
	if (!active_ || !handle)
		return false;
	const DWORD result = WinHttpWebSocketSend(static_cast<HINTERNET>(handle),
						  WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
						  const_cast<char *>(message.constData()),
						  static_cast<DWORD>(message.size()));
	if (result != NO_ERROR) {
		reportWindowsError(QStringLiteral("WebSocket send"), result);
		return false;
	}
	return true;
}

void WinHttpWebSocket::run(QString url)
{
	// WinHTTP's URL cracker does not recognize ws:// or wss://. Parse their
	// HTTP equivalents, then perform the normal WebSocket upgrade below.
	QString parseUrl = url.trimmed();
	bool secure = false;
	if (parseUrl.startsWith(QStringLiteral("wss://"), Qt::CaseInsensitive)) {
		parseUrl.replace(0, 6, QStringLiteral("https://"));
		secure = true;
	} else if (parseUrl.startsWith(QStringLiteral("ws://"), Qt::CaseInsensitive)) {
		parseUrl.replace(0, 5, QStringLiteral("http://"));
	} else if (parseUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
		secure = true;
	}
	const std::wstring urlText = parseUrl.toStdWString();
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	parts.dwHostNameLength = static_cast<DWORD>(-1);
	parts.dwUrlPathLength = static_cast<DWORD>(-1);
	parts.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(urlText.c_str(), 0, 0, &parts)) {
		reportWindowsError(QStringLiteral("Invalid signaling URL"), GetLastError());
		active_ = false;
		return;
	}

	const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
	std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
	if (parts.dwExtraInfoLength)
		path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	if (path.empty())
		path = L"/";

	HINTERNET session = WinHttpOpen(L"OBS Nextcloud Talk/0.2",
					WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connection = nullptr;
	HINTERNET request = nullptr;
	HINTERNET websocket = nullptr;
	if (!session) {
		reportWindowsError(QStringLiteral("WinHTTP initialization"), GetLastError());
		active_ = false;
		return;
	}
	WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
	connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
	if (!connection) {
		reportWindowsError(QStringLiteral("Signaling connection"), GetLastError());
		goto cleanup;
	}
	request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
				     WINHTTP_DEFAULT_ACCEPT_TYPES,
				     secure ? WINHTTP_FLAG_SECURE : 0);
	if (!request) {
		reportWindowsError(QStringLiteral("Signaling request"), GetLastError());
		goto cleanup;
	}
	if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
	    !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(request, nullptr)) {
		reportWindowsError(QStringLiteral("WebSocket upgrade"), GetLastError());
		goto cleanup;
	}
	websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
	if (!websocket) {
		reportWindowsError(QStringLiteral("WebSocket upgrade"), GetLastError());
		goto cleanup;
	}
	WinHttpCloseHandle(request);
	request = nullptr;
	websocketHandle_ = websocket;
	if (onOpen)
		onOpen();

	{
		std::array<char, 64 * 1024> buffer{};
		QByteArray message;
		while (active_) {
			DWORD bytesRead = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
			const DWORD result = WinHttpWebSocketReceive(websocket, buffer.data(),
							    static_cast<DWORD>(buffer.size()), &bytesRead, &type);
			if (result != NO_ERROR) {
				if (active_)
					reportWindowsError(QStringLiteral("WebSocket receive"), result);
				break;
			}
			if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
				break;
			if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
			    type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
				message.append(buffer.data(), static_cast<qsizetype>(bytesRead));
				if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
					if (onTextMessage)
						onTextMessage(message);
					message.clear();
				}
			}
		}
	}

cleanup:
	active_ = false;
	{
		std::lock_guard<std::mutex> lock(sendMutex_);
		if (websocketHandle_.exchange(nullptr) == websocket && websocket)
			WinHttpCloseHandle(websocket);
	}
	if (request)
		WinHttpCloseHandle(request);
	if (connection)
		WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	if (onClosed)
		onClosed();
}

void WinHttpWebSocket::reportWindowsError(const QString &operation, unsigned long code)
{
	if (onError)
		onError(QStringLiteral("%1: %2").arg(operation, windowsMessage(code)));
}

} // namespace nextcloud_talk
