#pragma once
#include <QObject>
#include <QSqlDatabase>
#include <QList>
#include "types.h"

// 通过 QODBC 连接 MySQL（Qt5 预编译版自带 QODBC 插件，无需自编译 QMYSQL）
class DbManager : public QObject {
    Q_OBJECT
public:
    explicit DbManager(QObject *parent = nullptr);

    // odbcDriver: 已安装的 MySQL ODBC 驱动名，如 "MySQL ODBC 8.0 Unicode Driver"
    bool open(const QString &odbcDriver, const QString &host, int port,
              const QString &database, const QString &user, const QString &password);
    bool isOpen() const;
    QString lastError() const;

    void saveMessage(const QString &conversationId, const QString &role,
                     const QString &content, const QString &model = QString());
    QList<ChatMessage> loadHistory(const QString &conversationId);

    // ---- 笔记存储（MySQL）----
    bool ensureNotesTable();                       // 建 notes 表（若不存在）
    bool ensureChatMessagesTable();                 // 建 chat_messages 表（若不存在）
    bool saveNote(Note &note);                     // 新增或更新（按 id）
    Note loadNote(const QString &id);              // 按 id 取一条
    QList<Note> loadAllNotes();                    // 列表（按更新时间倒序）
    bool deleteNote(const QString &id);            // 删除一条笔记

    // ---- AI 结果历史（本地 SQLite，不依赖 MySQL）----
    bool openLocal(const QString &filePath);       // 打开本地 SQLite 结果库
    bool isLocalOpen() const;
    bool ensureResultsTable();                     // 建 ai_results 表
    bool saveResult(AiResult &r);                  // 存入一条结果
    QList<AiResult> loadResults(const QString &type); // 按功能取历史（新→旧）
    QList<AiResult> loadResults(const QString &type, const QString &noteId); // +按笔记筛选

private:
    QSqlDatabase m_db;        // MySQL（笔记）
    QSqlDatabase m_local;     // 本地 SQLite（AI 结果历史）
    QString m_err;
};
