#include "ragclient.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

static constexpr int NET_TIMEOUT_MS = 90000;

RagClient::RagClient(QObject *parent) : QObject(parent) {
    m_mgr = new QNetworkAccessManager(this);
}

void RagClient::setBackend(const QString &url) {
    m_backend = url.trimmed();
    if (!m_backend.endsWith("/")) m_backend += "/";
}
void RagClient::setApiKey(const QString &k) { m_apiKey = k; }

void RagClient::requestIndex(const QString &noteId, const QString &text) {
    QJsonObject b;
    b["note_id"] = noteId;
    b["text"] = text;
    b["api_key"] = m_apiKey;
    doPost(Index, b);
}

void RagClient::requestRetrieve(const QString &noteId, const QString &query, int topK) {
    QJsonObject b;
    // noteId 为空时走全局检索
    if (!noteId.isEmpty())
        b["note_id"] = noteId;
    b["query"] = query;
    b["top_k"] = topK;
    b["api_key"] = m_apiKey;
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
                // 把 chunks 数组原样回传，由 MainWindow 解析
                QJsonArray arr = obj.value("chunks").toArray();
                QJsonObject wrap;
                wrap["chunks"] = arr;
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
