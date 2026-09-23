#include "talk-dock.hpp"

#include "credential-store.hpp"
#include "obs-log.hpp"
#include "outgoing-preview.hpp"
#include "participant-source.hpp"
#include "theme-icons.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace nextcloud_talk {
namespace {

QString stateLabel(ConnectionState state)
{
	switch (state) {
	case ConnectionState::Disconnected:
		return QStringLiteral("Disconnected");
	case ConnectionState::LoadingConversations:
		return QStringLiteral("Connecting");
	case ConnectionState::Ready:
		return QStringLiteral("Ready");
	case ConnectionState::Joining:
		return QStringLiteral("Joining");
	case ConnectionState::InCall:
		return QStringLiteral("In call");
	case ConnectionState::Leaving:
		return QStringLiteral("Leaving");
	case ConnectionState::Error:
		return QStringLiteral("Error");
	}
	return QStringLiteral("Unknown");
}

struct OutputLists {
	QStringList video;
	QStringList audio;
};

bool isVideoCaptureDevice(const QString &id)
{
	return id == QStringLiteral("dshow_input") || id == QStringLiteral("v4l2_input") ||
	       id == QStringLiteral("av_capture_input") || id == QStringLiteral("macos-avcapture") ||
	       id == QStringLiteral("macos-avcapture-fast");
}

bool isAudioInputCapture(const QString &id)
{
	return id == QStringLiteral("wasapi_input_capture") || id == QStringLiteral("coreaudio_input_capture") ||
	       id == QStringLiteral("pulse_input_capture") || id == QStringLiteral("alsa_input_capture");
}

bool enumerateSource(void *data, obs_source_t *source)
{
	auto *lists = static_cast<OutputLists *>(data);
	const char *name = obs_source_get_name(source);
	const char *id = obs_source_get_unversioned_id(source);
	if (!id)
		id = obs_source_get_id(source);
	if (!name || !id)
		return true;

	const QString sourceId = QString::fromUtf8(id);
	if (isVideoCaptureDevice(sourceId))
		lists->video.push_back(QString::fromUtf8(name));
	if (isAudioInputCapture(sourceId))
		lists->audio.push_back(QString::fromUtf8(name));
	return true;
}

} // namespace

TalkDock::TalkDock(QWidget *parent) : QWidget(parent)
{
	buildUi();
	bindEvents();
	loadConfig();
	refreshOutputChoices();
	// Scene collection sources are restored after modules and docks are created.
	// Retry once that restoration has completed, while retaining the configured names.
	QTimer::singleShot(1500, this, [this] { refreshOutputChoices(); });
	applyState(ConnectionState::Disconnected, QStringLiteral("Enter a Nextcloud app password, then connect."));
}

TalkDock::~TalkDock()
{
	shutdown();
}

void TalkDock::save()
{
	currentConfig().save();
}

void TalkDock::shutdown()
{
	if (shutdown_)
		return;
	shutdown_ = true;
	save();
	mediaTransport_.stop();
}

void TalkDock::showSettings()
{
	refreshOutputChoices();
	loadStoredCredential();
	settingsDialog_->show();
	settingsDialog_->raise();
	settingsDialog_->activateWindow();
}

