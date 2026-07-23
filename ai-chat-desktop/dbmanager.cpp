#include "dbmanager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QUuid>
#include <QDateTime>

DbManager::DbManager(QObject *parent) : QObject(parent) {}

bool DbManager::open(const QString &odbcDriver, const QString &host, int port,
                     const QString &database, const QString &user, const QString &password) {
    const QString connName = "ai_chat_conn";

    // 先关闭并移除旧连接（确保重连时使用全新参数）
    if (QSqlDatabase::contains(connName)) {
        {   // 作用域：确保 QSqlDatabase 析构后再 remove
            QSqlDatabase oldDb = QSqlDatabase::database(connName);
            if (oldDb.isOpen()) oldDb.close();
        }
        QSqlDatabase::removeDatabase(connName);
    }

    m_db = QSqlDatabase::addDatabase("QODBC", connName);

    // 用连接字符串方式，无需在系统里预先配置 DSN
    QString connStr = QStringLiteral(
        "DRIVER={%1};SERVER=%2;PORT=%3;DATABASE=%4;UID=%5;PWD=%6;CHARSET=utf8mb4;")
        .arg(odbcDriver, host)
        .arg(port)
        .arg(database, user, password);
    m_db.setDatabaseName(connStr);

    if (!m_db.open()) {
        m_err = m_db.lastError().text();
        return false;
    }
    return true;
}

bool DbManager::isOpen() const { return m_db.isOpen(); }
QString DbManager::lastError() const { return m_err; }

void DbManager::saveMessage(const QString &conversationId, const QString &role,
                            const QString &content, const QString &model) {
    if (!m_db.isOpen()) return;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO chat_messages (conversation_id, role, content, model) "
              "VALUES (?, ?, ?, ?)");
    q.addBindValue(conversationId);
    q.addBindValue(role);
    q.addBindValue(content);
    q.addBindValue(model);
    if (!q.exec())
        m_err = q.lastError().text();
}

QList<ChatMessage> DbManager::loadHistory(const QString &conversationId) {
    QList<ChatMessage> list;
    if (!m_db.isOpen()) return list;
    QSqlQuery q(m_db);
    q.prepare("SELECT role, content FROM chat_messages "
              "WHERE conversation_id = ? ORDER BY id ASC");
    q.addBindValue(conversationId);
    if (q.exec()) {
        while (q.next()) {
            ChatMessage m;
            m.role = q.value(0).toString();
            m.content = q.value(1).toString();
            list.append(m);
        }
    } else {
        m_err = q.lastError().text();
    }
    return list;
}

// ---------------- 笔记 ----------------
bool DbManager::ensureNotesTable() {
    if (!m_db.isOpen()) return false;
    QSqlQuery q(m_db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS notes ("
                "id VARCHAR(40) PRIMARY KEY, "
                "title VARCHAR(255) NOT NULL DEFAULT '未命名笔记', "
                "content LONGTEXT, "
                "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)")) {
        m_err = q.lastError().text();
        return false;
    }
    return true;
}

bool DbManager::ensureChatMessagesTable() {
    if (!m_db.isOpen()) return false;
    QSqlQuery q(m_db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS chat_messages ("
                "id INT AUTO_INCREMENT PRIMARY KEY, "
                "conversation_id VARCHAR(40) NOT NULL, "
                "role VARCHAR(20) NOT NULL, "
                "content LONGTEXT, "
                "model VARCHAR(100), "
                "created_at DATETIME DEFAULT CURRENT_TIMESTAMP, "
                "INDEX idx_conv (conversation_id))")) {
        m_err = q.lastError().text();
        return false;
    }
    return true;
}

