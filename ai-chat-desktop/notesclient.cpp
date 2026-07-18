#include "notesclient.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>

static constexpr int NET_TIMEOUT_MS = 90000;

NotesClient::NotesClient(QObject *parent) : QObject(parent) {
    m_mgr = new QNetworkAccessManager(this);
}

void NotesClient::setBackend(const QString &url) {
    m_backend = url.trimmed();
    if (!m_backend.endsWith("/")) m_backend += "/";
}
void NotesClient::setApiKey(const QString &k)  { m_apiKey = k; }
void NotesClient::setBaseUrl(const QString &u) { m_baseUrl = u; }
void NotesClient::setModel(const QString &m)   { m_model = m; }
void NotesClient::setProvider(const QString &p){ m_provider = p; }

void NotesClient::requestSummary(const QString &text, const QString &detail) {
    QJsonObject b;
    b["text"] = text;
    b["detail"] = detail;
    b["provider"] = m_provider;
    b["api_key"] = m_apiKey;
    b["base_url"] = m_baseUrl;
    b["model"] = m_model;
    doPost(Summary, b);
}

void NotesClient::requestPolish(const QString &text) {
    QJsonObject b;
    b["text"] = text;
    b["provider"] = m_provider;
    b["api_key"] = m_apiKey;
    b["base_url"] = m_baseUrl;
    b["model"] = m_model;
    doPost(Polish, b);
}

void NotesClient::requestSummaryStream(const QString &text, const QString &detail) {
    QJsonObject b;
    b["text"] = text;
    b["detail"] = detail;
    b["provider"] = m_provider;
    b["api_key"] = m_apiKey;
    b["base_url"] = m_baseUrl;
    b["model"] = m_model;
    doStreamPost(Summary, b);
}

void NotesClient::requestPolishStream(const QString &text) {
    QJsonObject b;
    b["text"] = text;
    b["provider"] = m_provider;
    b["api_key"] = m_apiKey;
    b["base_url"] = m_baseUrl;
    b["model"] = m_model;
    doStreamPost(Polish, b);
}

void NotesClient::doPost(ReqType t, const QJsonObject &body) {
    QString ep = (t == Summary) ? "api/summary" : "api/polish";

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
            if (t == Summary)       out = obj.value("summary").toString();
            else if (t == Polish)   out = obj.value("polished").toString();
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

void NotesClient::doStreamPost(ReqType t, const QJsonObject &body) {
    QString ep = (t == Summary) ? "api/summary/stream" : "api/polish/stream";

    QUrl url(m_backend + ep);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(120000);
#endif

    QNetworkReply *reply = m_mgr->post(req, QJsonDocument(body).toJson());

    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        QByteArray data = reply->readAll();
        emit chunkReceived(QString::fromUtf8(data));
    });

    connect(reply, &QNetworkReply::finished, this, [=]() {
        if (reply->error() != QNetworkReply::NoError) {
            emit result((int)t, QString(), reply->errorString());
        } else {
            emit result((int)t, QString(), QString());  // 正常结束
        }
        reply->deleteLater();
    });
}

void NotesClient::requestImport(const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit result((int)Import, QString(), "无法打开文件：" + filePath);
        return;
    }
    QByteArray fileData = file.readAll();
    file.close();

    QFileInfo fi(filePath);
    QString filename = fi.fileName();

    // 构建 multipart/form-data
    QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"file\"; filename=\"%1\"").arg(filename));
    // 根据扩展名设置 Content-Type
    QString ext = fi.suffix().toLower();
    if (ext == "pdf")
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, "application/pdf");
    else if (ext == "docx")
        filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                           "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    filePart.setBody(fileData);
    multiPart->append(filePart);

    QUrl url(m_backend + "api/import");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "ai-chat-desktop");

    QNetworkReply *reply = m_mgr->post(req, multiPart);
    multiPart->setParent(reply);  // reply 删除时一并释放

    QTimer::singleShot(NET_TIMEOUT_MS, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [=]() {
        if (reply->error() != QNetworkReply::NoError) {
            emit result((int)Import, QString(), reply->errorString());
        } else {
            QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
            QString title = obj.value("title").toString();
            QString content = obj.value("content").toString();
            if (content.isEmpty()) {
                QString err = obj.value("detail").toString();
                emit result((int)Import, QString(), err.isEmpty() ? "导入失败：内容为空" : err);
            } else {
                // title||content 格式，用 \n 分隔
                emit result((int)Import, title + "\n" + content, QString());
            }
        }
        reply->deleteLater();
    });
}