void TalkDock::buildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);
	auto *header = new QHBoxLayout;
	titleIcon_ = new QLabel(this);
	titleIcon_->setFixedSize(24, 24);
	titleIcon_->setAlignment(Qt::AlignCenter);
	auto *title = new QLabel(QStringLiteral("Nextcloud Talk plugin for OBS"), this);
	auto titleFont = title->font();
	titleFont.setBold(true);
	titleFont.setPointSize(titleFont.pointSize() + 2);
	title->setFont(titleFont);
	auto *version = new QLabel(QStringLiteral("v%1").arg(QStringLiteral(NEXTCLOUD_TALK_VERSION)), this);
	version->setForegroundRole(QPalette::PlaceholderText);
	header->addWidget(titleIcon_);
	header->addWidget(title);
	header->addStretch(1);
	header->addWidget(version);
	root->addLayout(header);

	settingsDialog_ = new QDialog(this);
	settingsDialog_->setWindowTitle(
		QStringLiteral("Nextcloud Talk plugin for OBS Settings — v%1").arg(QStringLiteral(NEXTCLOUD_TALK_VERSION)));
	settingsDialog_->setModal(false);
	settingsDialog_->resize(640, 650);
	auto *settingsLayout = new QVBoxLayout(settingsDialog_);
	auto *settingsHeader = new QHBoxLayout;
	settingsIcon_ = new QLabel(settingsDialog_);
	settingsIcon_->setFixedSize(24, 24);
	settingsIcon_->setAlignment(Qt::AlignCenter);
	auto *settingsTitle = new QLabel(QStringLiteral("Nextcloud Talk plugin for OBS"), settingsDialog_);
	auto settingsTitleFont = settingsTitle->font();
	settingsTitleFont.setBold(true);
	settingsTitle->setFont(settingsTitleFont);
	auto *settingsVersion =
		new QLabel(QStringLiteral("v%1").arg(QStringLiteral(NEXTCLOUD_TALK_VERSION)), settingsDialog_);
	settingsVersion->setForegroundRole(QPalette::PlaceholderText);
	settingsHeader->addWidget(settingsIcon_);
	settingsHeader->addWidget(settingsTitle);
	settingsHeader->addStretch(1);
	settingsHeader->addWidget(settingsVersion);
	settingsLayout->addLayout(settingsHeader);

	auto *accountBox = new QGroupBox(QStringLiteral("Nextcloud account"), settingsDialog_);
	auto *accountForm = new QFormLayout(accountBox);
	serverUrl_ = new QLineEdit(accountBox);
	serverUrl_->setPlaceholderText(QStringLiteral("https://cloud.example.com"));
	username_ = new QLineEdit(accountBox);
	appPassword_ = new QLineEdit(accountBox);
	appPassword_->setEchoMode(QLineEdit::Password);
	appPassword_->setPlaceholderText(QStringLiteral("Stored securely in Windows Credential Manager"));
	forgetPassword_ = new QPushButton(QStringLiteral("Forget saved"), accountBox);
	forgetPassword_->setEnabled(false);
	auto *passwordRow = new QWidget(accountBox);
	auto *passwordLayout = new QHBoxLayout(passwordRow);
	passwordLayout->setContentsMargins(0, 0, 0, 0);
	passwordLayout->addWidget(appPassword_, 1);
	passwordLayout->addWidget(forgetPassword_);
	accountForm->addRow(QStringLiteral("Server"), serverUrl_);
	accountForm->addRow(QStringLiteral("Username"), username_);
	accountForm->addRow(QStringLiteral("App password"), passwordRow);
	settingsLayout->addWidget(accountBox);

	auto *outputBox = new QGroupBox(QStringLiteral("Outgoing media"), settingsDialog_);
	auto *outputForm = new QFormLayout(outputBox);
	outgoingMode_ = new QComboBox(outputBox);
	outgoingMode_->addItem(QStringLiteral("Video / audio devices"), QStringLiteral("devices"));
	outgoingMode_->addItem(QStringLiteral("OBS scene"), QStringLiteral("scene"));
	outgoingMode_->addItem(QStringLiteral("OBS Preview"), QStringLiteral("preview"));
	outgoingMode_->addItem(QStringLiteral("OBS Program"), QStringLiteral("program"));
	outgoingVideo_ = new QComboBox(outputBox);
	outgoingAudio_ = new QComboBox(outputBox);
	outgoingScene_ = new QComboBox(outputBox);
	outgoingVideoLabel_ = new QLabel(QStringLiteral("Video Capture Device"), outputBox);
	outgoingAudioLabel_ = new QLabel(QStringLiteral("Audio Input Capture"), outputBox);
	outgoingSceneLabel_ = new QLabel(QStringLiteral("Scene"), outputBox);
	outgoingPreview_ = new OutgoingPreview(outputBox);
	refreshOutputs_ = new QPushButton(QStringLiteral("Refresh OBS sources"), outputBox);
	outputForm->addRow(QStringLiteral("Source"), outgoingMode_);
	outputForm->addRow(outgoingVideoLabel_, outgoingVideo_);
	outputForm->addRow(outgoingAudioLabel_, outgoingAudio_);
	outputForm->addRow(outgoingSceneLabel_, outgoingScene_);
	outputForm->addRow(QStringLiteral("Outgoing preview"), outgoingPreview_);
	outputForm->addRow(refreshOutputs_);
	settingsLayout->addWidget(outputBox);
	auto *settingsButtons = new QDialogButtonBox(QDialogButtonBox::Close, settingsDialog_);
	settingsLayout->addWidget(settingsButtons);
	connect(settingsButtons, &QDialogButtonBox::rejected, this, [this] {
		save();
		settingsDialog_->hide();
	});

	auto *connectionBox = new QGroupBox(QStringLiteral("Nextcloud connection"), this);
	auto *connectionLayout = new QVBoxLayout(connectionBox);
	connectButton_ = new QPushButton(QStringLiteral("Connect / Refresh"), connectionBox);
	connectionLayout->addWidget(connectButton_);
	root->addWidget(connectionBox);

	auto *callBox = new QGroupBox(QStringLiteral("Call"), this);
	callBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	auto *callLayout = new QVBoxLayout(callBox);
	conversation_ = new QComboBox(callBox);
	conversation_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	joinButton_ = new QPushButton(QStringLiteral("Join call"), callBox);
	status_ = new QLabel(callBox);
	status_->setWordWrap(true);
	participants_ = new QListWidget(callBox);
	participants_->setContextMenuPolicy(Qt::CustomContextMenu);
	participants_->setMinimumHeight(72);
	participants_->setMaximumHeight(120);
	callLayout->addWidget(conversation_);
	callLayout->addWidget(joinButton_);
	callLayout->addWidget(status_);
	callLayout->addWidget(new QLabel(QStringLiteral("Remote participants"), callBox));
	callLayout->addWidget(participants_);
	root->addWidget(callBox);

	auto *chatBox = new QGroupBox(QStringLiteral("Call chat"), this);
	auto *chatLayout = new QVBoxLayout(chatBox);
	chat_ = new QTextBrowser(chatBox);
	chat_->setOpenExternalLinks(true);
	chat_->setMinimumHeight(80);
	chat_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	chatStatus_ = new QLabel(QStringLiteral("Join a call to use its chat."), chatBox);
	chatStatus_->setWordWrap(true);
	chatStatus_->setStyleSheet(QStringLiteral("color: palette(mid);"));
	auto *chatComposer = new QHBoxLayout;
	chatInput_ = new QLineEdit(chatBox);
	chatInput_->setPlaceholderText(QStringLiteral("Write a message…"));
	chatSend_ = new QPushButton(QStringLiteral("Send"), chatBox);
	chatComposer->addWidget(chatInput_, 1);
	chatComposer->addWidget(chatSend_);
	chatLayout->addWidget(chat_, 1);
	chatLayout->addWidget(chatStatus_);
	chatLayout->addLayout(chatComposer);
	root->addWidget(chatBox, 1);
	chatBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

	feedbackGuard_ = new QCheckBox(QStringLiteral("Exclude Talk participant audio (feedback guard)"), this);
	root->addWidget(feedbackGuard_);
	resetChat();

	connect(refreshOutputs_, &QPushButton::clicked, this, [this] { refreshOutputChoices(); });
	outgoingPreviewTimer_ = new QTimer(this);
	outgoingPreviewTimer_->setInterval(500);
	connect(outgoingPreviewTimer_, &QTimer::timeout, this, [this] {
		if (settingsDialog_->isVisible())
			updateOutgoingPreview();
	});
	outgoingPreviewTimer_->start();
	refreshTheme();
}

