#pragma once

#include <QWidget>

#include <memory>

struct obs_source;
using obs_source_t = struct obs_source;

namespace nextcloud_talk {

// A small native OBS display used by the settings dialog to show exactly the
// video source selected for publication.
class OutgoingPreview final : public QWidget {
public:
	explicit OutgoingPreview(QWidget *parent = nullptr);
	~OutgoingPreview() override;

	void setSource(obs_source_t *source);

protected:
	QPaintEngine *paintEngine() const override;
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace nextcloud_talk
