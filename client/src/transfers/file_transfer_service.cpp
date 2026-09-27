#include "tmc/transfers/file_transfer_service.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <algorithm>

namespace tmc {
namespace {
constexpr qsizetype HeaderSize = 29;
constexpr qsizetype ChunkSize = 16 * 1024;
constexpr qint64 MaxFileSize = qint64{8} * 1024 * 1024 * 1024 * 1024;
constexpr int MaxTransfers = 64;
enum class Kind : quint8 { Offer = 1, Accept, Chunk, Ack, Finish, Complete, Cancel };
enum class Phase { Hashing, Offered, Incoming, Sending, Receiving, Complete, Cancelled, Failed };

bool terminal(Phase phase) {
    return phase == Phase::Complete || phase == Phase::Cancelled || phase == Phase::Failed;
}

bool validFileName(const QString& name) {
    if (name.trimmed().isEmpty() || name == "." || name == ".." || name.toUtf8().size() > 255 ||
        name.contains('/') || name.contains('\\') || name.contains(':')) return false;
    for (const auto c : name) if (!c.isPrint()) return false;
    return true;
}

QByteArray frame(Kind kind, const QString& id, qint64 offset = 0, const QByteArray& payload = {}) {
    QByteArray bytes("TMCF", 4);
    bytes.append(static_cast<char>(kind));
    bytes.append(QUuid(id).toRfc4122());
    const auto position = qToBigEndian(static_cast<quint64>(offset));
    bytes.append(reinterpret_cast<const char*>(&position), sizeof(position));
    bytes.append(payload);
    return bytes;
}

QString phaseLabel(Phase phase, bool connected) {
    if (!connected && !terminal(phase) && phase != Phase::Hashing && phase != Phase::Incoming)
        return QStringLiteral("Ожидаем восстановления связи");
    switch (phase) {
    case Phase::Hashing: return QStringLiteral("Подготовка файла…");
    case Phase::Offered: return QStringLiteral("Ожидаем принятия");
    case Phase::Incoming: return QStringLiteral("Вам предлагают файл");
    case Phase::Sending: return QStringLiteral("Отправка");
    case Phase::Receiving: return QStringLiteral("Получение");
    case Phase::Complete: return QStringLiteral("Готово");
    case Phase::Cancelled: return QStringLiteral("Отменено");
    case Phase::Failed: return QStringLiteral("Ошибка");
    }
    return {};
}
} // namespace

struct FileTransferService::State {
    struct Transfer {
        QString id, peerId, peerName, name, path, error;
        bool outgoing{false};
        bool currentSession{true};
        Phase phase{Phase::Incoming};
        qint64 size{0}, position{0}, sent{0}, lastOffer{-2000};
        QByteArray digest, reply;
        std::unique_ptr<QFile> source;
        std::unique_ptr<QSaveFile> destination;
        QCryptographicHash hash{QCryptographicHash::Sha256};
    };
    Sender sender;
    QHash<QString, std::shared_ptr<Transfer>> entries;
    struct Retired { QString peerId; QByteArray reply; bool pending{false}; };
    QHash<QString, Retired> retired;
    QStringList retiredOrder;
    QStringList order;
    QSet<QString> connected;
    QTimer timer;
    QElapsedTimer clock;
    qint64 notified{0};
    bool dirty{false};
    qsizetype nextTransfer{0};