void TalkDock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
	    event->type() == QEvent::StyleChange)
		refreshTheme();
}

void TalkDock::refreshTheme()
{
	const QColor color = titleIcon_ ? titleIcon_->palette().color(QPalette::WindowText)
					 : palette().color(QPalette::WindowText);
	const qreal ratio = devicePixelRatioF();
	if (titleIcon_)
		titleIcon_->setPixmap(themedSvgPixmap(QStringLiteral("nextcloud-talk-dark.svg"), color,
						      QSize(20, 20), ratio));
	if (settingsIcon_)
		settingsIcon_->setPixmap(themedSvgPixmap(QStringLiteral("nextcloud-talk-dark.svg"), color,
							 QSize(20, 20), ratio));
	setWindowIcon(themedSvgIcon(QStringLiteral("nextcloud-talk-dark.svg"), color, QSize(32, 32), ratio));
	if (participants_)
		refreshParticipantList();
}

void TalkDock::bindEvents()
{
	connect(connectButton_, &QPushButton::clicked, this, [this] { connectToServer(); });
	connect(joinButton_, &QPushButton::clicked, this, [this] { joinOrLeave(); });
	connect(participants_, &QListWidget::customContextMenuRequested, this,
		[this](const QPoint &position) { showParticipantContextMenu(position); });
	connect(chatSend_, &QPushButton::clicked, this, [this] { sendChatMessage(); });
	connect(chatInput_, &QLineEdit::returnPressed, this, [this] { sendChatMessage(); });
	connect(forgetPassword_, &QPushButton::clicked, this, [this] { forgetStoredCredential(); });
	auto accountEdited = [this] {
		appPassword_->clear();
		storedCredentialAvailable_ = false;
		forgetPassword_->setEnabled(false);
	};
	connect(serverUrl_, &QLineEdit::textEdited, this, accountEdited);
	connect(username_, &QLineEdit::textEdited, this, accountEdited);
	connect(serverUrl_, &QLineEdit::editingFinished, this, [this] { loadStoredCredential(); });
	connect(username_, &QLineEdit::editingFinished, this, [this] { loadStoredCredential(); });
	connect(feedbackGuard_, &QCheckBox::toggled, this, [this] {
		refreshOutputChoices();
		save();
	});
	connect(conversation_, &QComboBox::currentIndexChanged, this, [this] {
		selectedConversationToken_ = conversation_->currentData().toString();
		updateCallAction();
		save();
	});
	connect(outgoingMode_, &QComboBox::currentIndexChanged, this, [this] {
		selectedOutgoingMode_ = outgoingMode_->currentData().toString();
		updateOutgoingControls();
		updateOutgoingPreview();
		save();
	});
	connect(outgoingVideo_, &QComboBox::currentIndexChanged, this, [this] {
		selectedVideoSource_ = outgoingVideo_->currentData().toString();
		updateOutgoingPreview();
		save();
	});
	connect(outgoingAudio_, &QComboBox::currentIndexChanged, this, [this] {
		selectedAudioSource_ = outgoingAudio_->currentData().toString();
		save();
	});
	connect(outgoingScene_, &QComboBox::currentIndexChanged, this, [this] {
		selectedScene_ = outgoingScene_->currentData().toString();
		updateOutgoingPreview();
		save();
	});

	client_.onStateChanged = [this](ConnectionState state, const QString &detail) {
		applyState(state, detail);
	};
	client_.onConversationsChanged = [this](const std::vector<Conversation> &items) {
		applyConversations(items);
	};
	client_.onParticipantsChanged = [this](const std::vector<Participant> &items) {
		applyParticipants(items);
	};
	client_.onChatMessages = [this](const std::vector<ChatMessage> &messages) {
		appendChatMessages(messages);
	};
	client_.onChatReset = [this] { resetChat(); };
	client_.onChatSendFinished = [this](bool ok, const QString &error) {
		const bool inCall = client_.state() == ConnectionState::InCall;
		if (!inCall) {
			resetChat();
			return;
		}
		chatInput_->setEnabled(inCall);
		chatSend_->setEnabled(inCall);
		chatStatus_->setText(ok ? QString() : error);
	};
	client_.onMediaSessionReady =
		[this](const QString &serverUrl, const QString &token, const QString &roomSessionId,
		       const SignalingSettings &settings) {
			const OutgoingMediaSelection selection = outgoingSelection();
			ObsLogLine(LOG_INFO) << "[nextcloud-talk] Starting media session with outgoing mode"
					  << outgoingMediaModeId(selection.mode);
			mediaTransport_.start(serverUrl, token, roomSessionId, settings, selection);
		};
	client_.onMediaSessionEnded = [this] { mediaTransport_.stop(); };
	mediaTransport_.onStatusChanged = [this](const QString &detail) {
		if (client_.state() == ConnectionState::InCall)
			status_->setText(detail);
	};
	mediaTransport_.onPublishingReady = [this] {
		if (client_.state() == ConnectionState::InCall)
			client_.updateCallFlags(7);
	};
	mediaTransport_.onParticipantMediaState = [this](const ParticipantMediaState &state) {
		registry_.updateMediaState(state);
	};
	registry_.onChanged = [this] {
		refreshParticipantList();
	};
}

