#pragma once

#ifndef NEXTCLOUD_TALK_TESTING
#include <obs-module.h>
#endif

#include <QDebug>
#include <QString>

#include <optional>

namespace nextcloud_talk {

#ifdef NEXTCLOUD_TALK_TESTING
inline constexpr int LOG_INFO = 200;
inline constexpr int LOG_WARNING = 300;
inline constexpr int LOG_ERROR = 400;
#endif

// Streams a Qt-formatted line into OBS's own log when the temporary is destroyed.
class ObsLogLine final {
public:
	explicit ObsLogLine(int level) : level_(level)
	{
		stream_.emplace(&text_);
		stream_->noquote();
	}

	~ObsLogLine()
	{
		stream_.reset();
		const QByteArray encoded = text_.trimmed().toUtf8();
#ifdef NEXTCLOUD_TALK_TESTING
		qInfo("%s", encoded.constData());
#else
		blog(level_, "%s", encoded.constData());
#endif
	}

	ObsLogLine(const ObsLogLine &) = delete;
	ObsLogLine &operator=(const ObsLogLine &) = delete;

	template<typename T> ObsLogLine &operator<<(const T &value)
	{
		*stream_ << value;
		return *this;
	}

private:
	QString text_;
	std::optional<QDebug> stream_;
	int level_;
};

} // namespace nextcloud_talk
