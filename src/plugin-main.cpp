#include "participant-source.hpp"
#include "outgoing-media.hpp"
#include "talk-dock.hpp"
#include "theme-icons.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCoreApplication>
#include <QAction>
#include <QFileInfo>
#include <QMainWindow>
#include <QPalette>
#include <QPointer>
#include <QSslSocket>
#include <QStringList>

#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-nextcloud-talk", "en-US")

namespace {

constexpr const char *DockId = "obs_nextcloud_talk_dock";
QPointer<nextcloud_talk::TalkDock> dock;
QPointer<QAction> settingsAction;
bool frontendExiting = false;

void refreshIcons()
{
	if (!dock)
		return;
	dock->refreshTheme();
	if (settingsAction) {
		const QColor color = dock->palette().color(QPalette::WindowText);
		settingsAction->setIcon(nextcloud_talk::themedSvgIcon(
			QStringLiteral("nextcloud-talk-dark.svg"), color, QSize(20, 20), dock->devicePixelRatioF()));
	}
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && dock) {
		frontendExiting = true;
		dock->shutdown();
	}
	else if (event == OBS_FRONTEND_EVENT_THEME_CHANGED)
		refreshIcons();
}

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Nextcloud Talk plugin for OBS";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Nextcloud Talk plugin for OBS";
}

bool obs_module_load(void)
{
	frontendExiting = false;
	const char *modulePath = obs_get_module_binary_path(obs_current_module());
	if (modulePath) {
		const QString pluginDirectory = QFileInfo(QString::fromUtf8(modulePath)).absolutePath();
		QCoreApplication::addLibraryPath(pluginDirectory);
	}

	const QStringList tlsBackends = QSslSocket::availableBackends();
	blog(tlsBackends.isEmpty() ? LOG_ERROR : LOG_INFO, "[nextcloud-talk] TLS backends: %s",
	     tlsBackends.isEmpty() ? "none" : tlsBackends.join(QStringLiteral(", ")).toUtf8().constData());

	nextcloud_talk::registerParticipantSourceTypes();
	nextcloud_talk::registerOutgoingMediaOutput();

	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock = new nextcloud_talk::TalkDock(mainWindow);
	if (!obs_frontend_add_dock_by_id(DockId, obs_module_text("Dock.Title"), dock)) {
		blog(LOG_ERROR, "[nextcloud-talk] Could not register dock; the ID is already in use");
		delete dock.data();
		dock.clear();
		return false;
	}
	settingsAction = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction("Nextcloud Talk plugin for OBS Settings"));
	if (settingsAction) {
		refreshIcons();
		QObject::connect(settingsAction, &QAction::triggered, [] {
			if (dock)
				dock->showSettings();
		});
	}

	obs_frontend_add_event_callback(frontendEvent, nullptr);
	blog(LOG_INFO, "[nextcloud-talk] Loaded version %s", NEXTCLOUD_TALK_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	if (dock) {
		dock->shutdown();
		// Keep the dock registered during application shutdown so OBS includes
		// its stable DockId in the persisted main-window layout.
		if (!frontendExiting)
			obs_frontend_remove_dock(DockId);
		dock.clear();
	}
	if (settingsAction)
		settingsAction->setEnabled(false);
	settingsAction.clear();
	blog(LOG_INFO, "[nextcloud-talk] Unloaded");
}