void TalkDock::loadConfig()
{
	loadedConfig_ = PluginConfig::load();
	selectedConversationToken_ = loadedConfig_.conversationToken;
	selectedOutgoingMode_ = outgoingMediaModeId(outgoingMediaModeFromId(loadedConfig_.outgoingMode));
	selectedVideoSource_ = loadedConfig_.outgoingVideoSource;
	selectedAudioSource_ = loadedConfig_.outgoingAudioSource;
	selectedScene_ = loadedConfig_.outgoingScene;
	serverUrl_->setText(loadedConfig_.serverUrl);
	username_->setText(loadedConfig_.username);
	feedbackGuard_->setChecked(loadedConfig_.feedbackGuard);
	loadStoredCredential();
}

void TalkDock::loadStoredCredential()
{
	if (!appPassword_->text().isEmpty())
		return;
	bool found = false;
	QString error;
	const QString password = CredentialStore::load(serverUrl_->text(), username_->text(), &found, &error);
	if (!error.isEmpty())
		ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Could not read saved credential:" << error;
	storedCredentialAvailable_ = found;
	forgetPassword_->setEnabled(found);
	if (found) {
		appPassword_->setText(password);
		appPassword_->setToolTip(QStringLiteral("Loaded from Windows Credential Manager"));
	}
}