bool DbManager::saveNote(Note &note) {
    if (!m_db.isOpen()) return false;
    if (note.id.isEmpty()) {
        note.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    const bool transactionStarted = m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO notes (id, title, content, updated_at) "
              "VALUES (?, ?, ?, NOW()) "
              "ON DUPLICATE KEY UPDATE "
              "title=VALUES(title), content=VALUES(content), updated_at=NOW()");
    q.addBindValue(note.id);
    q.addBindValue(note.title);
    q.addBindValue(note.content);
    if (!q.exec()) {
        m_err = q.lastError().text();
        if (transactionStarted) m_db.rollback();
        return false;
    }
    if (transactionStarted && !m_db.commit()) {
        m_err = m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    // 取回更新时间
    QSqlQuery q2(m_db);
    q2.prepare("SELECT updated_at FROM notes WHERE id=?");
    q2.addBindValue(note.id);
    if (q2.exec() && q2.next())
        note.updatedAt = q2.value(0).toDateTime().toString("yyyy-MM-dd hh:mm");
    return true;
}

Note DbManager::loadNote(const QString &id) {
    Note n;
    if (!m_db.isOpen()) return n;
    QSqlQuery q(m_db);
    q.prepare("SELECT id, title, content, updated_at FROM notes WHERE id=?");
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        n.id = q.value(0).toString();
        n.title = q.value(1).toString();
        n.content = q.value(2).toString();
        n.updatedAt = q.value(3).toDateTime().toString("yyyy-MM-dd hh:mm");
    }
    return n;
}

QList<Note> DbManager::loadAllNotes() {
    QList<Note> list;
    if (!m_db.isOpen()) return list;
    QSqlQuery q(m_db);
    q.prepare("SELECT id, title, content, updated_at FROM notes "
              "ORDER BY updated_at DESC");
    if (q.exec()) {
        while (q.next()) {
            Note n;
            n.id = q.value(0).toString();
            n.title = q.value(1).toString();
            n.content = q.value(2).toString();
            n.updatedAt = q.value(3).toDateTime().toString("yyyy-MM-dd hh:mm");
            list.append(n);
        }
    } else {
        m_err = q.lastError().text();
    }
    return list;
}

bool DbManager::deleteNote(const QString &id) {
    if (!m_db.isOpen()) return false;
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM notes WHERE id=?");
    q.addBindValue(id);
    if (!q.exec()) {
        m_err = q.lastError().text();
        return false;
    }
    return true;
}

// ---------------- AI 结果历史（本地 SQLite） ----------------
bool DbManager::openLocal(const QString &filePath) {
    const QString connName = "ai_results_local";
    if (QSqlDatabase::contains(connName))
        m_local = QSqlDatabase::database(connName);
    else
        m_local = QSqlDatabase::addDatabase("QSQLITE", connName);
    m_local.setDatabaseName(filePath);
    if (!m_local.open()) {
        m_err = m_local.lastError().text();
        return false;
    }
    return true;
}

bool DbManager::isLocalOpen() const { return m_local.isOpen(); }

bool DbManager::ensureResultsTable() {
    if (!m_local.isOpen()) return false;
    QSqlQuery q(m_local);
    if (!q.exec("CREATE TABLE IF NOT EXISTS ai_results ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                "note_id TEXT, "
                "type TEXT NOT NULL, "
                "content TEXT, "
                "created_at TEXT)")) {
        m_err = q.lastError().text();
        return false;
    }
    return true;
}

bool DbManager::ensureContentTables() {
    if (!m_local.isOpen()) return false;
    QSqlQuery q(m_local);
    if (!q.exec("CREATE TABLE IF NOT EXISTS content_items ("
                "id TEXT PRIMARY KEY, title TEXT NOT NULL, file_path TEXT, "
                "item_type TEXT NOT NULL, source_path TEXT, updated_at TEXT)")) {
        m_err = q.lastError().text();
        return false;
    }
    if (!q.exec("CREATE TABLE IF NOT EXISTS note_document_links ("
                "note_id TEXT NOT NULL, document_id TEXT NOT NULL, "
                "PRIMARY KEY(note_id, document_id))")) {
        m_err = q.lastError().text();
        return false;
    }
    return true;
}

bool DbManager::upsertContentItem(const ContentItem &item) {
    if (!m_local.isOpen()) return false;
    QSqlQuery q(m_local);
    q.prepare("INSERT OR REPLACE INTO content_items "
              "(id, title, file_path, item_type, source_path, updated_at) "
              "VALUES (?, ?, ?, ?, ?, ?)");
    q.addBindValue(item.id);
    q.addBindValue(item.title);
    q.addBindValue(item.filePath);
    q.addBindValue(item.type);
    q.addBindValue(item.sourcePath);
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!q.exec()) {
        m_err = q.lastError().text();
        return false;
    }
    return true;
}

