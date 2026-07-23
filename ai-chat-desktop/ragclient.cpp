#include "ragclient.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

static constexpr int NET_TIMEOUT_MS = 300000; // 5min：Jina 编码 + Milvus 全量重建大文献可达数分钟

RagClient::RagClient(QObject *parent) : QObject(parent) {
    m_mgr = new QNetworkAccessManager(this);
}

void RagClient::setBackend(const QString &url) {
    m_backend = url.trimmed();
    if (!m_backend.endsWith("/")) m_backend += "/";
}

void RagClient::setApiKey(const QString &k) { m_apiKey = k; }
// 兼容旧配置；本地 Jina Embedding 不使用该 Key，仅保留接口不破坏调用方。

void RagClient::setRewriteConfig(const QString &provider, const QString &apiKey,
                                 const QString &baseUrl, const QString &model) {
    // RAG 检索前的查询改写复用文本模型（DeepSeek / 通义千问）。
    m_rewriteProvider = provider.isEmpty() ? QStringLiteral("deepseek") : provider;
    m_rewriteApiKey = apiKey;
    m_rewriteBaseUrl = baseUrl;
    m_rewriteModel = model;
}

void RagClient::requestIndex(const QString &noteId, const QString &text,
                             const QString &title, const QString &source) {
    QJsonObject b;
    b["note_id"] = noteId;
    b["text"] = text;
    b["title"] = title;
    b["source"] = source;
    doPost(Index, b);
}

void RagClient::requestRetrieve(const QStringList &documentIds, const QString &query,
                                const QList<ChatMessage> &history, int topK) {
    QJsonObject b;
    // document_ids：空数组表示主动检索全部文献；非空则限定范围。
    QJsonArray ids;
    for (const QString &id : documentIds)
        ids.append(id);
    b["document_ids"] = ids;
    b["query"] = query;
    b["top_k"] = topK;
    // 对话历史：供后端做追问改写（指代/省略补全）。
    QJsonArray hist;
    for (const ChatMessage &m : history) {
        QJsonObject hm;
        hm["role"] = m.role;
        hm["content"] = m.content;
        hist.append(hm);
    }
    b["history"] = hist;
    // 查询改写用的文本模型配置（复用摘要/润色同一套 Key）。
    b["rewrite_provider"] = m_rewriteProvider;
    if (!m_rewriteApiKey.isEmpty())   b["rewrite_api_key"]  = m_rewriteApiKey;
    if (!m_rewriteBaseUrl.isEmpty())  b["rewrite_base_url"] = m_rewriteBaseUrl;
    if (!m_rewriteModel.isEmpty())    b["rewrite_model"]    = m_rewriteModel;
    doPost(Retrieve, b);
}

void RagClient::requestDelete(const QString &noteId) {
    QJsonObject b;
    b["note_id"] = noteId;
    doPost(Delete, b);
}

void RagClient::doPost(ReqType t, const QJsonObject &body) {
    QString ep;
    switch (t) {
        case Index:    ep = "api/rag/index";    break;
        case Retrieve: ep = "api/rag/retrieve"; break;
        case Delete:   ep = "api/rag/delete";   break;
    }
    QUrl url(m_backend + ep);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(NET_TIMEOUT_MS);
#endif

    QNetworkReply *reply = m_mgr->post(req, QJsonDocument(body).toJson());

    QTimer::singleShot(NET_TIMEOUT_MS, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [=]() {
        QString out, err;
        if (reply->error() == QNetworkReply::NoError) {
            QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            if (t == Index) {
                out = QString::number(obj.value("indexed").toInt());
            } else if (t == Delete) {
                out = obj.value("deleted").toBool() ? "ok" : "fail";
            } else {
                // Retrieve：把 chunks（字符串数组）与 sources（元数据对象数组）
                // 一并回传，由 MainWindow 解析来源引用。
                QJsonObject wrap;
                wrap["chunks"] = obj.value("chunks").toArray();
                wrap["sources"] = obj.value("sources").toArray();
                out = QString::fromUtf8(QJsonDocument(wrap).toJson(QJsonDocument::Compact));
            }
            if (out.isEmpty() && obj.contains("detail"))
                err = obj.value("detail").toString();
        } else {
            QByteArray raw = reply->readAll();
            QJsonObject obj = QJsonDocument::fromJson(raw).object();
            if (obj.contains("detail"))
                err = obj.value("detail").toString();
            else
                err = reply->errorString();
        }
        reply->deleteLater();
        emit result((int)t, out, err);
    });
}