    void fail(Transfer& transfer, const QString& reason) {
        transfer.phase = Phase::Failed;
        transfer.error = reason;
        transfer.source.reset();
        transfer.destination.reset();
        transfer.reply = frame(Kind::Cancel, transfer.id);
        dirty = true;
    }
};

FileTransferService::FileTransferService(Sender sender, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>()) {
    state_->sender = std::move(sender);
    state_->clock.start();
    state_->timer.setInterval(16);
    connect(&state_->timer, &QTimer::timeout, this, &FileTransferService::pump);
    state_->timer.start();
}

FileTransferService::~FileTransferService() = default;

bool FileTransferService::contains(const QString& id) const { return state_->entries.contains(id); }

bool FileTransferService::hasActive(const QString& peerId) const {
    for (const auto& transfer : state_->entries)
        if (transfer->peerId == peerId && !terminal(transfer->phase)) return true;
    return false;
}

Result<void> FileTransferService::offer(const QString& peerId, const QString& peerName, const QString& path) {
    if (peerId.isEmpty() || state_->entries.size() >= MaxTransfers)
        return Result<void>::failure("Закройте завершённые передачи перед добавлением новых.");
    const QFileInfo info(path);
    if (!info.isFile() || info.size() < 0 || info.size() > MaxFileSize || !validFileName(info.fileName()))
        return Result<void>::failure("Невозможно отправить выбранный файл.");
    auto transfer = std::make_shared<State::Transfer>();
    transfer->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    transfer->peerId = peerId;
    transfer->peerName = peerName;
    transfer->name = info.fileName();
    transfer->path = path;
    transfer->size = info.size();
    transfer->outgoing = true;
    transfer->phase = Phase::Hashing;
    transfer->source = std::make_unique<QFile>(path);
    if (!transfer->source->open(QIODevice::ReadOnly)) return Result<void>::failure("Не удалось открыть файл для отправки.");
    state_->entries.insert(transfer->id, transfer);
    state_->order.prepend(transfer->id);
    emit changed();
    return Result<void>::success();
}

Result<void> FileTransferService::accept(const QString& id, const QString& path) {
    const auto transfer = state_->entries.value(id);
    if (!transfer || transfer->outgoing || transfer->phase != Phase::Incoming)
        return Result<void>::failure("Предложение файла больше недоступно.");
    if (path.isEmpty()) return Result<void>::failure("Выберите место сохранения файла.");
    auto file = std::make_unique<QSaveFile>(path);
    file->setDirectWriteFallback(false);
    if (!file->open(QIODevice::WriteOnly)) return Result<void>::failure("Не удалось создать файл в выбранном месте.");
    transfer->destination = std::move(file);
    transfer->path = path;
    transfer->phase = Phase::Receiving;
    transfer->reply = frame(Kind::Accept, id, transfer->position);
    emit changed();
    return Result<void>::success();
}

void FileTransferService::cancel(const QString& id) {
    const auto transfer = state_->entries.value(id);
    if (!transfer || terminal(transfer->phase)) return;
    transfer->phase = Phase::Cancelled;
    transfer->source.reset();
    transfer->destination.reset();
    transfer->reply = frame(Kind::Cancel, id);
    emit changed();
}

void FileTransferService::dismiss(const QString& id) {
    const auto transfer = state_->entries.value(id);
    if (!transfer || !terminal(transfer->phase)) return;
    state_->retired.insert(id, {transfer->peerId, frame(transfer->phase == Phase::Complete ? Kind::Complete : Kind::Cancel,
        id, transfer->phase == Phase::Complete ? transfer->size : 0), !transfer->reply.isEmpty()});
    state_->retiredOrder.append(id);
    while (state_->retiredOrder.size() > 1024) state_->retired.remove(state_->retiredOrder.takeFirst());
    state_->entries.remove(id);
    state_->order.removeAll(id);
    emit changed();
}

void FileTransferService::setPeerConnected(const QString& peerId, bool connected) {
    if (state_->connected.contains(peerId) == connected) return;
    if (connected) state_->connected.insert(peerId);
    else state_->connected.remove(peerId);
    for (const auto& transfer : state_->entries) {
        if (transfer->peerId == peerId && transfer->outgoing && transfer->phase == Phase::Sending) {
            transfer->phase = Phase::Offered;
            transfer->lastOffer = -2000;
        }
    }
    emit changed();
}

void FileTransferService::removePeer(const QString& peerId) {
    for (const auto& transfer : state_->entries) {
        if (transfer->peerId == peerId && !terminal(transfer->phase)) cancel(transfer->id);
        if (transfer->peerId == peerId) transfer->reply.clear();
    }
    state_->connected.remove(peerId);
}

void FileTransferService::clear() {
    for (const auto& transfer : state_->entries) {
        transfer->currentSession = false;
        if (!terminal(transfer->phase)) {
            state_->sender(transfer->peerId, frame(Kind::Cancel, transfer->id));
            transfer->phase = Phase::Cancelled;
            transfer->source.reset();
            transfer->destination.reset();
        }
        transfer->reply.clear();
    }
    state_->connected.clear();
    emit changed();
}

void FileTransferService::receive(const QString& peerId, const QString& peerName, const QByteArray& bytes) {
    if (bytes.size() < HeaderSize || bytes.size() > HeaderSize + ChunkSize || !bytes.startsWith("TMCF")) return;
    const auto id = QUuid::fromRfc4122(bytes.mid(5, 16)).toString(QUuid::WithoutBraces);
    if (QUuid(id).isNull()) return;
    const auto offset = qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.constData() + 21));
    if (offset > static_cast<quint64>(MaxFileSize)) return;
    const auto kind = static_cast<Kind>(static_cast<quint8>(bytes.at(4)));
    const auto payload = bytes.mid(HeaderSize);
    const auto retired = state_->retired.constFind(id);
    if (retired != state_->retired.cend()) {
        if (retired->peerId == peerId && (kind == Kind::Offer || kind == Kind::Finish))
            state_->sender(peerId, retired->reply);
        return;
    }
    auto transfer = state_->entries.value(id);
    if (transfer && transfer->peerId != peerId) return;
    if (kind == Kind::Offer) {
        if (payload.size() < 33 || payload.size() > 287) return;
        const auto nameBytes = payload.mid(32);
        const auto name = QString::fromUtf8(nameBytes);
        if (name.toUtf8() != nameBytes || !validFileName(name)) return;
        if (!transfer) {
            if (state_->entries.size() >= MaxTransfers) { state_->sender(peerId, frame(Kind::Cancel, id)); return; }
            transfer = std::make_shared<State::Transfer>();
            transfer->id = id;
            transfer->peerId = peerId;
            transfer->peerName = peerName;
            transfer->name = name;
            transfer->size = static_cast<qint64>(offset);
            transfer->digest = payload.first(32);
            state_->entries.insert(id, transfer);
            state_->order.prepend(id);
            emit changed();
            emit incomingOffered();
        } else {
            if (transfer->outgoing || transfer->name != name || transfer->size != static_cast<qint64>(offset) ||
                transfer->digest != payload.first(32)) return;
            if (transfer->phase == Phase::Receiving) transfer->reply = frame(Kind::Accept, id, transfer->position);
            else if (transfer->phase == Phase::Complete) transfer->reply = frame(Kind::Complete, id, transfer->size);
            else if (terminal(transfer->phase)) transfer->reply = frame(Kind::Cancel, id);
        }
        return;
    }
    if (!transfer) return;
    if (kind == Kind::Cancel && !terminal(transfer->phase)) {
        transfer->phase = Phase::Cancelled;
        transfer->source.reset();
        transfer->destination.reset();
        transfer->reply.clear();
        emit changed();
        return;
    }
    if (transfer->outgoing) {
        if (kind == Kind::Accept && transfer->phase == Phase::Offered && offset <= static_cast<quint64>(transfer->size)) {
            if (!transfer->source || transfer->source->size() != transfer->size || !transfer->source->seek(static_cast<qint64>(offset))) {
                state_->fail(*transfer, "Исходный файл изменён или недоступен.");
                return;
            }
            transfer->position = transfer->sent = static_cast<qint64>(offset);
            transfer->phase = Phase::Sending;
            emit changed();
        } else if (kind == Kind::Ack && transfer->phase == Phase::Sending && offset <= static_cast<quint64>(transfer->sent)) {
            transfer->position = std::max(transfer->position, static_cast<qint64>(offset));
            state_->dirty = true;
        } else if (kind == Kind::Complete && (transfer->phase == Phase::Sending || transfer->phase == Phase::Offered) &&
                   offset == static_cast<quint64>(transfer->size)) {
            transfer->position = transfer->size;
            transfer->phase = Phase::Complete;
            transfer->source.reset();
            emit changed();
        }
        return;
    }
    if (kind == Kind::Finish && transfer->phase == Phase::Complete) {
        transfer->reply = frame(Kind::Complete, id, transfer->size);
        return;
    }
    if (transfer->phase != Phase::Receiving) return;
    if (kind == Kind::Chunk) {
        if (payload.isEmpty() || offset != static_cast<quint64>(transfer->position) ||
            payload.size() > transfer->size - transfer->position) {
            state_->fail(*transfer, "Нарушен порядок передачи файла.");
            return;
        }
        if (transfer->destination->write(payload) != payload.size()) {
            state_->fail(*transfer, "Не удалось записать файл. Проверьте свободное место.");
            return;
        }
        transfer->hash.addData(payload);
        transfer->position += payload.size();
        transfer->reply = frame(Kind::Ack, id, transfer->position);
        state_->dirty = true;
    } else if (kind == Kind::Finish) {
        if (transfer->position != transfer->size || transfer->hash.result() != transfer->digest) {
            state_->fail(*transfer, "Проверка целостности файла не пройдена.");
            return;
        }
        if (!transfer->destination->commit()) {
            state_->fail(*transfer, "Не удалось сохранить полученный файл.");
            return;
        }
        transfer->destination.reset();
        transfer->phase = Phase::Complete;
        transfer->reply = frame(Kind::Complete, id, transfer->size);
        emit changed();
    }
}

