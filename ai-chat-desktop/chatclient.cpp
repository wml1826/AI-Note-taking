#include "chatclient.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QTimer>
#include <QTextCodec>

static constexpr int NET_TIMEOUT_MS = 90000;  // 90 秒超时

ChatClient::ChatClient(QObject *parent)
    : QObject(parent), m_mgr(new QNetworkAccessManager(this)) {}

void ChatClient::setApiKey(const QString &key) { m_apiKey = key; }
void ChatClient::setBaseUrl(const QString &url) {
    m_baseUrl = url.trimmed();
    // 防御：确保有协议前缀，避免 QUrl 把纯域名当 host 解析导致 "Host not found"
    if (!m_baseUrl.startsWith("http://", Qt::CaseInsensitive) &&
        !m_baseUrl.startsWith("https://", Qt::CaseInsensitive))
        m_baseUrl.prepend("https://");
    if (!m_baseUrl.endsWith('/')) m_baseUrl += '/';
}
void ChatClient::setModel(const QString &model) { m_model = model; }

static QJsonArray buildMessages(const QList<ChatMessage> &history) {
    QJsonArray msgs;
    for (const auto &m : history) {
        QJsonObject o;
        o["role"] = m.role;
        if (!m.images.isEmpty()) {
            // 多模态：content 用数组（文本段 + 图片段）
            QJsonArray parts;
            if (!m.content.isEmpty())
                parts.append(QJsonObject{{"type", "text"}, {"text", m.content}});
            for (const auto &img : m.images) {
                QString dataUrl = "data:" + img.mime + ";base64," + QString::fromLatin1(img.base64);
                parts.append(QJsonObject{
                    {"type", "image_url"},
                    {"image_url", QJsonObject{{"url", dataUrl}}}
                });
            }
            o["content"] = parts;
        } else {
            o["content"] = m.content;
        }
        msgs.append(o);
    }
    return msgs;
}

void ChatClient::send(const QList<ChatMessage> &history) {
    QUrl url(m_baseUrl + "chat/completions");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_apiKey.isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(NET_TIMEOUT_MS);
#endif

    QJsonObject body;
    body["model"] = m_model;
    body["messages"] = buildMessages(history);
    body["stream"] = false;
    body["temperature"] = 0.7;

    QNetworkReply *reply = m_mgr->post(req, QJsonDocument(body).toJson());

    // 超时看门狗：Qt 5.15 以下无 setTransferTimeout，用 QTimer 兜底
    QTimer::singleShot(NET_TIMEOUT_MS, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QByteArray raw = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(raw);
            QString msg;
            if (doc.isObject() && doc.object().contains("error"))
                msg = doc.object()["error"].toObject()["message"].toString();
            else
                msg = reply->errorString();
            emit finishedReply(QString(), msg);
            return;
        }
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isNull() || !doc.isObject()) {
            emit finishedReply(QString(), "服务端返回非法 JSON：\n" + QString::fromUtf8(data));
            return;
        }
        QJsonObject obj = doc.object();
        if (obj.contains("error")) {
            emit finishedReply(QString(),
                "API 错误: " + obj["error"].toObject()["message"].toString());
            return;
        }
        QJsonArray choices = obj["choices"].toArray();
        if (choices.isEmpty()) {
            emit finishedReply(QString(), "无返回内容：\n" + QString::fromUtf8(data));
            return;
        }
        QString content = choices.at(0).toObject()["message"].toObject()["content"].toString();
        emit finishedReply(content, QString());
    });
}

void ChatClient::sendStream(const QList<ChatMessage> &history) {
    QUrl url(m_baseUrl + "chat/completions");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_apiKey.isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(120000);  // 流式给更长超时
#endif

    QJsonObject body;
    body["model"] = m_model;
    body["messages"] = buildMessages(history);
    body["stream"] = true;
    body["temperature"] = 0.7;

    QNetworkReply *reply = m_mgr->post(req, QJsonDocument(body).toJson());

    // 流式读取：每当有新数据可读时解析 SSE 行
    // sseBuffer 堆分配，避免 sendStream 返回后 lambda 访问悬空引用
    QByteArray *sseBuffer = new QByteArray;
    connect(reply, &QIODevice::readyRead, this, [this, reply, sseBuffer]() {
        *sseBuffer += reply->readAll();
        // 按行解析 SSE
        int idx;
        while ((idx = sseBuffer->indexOf('\n')) >= 0) {
            QByteArray line = sseBuffer->left(idx).trimmed();
            *sseBuffer = sseBuffer->mid(idx + 1);
            if (!line.startsWith("data:"))
                continue;
            QByteArray data = line.mid(5).trimmed();
            if (data == "[DONE]") {
                continue;
            }
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (!doc.isObject())
                continue;
            QJsonArray choices = doc.object()["choices"].toArray();
            if (choices.isEmpty())
                continue;
            QJsonObject delta = choices.at(0).toObject()["delta"].toObject();
            QString text = delta["content"].toString();
            if (!text.isEmpty())
                emit chunkReceived(text);
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, sseBuffer]() {
        delete sseBuffer;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit finishedReply(QString(), reply->errorString());
        } else {
            emit finishedReply(QString(), QString());  // 正常结束
        }
    });
}
