#include "outgoing-preview.hpp"

#include <obs.h>

#include <QPaintEngine>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QtMath>
#include <QWindow>

#include <algorithm>
#include <mutex>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

namespace nextcloud_talk {

class OutgoingPreview::Impl {
public:
	explicit Impl(OutgoingPreview *widget) : widget_(widget) {}

	~Impl()
	{
		if (display_) {
			obs_display_remove_draw_callback(display_, draw, this);
			obs_display_destroy(display_);
		}
		setSource(nullptr);
	}

	void createDisplay()
	{
		if (display_ || !widget_->windowHandle() || !widget_->windowHandle()->isExposed())
			return;
		const qreal ratio = widget_->devicePixelRatioF();
		gs_init_data info = {};
		info.cx = std::max(1, qRound(widget_->width() * ratio));
		info.cy = std::max(1, qRound(widget_->height() * ratio));
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
		info.window.hwnd = reinterpret_cast<HWND>(widget_->winId());
#elif defined(__APPLE__)
		info.window.view = reinterpret_cast<void *>(widget_->winId());
#else
		if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL)
			return;
		info.window.id = widget_->winId();
		info.window.display = obs_get_nix_platform_display();
#endif
		display_ = obs_display_create(&info, 0xff202020);
		if (display_)
			obs_display_add_draw_callback(display_, draw, this);
	}

	void resize()
	{
		if (!display_)
			return;
		const qreal ratio = widget_->devicePixelRatioF();
		obs_display_resize(display_, std::max(1, qRound(widget_->width() * ratio)),
				   std::max(1, qRound(widget_->height() * ratio)));
	}

	void setSource(obs_source_t *source)
	{
		if (source)
			obs_source_get_ref(source);
		std::lock_guard<std::mutex> lock(mutex_);
		if (source == source_) {
			if (source)
				obs_source_release(source);
			return;
		}
		if (source)
			obs_source_inc_showing(source);
		if (source_) {
			obs_source_dec_showing(source_);
			obs_source_release(source_);
		}
		source_ = source;
	}

private:
	static void draw(void *data, uint32_t width, uint32_t height)
	{
		auto *preview = static_cast<Impl *>(data);
		std::lock_guard<std::mutex> lock(preview->mutex_);
		if (!preview->source_)
			return;
		const uint32_t sourceWidth = std::max(1u, obs_source_get_width(preview->source_));
		const uint32_t sourceHeight = std::max(1u, obs_source_get_height(preview->source_));
		const float scale = std::min(static_cast<float>(width) / sourceWidth,
					     static_cast<float>(height) / sourceHeight);
		const int drawWidth = std::max(1, static_cast<int>(sourceWidth * scale));
		const int drawHeight = std::max(1, static_cast<int>(sourceHeight * scale));
		const int x = (static_cast<int>(width) - drawWidth) / 2;
		const int y = (static_cast<int>(height) - drawHeight) / 2;

		gs_viewport_push();
		gs_projection_push();
		const bool previousSrgb = gs_set_linear_srgb(true);
		gs_ortho(0.0f, static_cast<float>(sourceWidth), 0.0f, static_cast<float>(sourceHeight),
			 -100.0f, 100.0f);
		gs_set_viewport(x, y, drawWidth, drawHeight);
		obs_source_video_render(preview->source_);
		gs_set_linear_srgb(previousSrgb);
		gs_projection_pop();
		gs_viewport_pop();
	}

	OutgoingPreview *widget_ = nullptr;
	obs_display_t *display_ = nullptr;
	obs_source_t *source_ = nullptr;
	std::mutex mutex_;
};

OutgoingPreview::OutgoingPreview(QWidget *parent) : QWidget(parent), impl_(std::make_unique<Impl>(this))
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(320, 180);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

OutgoingPreview::~OutgoingPreview() = default;

void OutgoingPreview::setSource(obs_source_t *source)
{
	impl_->setSource(source);
}

QPaintEngine *OutgoingPreview::paintEngine() const
{
	return nullptr;
}

void OutgoingPreview::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event);
	impl_->createDisplay();
}

void OutgoingPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	impl_->resize();
}

void OutgoingPreview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	impl_->createDisplay();
	impl_->resize();
}

} // namespace nextcloud_talk