void FileTransferService::pump() {
    const auto now = state_->clock.elapsed();
    for (auto& retired : state_->retired) {
        if (retired.pending && state_->connected.contains(retired.peerId) &&
            state_->sender(retired.peerId, retired.reply)) retired.pending = false;
    }
    // Global work bounds keep hashing and transfer traffic from monopolizing the GUI thread.
    int hashBudget = 256 * 1024;
    int chunkBudget = 8;
    // Rotate the starting transfer so one large file cannot starve the rest.
    const auto order = state_->order;
    const auto start = order.isEmpty() ? 0 : state_->nextTransfer++ % order.size();
    for (qsizetype index = 0; index < order.size(); ++index) {
        const auto id = order.at((start + index) % order.size());
        const auto transfer = state_->entries.value(id);
        if (!transfer) continue;
        if (transfer->phase == Phase::Hashing && hashBudget > 0) {
            const auto bytes = transfer->source->read(hashBudget);
            hashBudget -= static_cast<int>(bytes.size());
            if (transfer->source->error() != QFileDevice::NoError || transfer->source->size() != transfer->size) {
                state_->fail(*transfer, "Не удалось прочитать исходный файл.");
                continue;
            }
            transfer->hash.addData(bytes);
            if (transfer->source->atEnd()) {
                transfer->digest = transfer->hash.result();
                transfer->source->seek(0);
                transfer->phase = Phase::Offered;
                state_->dirty = true;
            }
        }
        if (!state_->connected.contains(transfer->peerId)) continue;
        if (!transfer->reply.isEmpty() && state_->sender(transfer->peerId, transfer->reply)) transfer->reply.clear();
        if (transfer->phase == Phase::Offered && now - transfer->lastOffer >= 2000) {
            if (state_->sender(transfer->peerId, frame(Kind::Offer, id, transfer->size, transfer->digest + transfer->name.toUtf8())))
                transfer->lastOffer = now;
        }
        if (transfer->phase != Phase::Sending) continue;
        while (chunkBudget > 0 && transfer->sent < transfer->size && transfer->sent - transfer->position < 256 * 1024) {
            const auto bytes = transfer->source->read(std::min<qint64>(ChunkSize, transfer->size - transfer->sent));
            if (bytes.isEmpty()) { state_->fail(*transfer, "Исходный файл изменён или недоступен."); break; }
            if (!state_->sender(transfer->peerId, frame(Kind::Chunk, id, transfer->sent, bytes))) {
                transfer->source->seek(transfer->sent);
                break;
            }
            transfer->sent += bytes.size();
            --chunkBudget;
        }
        if (transfer->phase == Phase::Sending && transfer->position == transfer->size && now - transfer->lastOffer >= 1000) {
            if (state_->sender(transfer->peerId, frame(Kind::Finish, id))) transfer->lastOffer = now;
        }
    }
    if (state_->dirty && now - state_->notified >= 200) {
        state_->dirty = false;
        state_->notified = now;
        emit changed();
    }
}

QVariantList FileTransferService::transfers() const {
    QVariantList rows;
    for (const auto& id : state_->order) {
        const auto& t = *state_->entries.value(id);
        rows.append(QVariantMap{{"id", id}, {"peerId", t.peerId}, {"peerName", t.peerName}, {"name", t.name},
            {"currentSession", t.currentSession}, {"outgoing", t.outgoing}, {"size", t.size}, {"transferred", t.position},
            {"progress", t.size == 0 ? (t.phase == Phase::Complete ? 1.0 : 0.0) : double(t.position) / double(t.size)},
            {"status", t.error.isEmpty() ? phaseLabel(t.phase, state_->connected.contains(t.peerId)) : t.error},
            {"canAccept", t.phase == Phase::Incoming}, {"finished", terminal(t.phase)}});
    }
    return rows;
}
} // namespace tmc