bool TalkDock::storeCredentialForCurrentAccount()
{
	if (appPassword_->text().isEmpty())
		loadStoredCredential();
	if (appPassword_->text().isEmpty())
		return true;
	QString error;
	if (!CredentialStore::save(serverUrl_->text(), username_->text(), appPassword_->text(), &error)) {
		applyState(ConnectionState::Error,
			   QStringLiteral("Could not save the app password securely: %1").arg(error));
		return false;
	}
	storedCredentialAvailable_ = true;
	forgetPassword_->setEnabled(true);
	appPassword_->setToolTip(QStringLiteral("Saved in Windows Credential Manager"));
	return true;
}

void TalkDock::forgetStoredCredential()
{
	QString error;
	if (!CredentialStore::remove(serverUrl_->text(), username_->text(), &error)) {
		applyState(ConnectionState::Error,
			   QStringLiteral("Could not remove the saved app password: %1").arg(error));
		return;
	}
	storedCredentialAvailable_ = false;
	forgetPassword_->setEnabled(false);
	appPassword_->clear();
	appPassword_->setToolTip({});
	applyState(ConnectionState::Disconnected, QStringLiteral("Saved app password removed."));
}

void TalkDock::connectToServer()
{
	if (!storeCredentialForCurrentAccount())
		return;
	client_.configure(serverUrl_->text(), username_->text(), appPassword_->text());
	client_.listConversations();
	save();
}

void TalkDock::joinOrLeave()
{
	if (client_.state() == ConnectionState::InCall) {
		client_.leaveCall();
		registry_.markAllOffline();
		return;
	}
	// Sources can be loaded after the dock during OBS startup. Re-resolve saved
	// source names at the moment the user joins so a valid saved choice is not
	// mistaken for None.
	refreshOutputChoices();
	const OutgoingMediaSelection selection = outgoingSelection();
	obs_source_t *videoSource = resolveOutgoingVideoSource(selection);
	const bool audioMissing = selection.mode == OutgoingMediaMode::Devices && selection.audioSourceName.isEmpty();
	if (!videoSource || audioMissing) {
		if (videoSource)
			obs_source_release(videoSource);
		applyState(ConnectionState::Error, QStringLiteral("Select a valid outgoing media source before joining."));
		return;
	}
	obs_source_release(videoSource);
	if (!storeCredentialForCurrentAccount())
		return;
	client_.configure(serverUrl_->text(), username_->text(), appPassword_->text());
	client_.joinCall(conversation_->currentData().toString(),
			 conversation_->currentData(Qt::UserRole + 1).toBool());
}

void TalkDock::applyState(ConnectionState state, const QString &detail)
{
	status_->setText(QStringLiteral("%1 — %2").arg(stateLabel(state), detail));
	const bool busy = state == ConnectionState::LoadingConversations || state == ConnectionState::Joining ||
			  state == ConnectionState::Leaving;
	connectButton_->setEnabled(!busy && state != ConnectionState::InCall);
	serverUrl_->setEnabled(!busy && state != ConnectionState::InCall);
	username_->setEnabled(!busy && state != ConnectionState::InCall);
	appPassword_->setEnabled(!busy && state != ConnectionState::InCall);
	forgetPassword_->setEnabled(!busy && state != ConnectionState::InCall && storedCredentialAvailable_);
	conversation_->setEnabled(!busy && state != ConnectionState::InCall);
	outgoingVideo_->setEnabled(!busy && state != ConnectionState::InCall);
	outgoingAudio_->setEnabled(!busy && state != ConnectionState::InCall);
	outgoingMode_->setEnabled(!busy && state != ConnectionState::InCall);
	outgoingScene_->setEnabled(!busy && state != ConnectionState::InCall);
	refreshOutputs_->setEnabled(!busy && state != ConnectionState::InCall);
	chatInput_->setEnabled(state == ConnectionState::InCall);
	chatSend_->setEnabled(state == ConnectionState::InCall);
	if (state == ConnectionState::InCall && chatStatus_->text() == QStringLiteral("Join a call to use its chat."))
		chatStatus_->clear();
	updateCallAction();
}

