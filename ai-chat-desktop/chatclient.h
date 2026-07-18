#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QList>
#include "types.h"

// 调用 OpenAI 兼容的 Chat Completions 接口（DeepSeek / Qwen / 本地 llama.cpp 通用）
// 默认按 DeepSeek 配置，可在设置里改成其他兼容端点。
class ChatClient : public QObject {
    Q_OBJECT
public:
    explicit ChatClient(QObject *parent = nullptr);

    void setApiKey(const QString &key);
    void setBaseUrl(const QString &url);  // 例如 https://api.deepseek.com/v1
    void setModel(const QString &model);

    // 发送整段历史（非流式），完成/出错时发 finishedReply 信号
    void send(const QList<ChatMessage> &history);

    // 流式发送：逐块发 chunkReceived，全部完成时发 finishedReply
    void sendStream(const QList<ChatMessage> &history);

signals:
    void finishedReply(const QString &reply, const QString &error);
    void chunkReceived(const QString &chunk);  // 流式增量文本

private:
    QNetworkAccessManager *m_mgr;
    QString m_apiKey;
    QString m_baseUrl = "https://api.deepseek.com/v1";
    QString m_model   = "deepseek-chat";
};
