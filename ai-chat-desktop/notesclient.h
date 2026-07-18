#pragma once
#include <QObject>
#include <QNetworkAccessManager>

// 调用本地 FastAPI 后端（智能笔记服务）的客户端
// 后端默认地址 http://127.0.0.1:8000
class NotesClient : public QObject {
    Q_OBJECT
public:
    enum ReqType { Summary = 1, Polish = 2, Import = 3 };

    explicit NotesClient(QObject *parent = nullptr);

    void setBackend(const QString &url);   // 如 http://127.0.0.1:8000
    void setApiKey(const QString &k);
    void setBaseUrl(const QString &u);     // 云端模型 BaseURL（透传给后端）
    void setModel(const QString &m);       // 文本模型名
    void setProvider(const QString &p);    // deepseek / dashscope

    void requestSummary(const QString &text, const QString &detail = "medium");
    void requestPolish(const QString &text);

    // 流式请求：逐块发 chunkReceived，完成时发 result
    void requestSummaryStream(const QString &text, const QString &detail = "medium");
    void requestPolishStream(const QString &text);

    // 文档导入：上传 PDF/Word 文件，后端提取纯文本
    void requestImport(const QString &filePath);

signals:
    // type 对应 ReqType；text 为结果，error 非空表示出错
    void result(int type, const QString &text, const QString &error);
    void chunkReceived(const QString &chunk);  // 流式增量文本

private:
    void doPost(ReqType t, const QJsonObject &body);
    void doStreamPost(ReqType t, const QJsonObject &body);
    QNetworkAccessManager *m_mgr;
    QString m_backend = "http://127.0.0.1:8000";
    QString m_apiKey, m_baseUrl, m_model = "deepseek-chat", m_provider = "deepseek";
};