void TalkDock::applyConversations(const std::vector<Conversation> &conversations)
{
	const QSignalBlocker blocker(conversation_);
	conversation_->clear();
	for (const Conversation &item : conversations) {
		const QString label = item.callActive ? QStringLiteral("● %1").arg(item.displayName) : item.displayName;
		conversation_->addItem(label, item.token);
		conversation_->setItemData(conversation_->count() - 1, item.callActive, Qt::UserRole + 1);
		conversation_->setItemData(conversation_->count() - 1, item.canStartCall, Qt::UserRole + 2);
	}
	selectComboData(conversation_, loadedConfig_.conversationToken);
	if (!conversation_->currentData().toString().isEmpty())
		selectedConversationToken_ = conversation_->currentData().toString();
	updateCallAction();
}

void TalkDock::applyParticipants(const std::vector<Participant> &participants)
{
	registry_.sync(participants, username_->text().trimmed());
	refreshOutputChoices();
}

void TalkDock::appendChatMessages(const std::vector<ChatMessage> &messages)
{
	for (const ChatMessage &message : messages) {
		if (displayedChatMessageIds_.contains(message.id))
			continue;
		displayedChatMessageIds_.insert(message.id);
		const QString time = message.timestamp > 0
					     ? QDateTime::fromSecsSinceEpoch(message.timestamp).toLocalTime().toString(
						       QStringLiteral("HH:mm"))
					     : QString();
		const QString author = message.actorDisplayName.isEmpty() ? QStringLiteral("Talk")
									  : message.actorDisplayName;
		QString body = message.message.toHtmlEscaped();
		body.replace('\n', QStringLiteral("<br>"));
		chat_->append(QStringLiteral("<p style=\"margin:2px 0 8px 0\"><span style=\"color:gray\">%1</span> "
					     "<b>%2</b><br>%3</p>")
				      .arg(time.toHtmlEscaped(), author.toHtmlEscaped(), body));
	}
	if (!messages.empty())
		chatStatus_->clear();
}

void TalkDock::resetChat()
{
	displayedChatMessageIds_.clear();
	if (chat_)
		chat_->clear();
	if (chatInput_) {
		chatInput_->clear();
		chatInput_->setEnabled(false);
	}
	if (chatSend_)
		chatSend_->setEnabled(false);
	if (chatStatus_)
		chatStatus_->setText(QStringLiteral("Join a call to use its chat."));
}

void TalkDock::sendChatMessage()
{
	const QString message = chatInput_->text().trimmed();
	if (message.isEmpty() || client_.state() != ConnectionState::InCall)
		return;
	chatInput_->setEnabled(false);
	chatSend_->setEnabled(false);
	chatStatus_->setText(QStringLiteral("Sending…"));
	client_.sendChatMessage(message);
	chatInput_->clear();
}

void TalkDock::updateCallAction()
{
	if (client_.state() == ConnectionState::InCall) {
		joinButton_->setText(QStringLiteral("Leave call"));
		joinButton_->setEnabled(true);
		return;
	}

	const bool busy = client_.state() == ConnectionState::LoadingConversations ||
			  client_.state() == ConnectionState::Joining || client_.state() == ConnectionState::Leaving;
	const bool active = conversation_->currentData(Qt::UserRole + 1).toBool();
	const bool canStart = conversation_->currentData(Qt::UserRole + 2).toBool();
	joinButton_->setText(active ? QStringLiteral("Join call") : QStringLiteral("Start call"));
	joinButton_->setEnabled(!busy && conversation_->count() > 0 && (active || canStart));
}

