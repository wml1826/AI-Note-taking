#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QList>
#include "types.h"

// RAG 检索客户端：调用 Python 后端的向量检索接口
//   POST /api/rag/index    为某条笔记建索引（切块+向量化+入向量库，全在 Python 端）
//   POST /api/rag/retrieve 检索与问题最相关的 Top-K 片段（noteId 为空时全局检索）
//   POST /api/rag/delete   删除某条笔记的向量索引
// 注意：本类只做「检索」，最终「组装 Prompt + 调 LLM 生成」在 MainWindow 里用 ChatClient 完成。
class RagClient : public QObject {
    Q_OBJECT
public:
    enum ReqType { Index = 1, Retrieve = 2, Delete = 3 };

    explicit RagClient(QObject *parent = nullptr);

    void setBackend(const QString &url);
    void setRewriteConfig(const QString &provider, const QString &apiKey,
                          const QString &baseUrl, const QString &model);
    void setApiKey(const QString &k);   // 兼容旧配置；本地 Jina 不使用该 Key

    void requestIndex(const QString &noteId, const QString &text,
                      const QString &title = QString(),
                      const QString &source = QString());
    void requestRetrieve(const QStringList &documentIds, const QString &query,
                         const QList<ChatMessage> &history, int topK = 10);
    void requestDelete(const QString &noteId);  // 删除笔记索引

signals:
    // type: Index / Retrieve / Delete；Retrieve 时 text 为 JSON {"chunks":[...]}
    void result(int type, const QString &text, const QString &error);

private:
    void doPost(ReqType t, const QJsonObject &body);
    QNetworkAccessManager *m_mgr;
    QString m_backend = "http://127.0.0.1:8000";
    QString m_apiKey;
    QString m_rewriteProvider = "deepseek";
    QString m_rewriteApiKey;
    QString m_rewriteBaseUrl;
    QString m_rewriteModel;
};
