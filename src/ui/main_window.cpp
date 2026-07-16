#include "ui/main_window.h"

#include "app/application_controller.h"
#include "app/network_session.h"
#include "messaging/chat_message.h"
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

using namespace tmc;

MainWindow::MainWindow(ApplicationController& controller, QWidget* parent)
    : QMainWindow(parent), controller_(controller),
      session_(std::make_unique<NetworkSession>(controller)) {
    setWindowTitle("TinyMesh Chat");
    resize(1040, 680);
    setMinimumSize(780, 520);
    setStyleSheet(R"(
        QMainWindow, QWidget { background: #f3f5f7; color: #303b45; font-size: 14px; }
        QMenuBar { background: #f8f9fa; padding: 4px; border-bottom: 1px solid #e1e5e9; }
        QMenuBar::item:selected, QMenu::item:selected { background: #e6ebef; border-radius: 5px; }
        QMenu { background: #ffffff; border: 1px solid #dce1e5; padding: 6px; }
        QListWidget { background: #e9edf0; border: 1px solid #d9dfe4; border-radius: 10px;
                      padding: 8px; outline: none; }
        QListWidget::item { border: 1px solid #e1e5e8; border-radius: 8px; margin: 3px;
                            padding: 9px; color: #303b45; }
        QListWidget::item:selected { background: #dce8ef; color: #25323b; }
        QLineEdit, QPlainTextEdit { background: #ffffff; border: 1px solid #d3d9de;
                                   border-radius: 8px; padding: 9px; selection-background-color: #9eb9ca; }
        QLineEdit:focus, QPlainTextEdit:focus { border: 1px solid #7898ad; }
        QPushButton { background: #68879b; color: white; border: none; border-radius: 8px;
                      padding: 9px 16px; font-weight: 600; }
        QPushButton:hover { background: #7897aa; }
        QPushButton:pressed { background: #58778b; }
        QStatusBar { background: #f8f9fa; color: #6f7b84; border-top: 1px solid #e1e5e9; }
        QLabel#meshBadge { background: #e4ecef; color: #506e7f; border-radius: 9px;
                           padding: 6px 11px; font-weight: 600; }
        QLabel#peerPanel { background: #ffffff; border: 1px solid #dce1e5; border-radius: 10px;
                           padding: 13px; }
    )");

    auto* central = new QWidget;
    auto* outer = new QVBoxLayout(central);
    auto* header = new QHBoxLayout;
    auto* title =
        new QLabel("<span style='font-size:20px;font-weight:700'>TinyMesh Chat</span>"
                   "<br><span style='color:#7f91a6'>децентрализованная P2P-комната</span>");
    header->addWidget(title);
    header->addStretch();
    auto* quickImport = new QPushButton("Вставить код");
    auto* quickInvite = new QPushButton("Пригласить");
    header->addWidget(quickImport);
    header->addWidget(quickInvite);
    mesh_ = new QLabel("Прямые связи: 0/0");
    mesh_->setObjectName("meshBadge");
    header->addWidget(mesh_);
    outer->addLayout(header);

    auto* body = new QHBoxLayout;
    messages_ = new QListWidget;
    messages_->setWordWrap(true);
    body->addWidget(messages_, 3);
    peers_ = new QLabel;
    peers_->setObjectName("peerPanel");
    peers_->setAlignment(Qt::AlignTop);
    peers_->setMinimumWidth(240);
    body->addWidget(peers_, 1);
    outer->addLayout(body);

    auto* composer = new QHBoxLayout;
    input_ = new QLineEdit;
    input_->setPlaceholderText("Сообщение участникам комнаты…");
    auto* send = new QPushButton("Отправить");
    send->setDefault(true);
    composer->addWidget(input_);
    composer->addWidget(send);
    outer->addLayout(composer);
    setCentralWidget(central);

    auto* roomMenu = menuBar()->addMenu("Комната");
    roomMenu->addAction("Создать комнату", this, &MainWindow::createRoom);
    roomMenu->addAction("Новое приглашение / переподключение", this, &MainWindow::createInvitation);
    roomMenu->addSeparator();
    roomMenu->addAction("Вставить signaling-текст", this, &MainWindow::importSignalingText);
    roomMenu->addAction("Импортировать signaling-файл", this, &MainWindow::importSignalingFile);

    auto* toolsMenu = menuBar()->addMenu("Инструменты");
    toolsMenu->addAction("Настройки STUN", this, &MainWindow::showStunSettings);
    toolsMenu->addAction("Диагностика сети", this, &MainWindow::showNetworkDiagnostics);
    menuBar()->addAction("О программе", this, &MainWindow::showAbout);

    connect(send, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(quickImport, &QPushButton::clicked, this, &MainWindow::importSignalingText);
    connect(quickInvite, &QPushButton::clicked, this, &MainWindow::createInvitation);
    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::sendMessage);
    connect(session_.get(), &NetworkSession::statusChanged, this,
            [this](const QString& status) { statusBar()->showMessage(status); });
    connect(session_.get(), &NetworkSession::errorOccurred, this, &MainWindow::showError);
    connect(session_.get(), &NetworkSession::meshChanged, this, [this](int connected, int total) {
        mesh_->setText(QString("Прямые связи: %1/%2").arg(connected).arg(total));
    });
    connect(session_.get(), &NetworkSession::peerChanged, this, &MainWindow::updatePeer);
    connect(session_.get(), &NetworkSession::signalingReady, this, &MainWindow::showSignaling);
    connect(session_.get(), &NetworkSession::messageReceived, this, &MainWindow::appendMessage);
    connect(session_.get(), &NetworkSession::deliveryChanged, this,
            [this](const QString& messageId, int acknowledged, int expected) {
                auto* item = messageItems_.value(messageId);
                if (!item)
                    return;
                updateDeliveryIndicator(item, acknowledged, expected);
            });
    connect(session_.get(), &NetworkSession::roomChanged, this,
            [this](const QString&, const QString& name) {
                setWindowTitle("TinyMesh Chat — " + name);
                messages_->clear();
                messageItems_.clear();
                peerStates_.clear();
                rebuildPeerLabel();
            });

    rebuildPeerLabel();
    statusBar()->showMessage("Создайте комнату или импортируйте приглашение.");
}

MainWindow::~MainWindow() = default;

void MainWindow::createRoom() {
    bool accepted = false;
    const auto name = QInputDialog::getText(
        this, "Новая комната", "Название комнаты:", QLineEdit::Normal, "Чат друзей", &accepted);
    if (!accepted)
        return;
    const auto result = session_->createRoom(name);
    if (!result)
        showError(result.error());
}

void MainWindow::createInvitation() {
    const auto result = session_->createInvitation();
    if (!result)
        showError(result.error());
}

void MainWindow::importSignalingText() {
    bool accepted = false;
    const auto text = QInputDialog::getMultiLineText(this, "Импорт signaling",
                                                     "Вставьте строку tmc1:", {}, &accepted);
    if (!accepted)
        return;
    const auto result = session_->importSignalingText(text);
    if (!result)
        showError(result.error());
}

void MainWindow::importSignalingFile() {
    const auto path = QFileDialog::getOpenFileName(
        this, "Импорт signaling", {},
        "TinyMesh signaling (*.tmcinvite *.tmcanswer);;JSON (*.json);;Все файлы (*)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showError("Не удалось открыть файл: " + file.errorString());
        return;
    }
    const auto result = session_->importSignalingDocument(file.readAll());
    if (!result)
        showError(result.error());
}

void MainWindow::showSignaling(const QString& kind, const QString& text, const QByteArray& document,
                               const QString& suggestedName) {
    QApplication::clipboard()->setText(text);

    QDialog dialog(this);
    dialog.setWindowTitle(kind == "offer" ? "Приглашение готово" : "Answer готов");
    dialog.resize(720, 400);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Строка уже скопирована в буфер обмена. Передайте её другу:"));
    auto* editor = new QPlainTextEdit(text);
    editor->setReadOnly(true);
    layout->addWidget(editor);
    auto* buttons = new QDialogButtonBox;
    auto* copy = buttons->addButton("Копировать", QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton("Сохранить файл…", QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(copy, &QPushButton::clicked, this,
            [text] { QApplication::clipboard()->setText(text); });
    connect(save, &QPushButton::clicked, &dialog, [this, document, suggestedName] {
        const auto path = QFileDialog::getSaveFileName(this, "Сохранить signaling", suggestedName);
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(document) != document.size())
            showError("Не удалось сохранить signaling-файл: " + file.errorString());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::sendMessage() {
    const auto text = input_->text();
    const auto result = session_->sendMessage(text);
    if (!result) {
        showError(result.error());
        return;
    }
    input_->clear();
}

void MainWindow::appendMessage(const ChatMessage& message, bool local) {
    const auto author =
        local ? controller_.identity().displayName
              : peerStates_
                    .value(message.senderId, {session_->peerDisplayName(message.senderId), false})
                    .first;
    const auto base =
        message.createdAt.toLocalTime().toString("[HH:mm] ") + author + "\n" + message.text;
    auto* item = new QListWidgetItem(base);
    item->setTextAlignment(Qt::AlignLeft);
    item->setBackground(local ? QColor("#edf4f7") : QColor("#ffffff"));
    item->setData(Qt::UserRole, message.messageId);
    item->setData(Qt::UserRole + 1, base);
    item->setData(Qt::UserRole + 2, message.logicalClock);
    item->setData(Qt::UserRole + 3, message.senderId);
    item->setData(Qt::UserRole + 4, message.messageId);
    int row = 0;
    for (; row < messages_->count(); ++row) {
        const auto* current = messages_->item(row);
        const auto currentClock = current->data(Qt::UserRole + 2).toLongLong();
        const auto currentSender = current->data(Qt::UserRole + 3).toString();
        const auto currentId = current->data(Qt::UserRole + 4).toString();
        if (message.logicalClock < currentClock ||
            (message.logicalClock == currentClock && message.senderId < currentSender) ||
            (message.logicalClock == currentClock && message.senderId == currentSender &&
             message.messageId < currentId))
            break;
    }
    messages_->insertItem(row, item);
    messageItems_.insert(message.messageId, item);
    if (local) {
        const auto counts = session_->deliveryCounts(message.messageId);
        if (counts) {
            const auto acknowledged = counts.value().first;
            const auto expected = counts.value().second;
            updateDeliveryIndicator(item, acknowledged, expected);
        }
    }
    constexpr int MaxVisibleMessages = 2000;
    if (messages_->count() > MaxVisibleMessages) {
        auto* oldest = messages_->takeItem(0);
        messageItems_.remove(oldest->data(Qt::UserRole).toString());
        delete oldest;
    }
    messages_->scrollToBottom();
}

void MainWindow::updatePeer(const QString& peerId, const QString& displayName, bool connected) {
    if (peerId.isEmpty())
        return;
    peerStates_[peerId] = {displayName.isEmpty() ? peerId.left(8) : displayName, connected};
    rebuildPeerLabel();
}

void MainWindow::updateDeliveryIndicator(QListWidgetItem* item, int acknowledged, int expected) {
    if (!item)
        return;
    const auto base = item->data(Qt::UserRole + 1).toString();
    QString marker;
    if (expected > 0 && acknowledged > 0)
        marker = acknowledged >= expected ? "  ✓✓" : "  ✓";
    item->setText(base + marker);
    item->setToolTip(expected > 0 ? QString("Доставлено: %1 из %2").arg(acknowledged).arg(expected)
                                  : QString("Сохранено локально"));
}

void MainWindow::rebuildPeerLabel() {
    QString html = "<span style='font-size:16px;font-weight:700'>Участники</span><br><br>"
                   "<span style='color:#729985'>●</span> " +
                   controller_.identity().displayName +
                   " <span style='color:#8292a6'>(вы)</span><br>";
    for (auto it = peerStates_.cbegin(); it != peerStates_.cend(); ++it)
        html +=
            QString("<span style='color:%1'>●</span> %2<br>")
                .arg(it.value().second ? "#729985" : "#a0a8ae", it.value().first.toHtmlEscaped());
    html += QString("<br><span style='color:#8292a6'>%1 из %2 мест занято</span>")
                .arg(peerStates_.size() + 1)
                .arg(controller_.config().maxRoomPeers);
    html += "<br><br><span style='color:#8292a6'>STUN включён<br>Relay не используется</span>";
    peers_->setText(html);
}

void MainWindow::showStunSettings() {
    bool accepted = false;
    const auto value = QInputDialog::getMultiLineText(
        this, "Настройки STUN",
        "Один stun: URI на строку:", controller_.config().stunServers.join('\n'), &accepted);
    if (!accepted)
        return;
    const auto result = controller_.updateStunServers(value.split('\n', Qt::SkipEmptyParts));
    if (!result) {
        showError(result.error());
        return;
    }
    rebuildPeerLabel();
    statusBar()->showMessage("Настройки STUN сохранены и будут применены к новым соединениям.",
                             10000);
}

void MainWindow::showNetworkDiagnostics() {
    QMessageBox::information(this, "Диагностика сети",
                             session_->diagnostics() +
                                 "\n\nЛокальный Peer ID: " + controller_.identity().peerId +
                                 "\nSTUN:\n  " + controller_.config().stunServers.join("\n  ") +
                                 "\n\nКаталог данных:\n" + controller_.dataDirectory());
}

void MainWindow::showAbout() {
    QMessageBox::about(
        this, "TinyMesh Chat",
        "Динамический прямой P2P-чат для небольшой компании.\n"
        "Первый offer/answer передаётся вручную, остальные связи строятся автоматически.\n"
        "TURN и relay не используются.\n"
        "Прямое соединение не гарантируется для строгих NAT.");
}

void MainWindow::showError(const QString& error) {
    statusBar()->showMessage(error, 10000);
    QMessageBox::warning(this, "TinyMesh Chat", error);
}
