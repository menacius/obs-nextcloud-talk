#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <iostream>

namespace {

bool iconHasClearBorder(const QString &path, int size)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;
	QByteArray svg = file.readAll();
	svg.replace("currentColor", "#ffffff");
	QSvgRenderer renderer(svg);
	if (!renderer.isValid())
		return false;
	QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	renderer.render(&painter, QRectF(0, 0, size, size));
	painter.end();

	bool hasVisiblePixel = false;
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			const bool visible = qAlpha(image.pixel(x, y)) != 0;
			hasVisiblePixel = hasVisiblePixel || visible;
			if (visible && (x == 0 || y == 0 || x == size - 1 || y == size - 1))
				return false;
		}
	}
	return hasVisiblePixel;
}

} // namespace

int main()
{
	const QString iconDirectory = QStringLiteral(NEXTCLOUD_TALK_SOURCE_DIR "/data/icons/");
	if (!iconHasClearBorder(iconDirectory + QStringLiteral("nextcloud-talk-dark.svg"), 20)) {
		std::cerr << "FAIL: application icon touches its raster boundary\n";
		return 1;
	}
	if (!iconHasClearBorder(iconDirectory + QStringLiteral("Talk.svg"), 16)) {
		std::cerr << "FAIL: speaking icon touches its raster boundary\n";
		return 1;
	}
	if (!iconHasClearBorder(iconDirectory + QStringLiteral("NoTalk.svg"), 16)) {
		std::cerr << "FAIL: not-speaking icon touches its raster boundary\n";
		return 1;
	}
	std::cout << "Icon raster boundary tests passed\n";
	return 0;
}
