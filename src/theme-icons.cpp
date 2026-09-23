#include "theme-icons.hpp"

#include <obs-module.h>

#include <QFile>
#include <QPainter>
#include <QSvgRenderer>

#include <cmath>

namespace nextcloud_talk {

QPixmap themedSvgPixmap(const QString &fileName, const QColor &color, const QSize &size, qreal devicePixelRatio)
{
	char *path = obs_module_file((QStringLiteral("icons/") + fileName).toUtf8().constData());
	if (!path)
		return {};
	QFile file(QString::fromUtf8(path));
	bfree(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};

	QByteArray svg = file.readAll();
	const QByteArray themedColor = color.name(QColor::HexRgb).toUtf8();
	svg.replace("currentColor", themedColor);
	const qsizetype svgTagEnd = svg.indexOf('>');
	if (svgTagEnd > 0)
		svg.insert(svgTagEnd, QByteArrayLiteral(" fill=\"") + themedColor + QByteArrayLiteral("\""));

	QSvgRenderer renderer(svg);
	if (!renderer.isValid())
		return {};
	const QSize pixelSize(std::max(1, static_cast<int>(std::ceil(size.width() * devicePixelRatio))),
			      std::max(1, static_cast<int>(std::ceil(size.height() * devicePixelRatio))));
	QPixmap pixmap(pixelSize);
	pixmap.setDevicePixelRatio(devicePixelRatio);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	// QPainter uses device-independent coordinates once the pixmap DPR is set.
	// Rendering into pixelSize here would scale twice and crop the SVG.
	renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
	return pixmap;
}

QIcon themedSvgIcon(const QString &fileName, const QColor &color, const QSize &size, qreal devicePixelRatio)
{
	return QIcon(themedSvgPixmap(fileName, color, size, devicePixelRatio));
}

} // namespace nextcloud_talk
