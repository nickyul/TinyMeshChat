#pragma once

#include <QHash>
#include <QMainWindow>
#include <memory>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QLineEdit;

namespace tmc {
class ApplicationController;
class NetworkSession;

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(ApplicationController& controller, QWidget* parent = nullptr);
    ~MainWindow() override;

  private slots:
    void createRoom();
    void createInvitation();
    void importSignalingText();
    void importSignalingFile();
    void sendMessage();
    void showAbout();
    void showStunSettings();
    void showNetworkDiagnostics();
    void openLogs();

  private:
    void showSignaling(const QString& kind, const QString& text, const QByteArray& document,
                       const QString& suggestedName);
    void appendMessage(const class ChatMessage& message, bool local);
    void updatePeer(const QString& peerId, const QString& displayName, bool connected);
    void updateDeliveryIndicator(QListWidgetItem* item, int acknowledged, int expected);
    void rebuildPeerLabel();
    void showError(const QString& error);

    ApplicationController& controller_;
    std::unique_ptr<NetworkSession> session_;
    QLabel* mesh_{};
    QLabel* peers_{};
    QListWidget* messages_{};
    QLineEdit* input_{};
    QHash<QString, QPair<QString, bool>> peerStates_;
    QHash<QString, QListWidgetItem*> messageItems_;
};
} // namespace tmc
