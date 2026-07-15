#include "storage/message_repository.h"
#include <QSqlError>
#include <QSqlQuery>
using namespace tmc;

static ChatMessage messageFromQuery(const QSqlQuery& q) {
    return {q.value(0).toString(),
            q.value(1).toString(),
            q.value(2).toString(),
            q.value(6).toString(),
            q.value(3).toLongLong(),
            QDateTime::fromString(q.value(4).toString(), Qt::ISODateWithMs),
            QDateTime::fromString(q.value(5).toString(), Qt::ISODateWithMs)};
}

Result<bool> MessageRepository::insert(const ChatMessage& m) {
    if (!m.isValid())
        return Result<bool>::failure("Invalid message");
    QSqlQuery q(db_);
    q.prepare("INSERT OR IGNORE INTO "
              "messages(message_id,room_id,sender_id,logical_clock,created_at,received_at,text) "
              "VALUES(?,?,?,?,?,?,?)");
    q.addBindValue(m.messageId);
    q.addBindValue(m.roomId);
    q.addBindValue(m.senderId);
    q.addBindValue(m.logicalClock);
    q.addBindValue(m.createdAt.toUTC().toString(Qt::ISODateWithMs));
    q.addBindValue(m.receivedAt.toUTC().toString(Qt::ISODateWithMs));
    q.addBindValue(m.text);
    if (!q.exec())
        return Result<bool>::failure(q.lastError().text());
    return Result<bool>::success(q.numRowsAffected() == 1);
}
Result<QList<ChatMessage>> MessageRepository::history(const QString& r, int limit) const {
    return recent(r, limit);
}

Result<QList<ChatMessage>> MessageRepository::recent(const QString& roomId, int limit) const {
    QSqlQuery q(db_);
    q.prepare("SELECT message_id,room_id,sender_id,logical_clock,created_at,received_at,text FROM "
              "(SELECT message_id,room_id,sender_id,logical_clock,created_at,received_at,text "
              "FROM messages WHERE room_id=? ORDER BY logical_clock DESC,sender_id "
              "DESC,message_id DESC LIMIT ?) ORDER BY logical_clock,sender_id,message_id");
    q.addBindValue(roomId);
    q.addBindValue(qBound(1, limit, 500));
    if (!q.exec())
        return Result<QList<ChatMessage>>::failure(q.lastError().text());
    QList<ChatMessage> out;
    while (q.next())
        out.push_back(messageFromQuery(q));
    return Result<QList<ChatMessage>>::success(out);
}

Result<QList<ChatMessage>> MessageRepository::byIds(const QString& roomId,
                                                    const QStringList& ids) const {
    if (ids.isEmpty())
        return Result<QList<ChatMessage>>::success({});
    if (ids.size() > 200)
        return Result<QList<ChatMessage>>::failure("Too many message IDs requested");
    QStringList placeholders;
    placeholders.fill("?", ids.size());
    QSqlQuery q(db_);
    q.prepare("SELECT message_id,room_id,sender_id,logical_clock,created_at,received_at,text "
              "FROM messages WHERE room_id=? AND message_id IN (" +
              placeholders.join(',') + ") ORDER BY logical_clock,sender_id,message_id");
    q.addBindValue(roomId);
    for (const auto& id : ids)
        q.addBindValue(id);
    if (!q.exec())
        return Result<QList<ChatMessage>>::failure(q.lastError().text());
    QList<ChatMessage> out;
    while (q.next())
        out.push_back(messageFromQuery(q));
    return Result<QList<ChatMessage>>::success(out);
}

Result<qint64> MessageRepository::count(const QString& roomId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT COUNT(*) FROM messages WHERE room_id=?");
    q.addBindValue(roomId);
    if (!q.exec() || !q.next())
        return Result<qint64>::failure(q.lastError().text());
    return Result<qint64>::success(q.value(0).toLongLong());
}

Result<QList<ChatMessage>>
MessageRepository::pendingForPeer(const QString& roomId, const QString& peerId, int limit) const {
    QSqlQuery q(db_);
    q.prepare("SELECT m.message_id,m.room_id,m.sender_id,m.logical_clock,m.created_at,"
              "m.received_at,m.text FROM messages m JOIN message_delivery d ON "
              "d.message_id=m.message_id WHERE m.room_id=? AND d.target_peer_id=? AND "
              "d.state='pending' ORDER BY m.logical_clock,m.sender_id,m.message_id LIMIT ?");
    q.addBindValue(roomId);
    q.addBindValue(peerId);
    q.addBindValue(qBound(1, limit, 5000));
    if (!q.exec())
        return Result<QList<ChatMessage>>::failure(q.lastError().text());
    QList<ChatMessage> out;
    while (q.next())
        out.push_back(messageFromQuery(q));
    return Result<QList<ChatMessage>>::success(out);
}

Result<QPair<int, int>> MessageRepository::deliveryCounts(const QString& messageId) const {
    QSqlQuery q(db_);
    q.prepare("SELECT COALESCE(SUM(CASE WHEN state='delivered' THEN 1 ELSE 0 END),0),COUNT(*) "
              "FROM message_delivery WHERE message_id=?");
    q.addBindValue(messageId);
    if (!q.exec() || !q.next())
        return Result<QPair<int, int>>::failure(q.lastError().text());
    return Result<QPair<int, int>>::success({q.value(0).toInt(), q.value(1).toInt()});
}

Result<void> MessageRepository::setDelivery(const QString& m, const QString& p, const QString& s) {
    static const QSet<QString> valid{"pending", "delivered", "failed"};
    if (!valid.contains(s))
        return Result<void>::failure("Invalid delivery state");
    QSqlQuery q(db_);
    q.prepare(
        "INSERT INTO "
        "message_delivery(message_id,target_peer_id,state,last_attempt_at,acknowledged_at,attempt_"
        "count) VALUES(?,?,?,?,?,1) ON CONFLICT(message_id,target_peer_id) DO UPDATE SET "
        "state=excluded.state,last_attempt_at=excluded.last_attempt_at,acknowledged_at=excluded."
        "acknowledged_at,attempt_count=attempt_count+CASE WHEN excluded.state='pending' THEN 1 "
        "ELSE "
        "0 END");
    auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    q.addBindValue(m);
    q.addBindValue(p);
    q.addBindValue(s);
    q.addBindValue(now);
    q.addBindValue(s == "delivered" ? QVariant(now) : QVariant());
    if (!q.exec())
        return Result<void>::failure(q.lastError().text());
    return Result<void>::success();
}
