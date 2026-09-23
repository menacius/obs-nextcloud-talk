#pragma once

#include "config.hpp"
#include "outgoing-media.hpp"
#include "source-registry.hpp"
#include "talk-client.hpp"
#include "talk-media-transport.hpp"

#include <QSet>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDialog;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;
class QTimer;

class QPoint;

namespace nextcloud_talk {

class OutgoingPreview;

class TalkDock final : public QWidget {
public:
	explicit TalkDock(QWidget *parent = nullptr);
	~TalkDock() override;

	void save();
	void shutdown();
	void showSettings();
	void refreshTheme();

protected:
	void changeEvent(QEvent *event) override;

private:
	void buildUi();
	void bindEvents();
	void loadConfig();
	void loadStoredCredential();
	bool storeCredentialForCurrentAccount();
	void forgetStoredCredential();
	void connectToServer();
	void joinOrLeave();
	void applyState(ConnectionState state, const QString &detail);
	void applyConversations(const std::vector<Conversation> &conversations);
	void applyParticipants(const std::vector<Participant> &participants);
	void appendChatMessages(const std::vector<ChatMessage> &messages);
	void resetChat();
	void sendChatMessage();
	void updateCallAction();
	void refreshParticipantList();
	void showParticipantContextMenu(const QPoint &position);
	void addParticipantSourcesToPreview(const QString &participantId, bool video, bool audio, bool screen);
	void refreshOutputChoices();
	void updateOutgoingControls();
	void updateOutgoingPreview();
	void selectComboData(QComboBox *combo, const QString &value);
	OutgoingMediaSelection outgoingSelection() const;
	PluginConfig currentConfig() const;

	TalkClient client_;
	TalkMediaTransport mediaTransport_;
	SourceRegistry registry_;
	PluginConfig loadedConfig_;
	QString selectedConversationToken_;
	QString selectedOutgoingMode_ = QStringLiteral("devices");
	QString selectedVideoSource_;
	QString selectedAudioSource_;
	QString selectedScene_;

	QLineEdit *serverUrl_ = nullptr;
	QLineEdit *username_ = nullptr;
	QLineEdit *appPassword_ = nullptr;
	QPushButton *forgetPassword_ = nullptr;
	QPushButton *connectButton_ = nullptr;
	QComboBox *conversation_ = nullptr;
	QPushButton *joinButton_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *titleIcon_ = nullptr;
	QLabel *settingsIcon_ = nullptr;
	QListWidget *participants_ = nullptr;
	QTextBrowser *chat_ = nullptr;
	QLineEdit *chatInput_ = nullptr;
	QPushButton *chatSend_ = nullptr;
	QLabel *chatStatus_ = nullptr;
	QComboBox *outgoingMode_ = nullptr;
	QComboBox *outgoingVideo_ = nullptr;
	QComboBox *outgoingAudio_ = nullptr;
	QComboBox *outgoingScene_ = nullptr;
	QLabel *outgoingVideoLabel_ = nullptr;
	QLabel *outgoingAudioLabel_ = nullptr;
	QLabel *outgoingSceneLabel_ = nullptr;
	OutgoingPreview *outgoingPreview_ = nullptr;
	QTimer *outgoingPreviewTimer_ = nullptr;
	QPushButton *refreshOutputs_ = nullptr;
	QCheckBox *feedbackGuard_ = nullptr;
	QDialog *settingsDialog_ = nullptr;
	QSet<qint64> displayedChatMessageIds_;
	bool storedCredentialAvailable_ = false;
	bool shutdown_ = false;
};

} // namespace nextcloud_talk