void TalkDock::refreshParticipantList()
{
	participants_->clear();
	const QColor activeColor = participants_->palette().color(QPalette::Text);
	const QColor inactiveColor = participants_->palette().color(QPalette::Disabled, QPalette::Text);
	const QSize iconSize(18, 18);
	const qreal ratio = devicePixelRatioF();
	for (const Participant &participant : registry_.onlineParticipants()) {
		auto *item = new QListWidgetItem(participants_);
		item->setData(Qt::UserRole, participant.stableId);
		item->setData(Qt::UserRole + 1, participant.screenSharing);
		auto *row = new QWidget(participants_);
		row->setMinimumHeight(28);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(4, 4, 4, 4);
		layout->setSpacing(6);
		auto addIcon = [layout, row, ratio, iconSize](const QString &fileName, const QColor &color,
							   const QString &toolTip) {
			auto *icon = new QLabel(row);
			icon->setFixedSize(iconSize);
			icon->setAlignment(Qt::AlignCenter);
			icon->setPixmap(themedSvgPixmap(fileName, color, QSize(16, 16), ratio));
			icon->setToolTip(toolTip);
			icon->setAccessibleName(toolTip);
			layout->addWidget(icon);
		};
		addIcon(participant.speaking ? QStringLiteral("Talk.svg") : QStringLiteral("NoTalk.svg"),
			activeColor, participant.speaking ? QStringLiteral("Speaking") : QStringLiteral("Not speaking"));
		auto *name = new QLabel(participant.displayName, row);
		layout->addWidget(name, 1);
		addIcon(participant.audioAvailable ? QStringLiteral("mic-fill.svg")
						   : QStringLiteral("mic-off-fill.svg"),
			participant.audioAvailable ? activeColor : inactiveColor,
			participant.audioAvailable ? QStringLiteral("Unmuted") : QStringLiteral("Muted"));
		addIcon(participant.videoAvailable ? QStringLiteral("video-camera-front.svg")
						   : QStringLiteral("video-camera-front-off.svg"),
			participant.videoAvailable ? activeColor : inactiveColor,
			participant.videoAvailable ? QStringLiteral("Video on") : QStringLiteral("Video off"));
		if (participant.screenSharing)
			addIcon(QStringLiteral("screen-share.svg"), activeColor, QStringLiteral("Screen sharing"));

		const QSize rowHint = row->sizeHint();
		item->setSizeHint(QSize(rowHint.width(), std::max(28, rowHint.height())));
		participants_->setItemWidget(item, row);
	}
	if (participants_->count() == 0)
		participants_->addItem(QStringLiteral("No remote participants"));
}

void TalkDock::showParticipantContextMenu(const QPoint &position)
{
	QListWidgetItem *item = participants_->itemAt(position);
	if (!item)
		return;
	const QString participantId = item->data(Qt::UserRole).toString();
	if (participantId.isEmpty())
		return;

	QMenu menu(participants_);
	QAction *videoAndAudio = menu.addAction(QStringLiteral("Add video + audio to preview scene"));
	QAction *video = menu.addAction(QStringLiteral("Add video to preview scene"));
	QAction *audio = menu.addAction(QStringLiteral("Add audio to preview scene"));
	QAction *screen = nullptr;
	if (item->data(Qt::UserRole + 1).toBool()) {
		menu.addSeparator();
		screen = menu.addAction(QStringLiteral("Add screen share to preview scene"));
	}

	QAction *selected = menu.exec(participants_->viewport()->mapToGlobal(position));
	if (selected == videoAndAudio)
		addParticipantSourcesToPreview(participantId, true, true, false);
	else if (selected == video)
		addParticipantSourcesToPreview(participantId, true, false, false);
	else if (selected == audio)
		addParticipantSourcesToPreview(participantId, false, true, false);
	else if (selected && selected == screen)
		addParticipantSourcesToPreview(participantId, false, false, true);
}

void TalkDock::addParticipantSourcesToPreview(const QString &participantId, bool video, bool audio, bool screen)
{
	obs_source_t *sceneSource = obs_frontend_get_current_preview_scene();
	if (!sceneSource)
		sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource) {
		status_->setText(QStringLiteral("No OBS preview scene is available."));
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		status_->setText(QStringLiteral("The current OBS preview is not a scene."));
		return;
	}

	int added = 0;
	auto addSource = [&](ParticipantSourceKind kind) {
		obs_source_t *source = registry_.sourceFor(participantId, kind);
		if (!source)
			return;
		const char *name = obs_source_get_name(source);
		if (name && obs_scene_find_source(scene, name))
			return;
		if (obs_scene_add(scene, source))
			++added;
	};
	if (video)
		addSource(ParticipantSourceKind::Video);
	if (audio)
		addSource(ParticipantSourceKind::Audio);
	if (screen)
		addSource(ParticipantSourceKind::Screen);

	const QString sceneName = QString::fromUtf8(obs_source_get_name(sceneSource));
	obs_source_release(sceneSource);
	status_->setText(added > 0
				 ? QStringLiteral("Added %1 participant source(s) to preview scene “%2”.")
					   .arg(added)
					   .arg(sceneName)
				 : QStringLiteral("The selected participant source(s) are already in preview scene “%1”.")
					   .arg(sceneName));
}

