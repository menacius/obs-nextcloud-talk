#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace nextcloud_talk {

QPixmap themedSvgPixmap(const QString &fileName, const QColor &color, const QSize &size,
			qreal devicePixelRatio = 1.0);
QIcon themedSvgIcon(const QString &fileName, const QColor &color, const QSize &size = QSize(24, 24),
		    qreal devicePixelRatio = 1.0);

} // namespace nextcloud_talk
