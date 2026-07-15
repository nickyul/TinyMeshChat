#include "storage/database.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
using namespace tmc;
Database::Database() : name_("tmc-" + QUuid::createUuid().toString(QUuid::WithoutBraces)) {
}
Database::~Database() {
    if (db_.isValid())
        db_.close();
    db_ = {};
    QSqlDatabase::removeDatabase(name_);
}
Result<void> Database::open(const QString& path) {
    db_ = QSqlDatabase::addDatabase("QSQLITE", name_);
    db_.setDatabaseName(path);
    if (!db_.open())
        return Result<void>::failure(db_.lastError().text());
    QSqlQuery q(db_);
    if (!q.exec("PRAGMA foreign_keys=ON") || !q.exec("PRAGMA journal_mode=WAL"))
        return Result<void>::failure(q.lastError().text());
    const char* sql[] = {
        "CREATE TABLE IF NOT EXISTS schema_version(version INTEGER NOT NULL)",
        "INSERT INTO schema_version(version) SELECT 2 WHERE NOT EXISTS(SELECT 1 FROM "
        "schema_version)",
        "CREATE TABLE IF NOT EXISTS local_identity(peer_id TEXT PRIMARY KEY,display_name TEXT NOT "
        "NULL,device_id TEXT NOT NULL,public_key BLOB,private_key_encrypted BLOB,created_at TEXT "
        "NOT NULL)",
        "CREATE TABLE IF NOT EXISTS rooms(room_id TEXT PRIMARY KEY,room_name TEXT NOT "
        "NULL,created_by TEXT NOT NULL,created_at TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS peers(peer_id TEXT NOT NULL,room_id TEXT NOT "
        "NULL,display_name TEXT NOT NULL,device_id TEXT NOT NULL,public_key BLOB,added_at TEXT NOT "
        "NULL,PRIMARY KEY(room_id,peer_id),FOREIGN KEY(room_id) REFERENCES rooms(room_id))",
        "CREATE TABLE IF NOT EXISTS messages(message_id TEXT PRIMARY KEY,room_id TEXT NOT "
        "NULL,sender_id TEXT NOT NULL,logical_clock INTEGER NOT NULL,created_at TEXT NOT "
        "NULL,received_at TEXT NOT NULL,text TEXT NOT NULL,FOREIGN KEY(room_id) REFERENCES "
        "rooms(room_id))",
        "CREATE TABLE IF NOT EXISTS message_delivery(message_id TEXT NOT NULL,target_peer_id TEXT "
        "NOT NULL,state TEXT NOT NULL,last_attempt_at TEXT,acknowledged_at TEXT,attempt_count "
        "INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(message_id,target_peer_id),FOREIGN KEY(message_id) "
        "REFERENCES messages(message_id))",
        "CREATE INDEX IF NOT EXISTS idx_messages_room_clock ON "
        "messages(room_id,logical_clock,sender_id,message_id)"};
    for (auto s : sql)
        if (!q.exec(s))
            return Result<void>::failure(q.lastError().text());

    bool legacyPeers = false;
    if (!q.exec("PRAGMA table_info(peers)"))
        return Result<void>::failure(q.lastError().text());
    while (q.next())
        legacyPeers = legacyPeers || q.value(1).toString() == "slot";
    if (legacyPeers) {
        if (!q.exec("PRAGMA foreign_keys=OFF") || !db_.transaction())
            return Result<void>::failure(q.lastError().text());
        const QStringList migration{
            "ALTER TABLE peers RENAME TO peers_legacy",
            "CREATE TABLE peers(peer_id TEXT NOT NULL,room_id TEXT NOT NULL,display_name TEXT NOT "
            "NULL,device_id TEXT NOT NULL,public_key BLOB,added_at TEXT NOT NULL,PRIMARY "
            "KEY(room_id,peer_id),FOREIGN KEY(room_id) REFERENCES rooms(room_id))",
            "INSERT OR IGNORE INTO "
            "peers(peer_id,room_id,display_name,device_id,public_key,added_at) "
            "SELECT peer_id,room_id,display_name,device_id,public_key,added_at FROM peers_legacy",
            "DROP TABLE peers_legacy", "UPDATE schema_version SET version=2"};
        for (const auto& statement : migration) {
            if (!q.exec(statement)) {
                const auto error = q.lastError().text();
                db_.rollback();
                q.exec("PRAGMA foreign_keys=ON");
                return Result<void>::failure(error);
            }
        }
        if (!db_.commit())
            return Result<void>::failure(db_.lastError().text());
        if (!q.exec("PRAGMA foreign_keys=ON"))
            return Result<void>::failure(q.lastError().text());
    }
    return Result<void>::success();
}