void TalkDock::refreshOutputChoices()
{
	const QString previousVideo = outgoingVideo_->currentData().toString().isEmpty()
					      ? selectedVideoSource_
					      : outgoingVideo_->currentData().toString();
	const QString previousAudio = outgoingAudio_->currentData().toString().isEmpty()
					      ? selectedAudioSource_
					      : outgoingAudio_->currentData().toString();
	const QString previousScene = outgoingScene_->currentData().toString().isEmpty()
					      ? selectedScene_
					      : outgoingScene_->currentData().toString();

	OutputLists lists;
	obs_enum_sources(enumerateSource, &lists);
	lists.video.removeDuplicates();
	lists.audio.removeDuplicates();
	lists.video.sort(Qt::CaseInsensitive);
	lists.audio.sort(Qt::CaseInsensitive);
	QStringList scenes;
	obs_frontend_source_list sceneList = {};
	obs_frontend_get_scenes(&sceneList);
	for (size_t index = 0; index < sceneList.sources.num; ++index) {
		const char *name = obs_source_get_name(sceneList.sources.array[index]);
		if (name)
			scenes.push_back(QString::fromUtf8(name));
	}
	obs_frontend_source_list_free(&sceneList);
	scenes.removeDuplicates();
	scenes.sort(Qt::CaseInsensitive);

	const QSignalBlocker modeBlocker(outgoingMode_);
	const QSignalBlocker videoBlocker(outgoingVideo_);
	const QSignalBlocker audioBlocker(outgoingAudio_);
	const QSignalBlocker sceneBlocker(outgoingScene_);
	outgoingVideo_->clear();
	outgoingAudio_->clear();
	outgoingScene_->clear();
	outgoingVideo_->addItem(QStringLiteral("None"), QString());
	outgoingAudio_->addItem(QStringLiteral("None"), QString());
	outgoingScene_->addItem(QStringLiteral("None"), QString());
	for (const QString &name : lists.video)
		outgoingVideo_->addItem(name, name);
	for (const QString &name : lists.audio)
		outgoingAudio_->addItem(name, name);
	for (const QString &name : scenes)
		outgoingScene_->addItem(name, name);
	selectComboData(outgoingMode_, selectedOutgoingMode_);
	selectComboData(outgoingVideo_, previousVideo);
	selectComboData(outgoingAudio_, previousAudio);
	selectComboData(outgoingScene_, previousScene);
	updateOutgoingControls();
	updateOutgoingPreview();
}

void TalkDock::updateOutgoingControls()
{
	const bool devices = outgoingMediaModeFromId(outgoingMode_->currentData().toString()) ==
			     OutgoingMediaMode::Devices;
	const bool scene = outgoingMediaModeFromId(outgoingMode_->currentData().toString()) ==
			   OutgoingMediaMode::Scene;
	outgoingVideoLabel_->setVisible(devices);
	outgoingVideo_->setVisible(devices);
	outgoingAudioLabel_->setVisible(devices);
	outgoingAudio_->setVisible(devices);
	outgoingSceneLabel_->setVisible(scene);
	outgoingScene_->setVisible(scene);
}

void TalkDock::updateOutgoingPreview()
{
	if (!outgoingPreview_)
		return;
	obs_source_t *source = resolveOutgoingVideoSource(outgoingSelection());
	outgoingPreview_->setSource(source);
	if (source)
		obs_source_release(source);
}

void TalkDock::selectComboData(QComboBox *combo, const QString &value)
{
	const int index = combo->findData(value);
	if (index >= 0)
		combo->setCurrentIndex(index);
}

PluginConfig TalkDock::currentConfig() const
{
	PluginConfig config;
	config.serverUrl = serverUrl_->text().trimmed();
	config.username = username_->text().trimmed();
	config.conversationToken = selectedConversationToken_;
	config.outgoingMode = selectedOutgoingMode_;
	config.outgoingVideoSource = selectedVideoSource_;
	config.outgoingAudioSource = selectedAudioSource_;
	config.outgoingScene = selectedScene_;
	config.feedbackGuard = feedbackGuard_->isChecked();
	return config;
}

OutgoingMediaSelection TalkDock::outgoingSelection() const
{
	OutgoingMediaSelection selection;
	selection.mode = outgoingMediaModeFromId(outgoingMode_->currentData().toString());
	selection.videoSourceName = outgoingVideo_->currentData().toString();
	selection.audioSourceName = outgoingAudio_->currentData().toString();
	selection.sceneName = outgoingScene_->currentData().toString();
	return selection;
}

} // namespace nextcloud_talk
