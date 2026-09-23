#pragma once

#include <QString>

namespace nextcloud_talk {

// Stores the app password in the operating-system credential vault. Passwords
// are never serialized into the OBS plugin settings file.
class CredentialStore final {
public:
	static QString load(const QString &serverUrl, const QString &username, bool *found = nullptr,
			    QString *error = nullptr);
	static bool save(const QString &serverUrl, const QString &username, const QString &password,
			 QString *error = nullptr);
	static bool remove(const QString &serverUrl, const QString &username, QString *error = nullptr);

private:
	static QString targetName(const QString &serverUrl, const QString &username);
};

} // namespace nextcloud_talk