ContentItem DbManager::loadContentItem(const QString &id) {
    ContentItem item;
    if (!m_local.isOpen()) return item;
    QSqlQuery q(m_local);
    q.prepare("SELECT id, title, file_path, item_type, source_path "
              "FROM content_items WHERE id=?");
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        item.id = q.value(0).toString();
        item.title = q.value(1).toString();
        item.filePath = q.value(2).toString();
        item.type = q.value(3).toString();
        item.sourcePath = q.value(4).toString();
    }
    return item;
}

QList<ContentItem> DbManager::loadDocuments() {
    QList<ContentItem> items;
    if (!m_local.isOpen()) return items;
    QSqlQuery q(m_local);
    if (q.exec("SELECT id, title, file_path, item_type, source_path "
               "FROM content_items WHERE item_type='document' "
               "ORDER BY title COLLATE NOCASE")) {
        while (q.next()) {
            ContentItem item;
            item.id = q.value(0).toString();
            item.title = q.value(1).toString();
            item.filePath = q.value(2).toString();
            item.type = q.value(3).toString();
            item.sourcePath = q.value(4).toString();
            items.append(item);
        }
    }
    return items;
}

bool DbManager::setLinkedDocuments(const QString &noteId,
                                   const QStringList &documentIds) {
    if (!m_local.isOpen() || noteId.isEmpty()) return false;
    if (!m_local.transaction()) return false;
    QSqlQuery q(m_local);
    q.prepare("DELETE FROM note_document_links WHERE note_id=?");
    q.addBindValue(noteId);
    if (!q.exec()) {
        m_local.rollback();
        return false;
    }
    q.prepare("INSERT INTO note_document_links(note_id, document_id) VALUES(?, ?)");
    for (const QString &documentId : documentIds) {
        q.bindValue(0, noteId);
        q.bindValue(1, documentId);
        if (!q.exec()) {
            m_local.rollback();
            return false;
        }
    }
    return m_local.commit();
}

QStringList DbManager::loadLinkedDocuments(const QString &noteId) {
    QStringList ids;
    if (!m_local.isOpen() || noteId.isEmpty()) return ids;
    QSqlQuery q(m_local);
    q.prepare("SELECT document_id FROM note_document_links "
              "WHERE note_id=? ORDER BY document_id");
    q.addBindValue(noteId);
    if (q.exec())
        while (q.next()) ids.append(q.value(0).toString());
    return ids;
}

bool DbManager::deleteContentItem(const QString &id) {
    if (!m_local.isOpen()) return false;
    QSqlQuery q(m_local);
    q.prepare("DELETE FROM note_document_links "
              "WHERE note_id=? OR document_id=?");
    q.addBindValue(id);
    q.addBindValue(id);
    q.exec();
    q.prepare("DELETE FROM content_items WHERE id=?");
    q.addBindValue(id);
    return q.exec();
}

bool DbManager::saveResult(AiResult &r) {
    if (!m_local.isOpen()) return false;
    QSqlQuery q(m_local);
    q.prepare("INSERT INTO ai_results (note_id, type, content, created_at) "
              "VALUES (?, ?, ?, ?)");
    q.addBindValue(r.noteId);
    q.addBindValue(r.type);
    q.addBindValue(r.content);
    q.addBindValue(r.createdAt);
    if (!q.exec()) {
        m_err = q.lastError().text();
        return false;
    }
    r.id = q.lastInsertId().toInt();
    return true;
}

QList<AiResult> DbManager::loadResults(const QString &type) {
    return loadResults(type, QString());
}

QList<AiResult> DbManager::loadResults(const QString &type, const QString &noteId) {
    QList<AiResult> list;
    if (!m_local.isOpen()) return list;
    QSqlQuery q(m_local);
    if (noteId.isEmpty()) {
        q.prepare("SELECT id, note_id, content, created_at FROM ai_results "
                  "WHERE type=? ORDER BY id DESC");
        q.addBindValue(type);
    } else {
        q.prepare("SELECT id, note_id, content, created_at FROM ai_results "
                  "WHERE type=? AND note_id=? ORDER BY id DESC");
        q.addBindValue(type);
        q.addBindValue(noteId);
    }
    if (q.exec()) {
        while (q.next()) {
            AiResult r;
            r.id = q.value(0).toInt();
            r.noteId = q.value(1).toString();
            r.type = type;
            r.content = q.value(2).toString();
            r.createdAt = q.value(3).toString();
            list.append(r);
        }
    } else {
        m_err = q.lastError().text();
    }
    return list;
}
