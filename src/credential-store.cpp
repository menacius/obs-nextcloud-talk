#include "credential-store.hpp"

#include <QCryptographicHash>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#endif

namespace nextcloud_talk {
namespace {

#ifdef Q_OS_WIN
QString windowsError(DWORD code)
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
#endif

} // namespace

QString CredentialStore::targetName(const QString &serverUrl, const QString &username)
{
	QString normalizedServer = serverUrl.trimmed().toLower();
	while (normalizedServer.endsWith('/'))
		normalizedServer.chop(1);
	const QByteArray identity = (normalizedServer + QLatin1Char('\n') + username.trimmed()).toUtf8();
	const QByteArray digest = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
	return QStringLiteral("OBSNextcloudTalk:%1").arg(QString::fromLatin1(digest));
}

QString CredentialStore::load(const QString &serverUrl, const QString &username, bool *found, QString *error)
{
	if (found)
		*found = false;
	if (error)
		error->clear();
	if (serverUrl.trimmed().isEmpty() || username.trimmed().isEmpty())
		return {};

#ifdef Q_OS_WIN
	const std::wstring target = targetName(serverUrl, username).toStdWString();
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
		const DWORD code = GetLastError();
		if (code != ERROR_NOT_FOUND && error)
			*error = windowsError(code);
		return {};
	}
	const QString password = QString::fromWCharArray(
		reinterpret_cast<const wchar_t *>(credential->CredentialBlob),
		static_cast<qsizetype>(credential->CredentialBlobSize / sizeof(wchar_t)));
	CredFree(credential);
	if (found)
		*found = true;
	return password;
#else
	if (error)
		*error = QStringLiteral("Secure credential storage is not implemented on this platform.");
	return {};
#endif
}

bool CredentialStore::save(const QString &serverUrl, const QString &username, const QString &password,
			   QString *error)
{
	if (error)
		error->clear();
	if (serverUrl.trimmed().isEmpty() || username.trimmed().isEmpty() || password.isEmpty()) {
		if (error)
			*error = QStringLiteral("Server, username and app password are required.");
		return false;
	}

#ifdef Q_OS_WIN
	const std::wstring target = targetName(serverUrl, username).toStdWString();
	const std::wstring user = username.trimmed().toStdWString();
	const std::wstring secret = password.toStdWString();
	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<wchar_t *>(target.c_str());
	credential.CredentialBlobSize = static_cast<DWORD>(secret.size() * sizeof(wchar_t));
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t *>(secret.data()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = const_cast<wchar_t *>(user.c_str());
	if (!CredWriteW(&credential, 0)) {
		if (error)
			*error = windowsError(GetLastError());
		return false;
	}
	return true;
#else
	if (error)
		*error = QStringLiteral("Secure credential storage is not implemented on this platform.");
	return false;
#endif
}

bool CredentialStore::remove(const QString &serverUrl, const QString &username, QString *error)
{
	if (error)
		error->clear();
	if (serverUrl.trimmed().isEmpty() || username.trimmed().isEmpty())
		return true;

#ifdef Q_OS_WIN
	const std::wstring target = targetName(serverUrl, username).toStdWString();
	if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
		const DWORD code = GetLastError();
		if (code == ERROR_NOT_FOUND)
			return true;
		if (error)
			*error = windowsError(code);
		return false;
	}
	return true;
#else
	if (error)
		*error = QStringLiteral("Secure credential storage is not implemented on this platform.");
	return false;
#endif
}

} // namespace nextcloud_talk
