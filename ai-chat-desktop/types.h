#pragma once
#include <QString>
#include <QList>

// 单张图片（已 base64 编码，随消息一起发给多模态模型）
struct ImagePart {
    QByteArray base64;   // 图片二进制 base64
    QString mime;        // 如 "image/png"
};

// 一条聊天消息，UI / 网络 / 数据库三层共用
struct ChatMessage {
    QString role;    // "system" | "user" | "assistant"
    QString content;
    QList<ImagePart> images;  // 仅 user 消息可能带图（多模态）
};

// 全局设置（UI / 持久化 / 客户端共用），原先在 settingsdialog.h，重构后集中于此
struct AppSettings {
    // —— 文本 / 笔记（默认 DeepSeek，在设置页统一配置）——
    QString apiKey;
    QString baseUrl = "https://api.deepseek.com/v1";
    QString model   = "deepseek-chat";   // 文本模型（笔记功能用）
    QString backend = "http://127.0.0.1:8000";  // FastAPI 后端地址

    // —— 视觉模型（图片分析，默认通义千问 Qwen-VL；在图片分析页单独填 Key）——
    QString visionApiKey;
    QString visionBaseUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1";
    QString visionModel   = "qwen3-vl-flash";

    QString odbcDriver = "MySQL ODBC 8.0 Unicode Driver";
    QString dbHost   = "localhost";
    int     dbPort   = 3306;
    QString dbName   = "ai_chat";
    QString dbUser   = "root";
    QString dbPassword = "123456";
};

// 一条笔记（存 MySQL）
struct Note {
    QString id;        // 数据库主键（UUID）
    QString title;
    QString content;
    QString updatedAt; // 格式化时间字符串
};

// 一条 AI 结果历史（持久化到本地 SQLite，跨会话保留，不依赖 MySQL）
struct AiResult {
    int id = 0;            // 自增主键
    QString noteId;        // 关联笔记 id（可能为空）
    QString type;          // "summary" | "polish" | "rag"
    QString content;       // 结果正文（润色存润色后文本）
    QString createdAt;     // 生成时间字符串
};

struct ContentItem {
    QString id;
    QString title;
    QString filePath;
    QString type;          // "document" | "note"
    QString sourcePath;    // 原始 PDF/Word 路径，仅文献使用
};
