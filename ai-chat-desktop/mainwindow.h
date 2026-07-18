#pragma once
#include <QMainWindow>
#include "types.h"
#include "chatclient.h"
#include "notesclient.h"
#include "ragclient.h"
#include "dbmanager.h"
#include "ui_mainwindow.h"

namespace Ui {
class ImageAnalysisPage;
class SettingsPage;
}

class QTimer;
class QListWidgetItem;
class QDir;
class QPushButton;
class QLineEdit;
class QCheckBox;

// 本地笔記条目（从 .md 文件扫描得到，不依赖 MySQL）
struct LocalNote {
    QString filePath;     // 完整路径（作为唯一标识）
    QString title;        // 标题（取文件名或首行）
    QString mtime;        // 修改时间
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // 笔记页
    void onSummary();
    void onPolish();
    void onNewNote();
    void onSaveNote();
    void onImportFile();                          // 导入 PDF/Word
    void onNotesResult(int type, const QString &text, const QString &error);

    // 流式接收
    void onNotesChunk(const QString &chunk);
    void onRagChunk(const QString &chunk);

    // 笔记列表 / 管理
    void onNoteSelected(QListWidgetItem *current, QListWidgetItem *previous);
    void onDeleteNote();
    void refreshNoteList();                     // 合并本地 .md + MySQL 刷新列表
    void loadNoteIntoEditor(const QString &id);
    void loadLocalNoteIntoEditor(const QString &filePath);
    void populateHistories();
    void saveAiResult(const QString &type, const QString &content);

    // 笔记搜索
    void onSearchChanged(const QString &keyword);

    // AI 结果历史点击回看
    void onSummaryHistoryClicked(QListWidgetItem *item);
    void onPolishHistoryClicked(QListWidgetItem *item);
    void onRagHistoryClicked(QListWidgetItem *item);

    // 笔记页 · RAG
    void onIndexNote();                       // 建立向量索引
    void onRagQA();                           // 检索 + 生成（Ctrl+4）
    void onRagResult(int type, const QString &text, const QString &error);
    void onRagGenerated(const QString &reply, const QString &error);

    // 图片分析页
    void onImgSelect();
    void onImgAnalyze();
    void onImgReply(const QString &reply, const QString &error);

    // 设置页
    void onProviderChanged(const QString &p);
    void onSaveSettings();

    // 其它
    void onNavChanged(int row);
    void onMemTimer();
    void onClearEdit();
    void onAbout();
    void togglePreviewMode();             // Markdown 预览/编辑切换（Ctrl+P）
    bool tryReconnectDbSilently();
    void checkApiKeyConfig();

private:
    void loadSettings();
    void saveSettings();
    void applySettingsToClients();
    void updateStatusModel();
    void updateWindowTitle();
    void newNote();
    void showPolishDiff(const QString &orig, const QString &polished);
    void setNoteButtonsEnabled(bool on);
    void indexCurrentNote();
    QString renderMarkdown(const QString &md);   // 轻量 Markdown → HTML

    // ---- 本地文件存储（.md）----
    bool saveNoteToLocal();
    QList<LocalNote> scanLocalNotes();
    void autoSaveDraft();
    QString noteFilePath(const QString &title);
    bool deleteLocalNote(const QString &filePath);

    Ui::MainWindow *ui = nullptr;
    Ui::ImageAnalysisPage *m_imageUi = nullptr;
    Ui::SettingsPage *m_settingsUi = nullptr;
    AppSettings m_s;

    NotesClient *m_notes = nullptr;
    ChatClient *m_imgClient = nullptr;
    ChatClient *m_chat = nullptr;          // RAG 最终生成（支持流式）
    RagClient *m_rag = nullptr;
    DbManager *m_db = nullptr;

    QTimer *m_memTimer = nullptr;

    bool m_notesBusy = false;
    QString m_lastNoteText;
    QString m_streamingText;             // 流式累积文本

    // 笔记页 · RAG 状态
    QString m_currentNoteId;
    QString m_ragQuestion;
    bool m_ragBusy = false;
    QList<ChatMessage> m_ragHistory;     // 多轮 RAG 对话历史

    // 图片分析页状态
    ImagePart m_imgPart;
    QString m_imgUserContent;
    bool m_imgBusy = false;

    // 本地存储
    QString m_notesDir;
    QString m_currentLocalPath;
    bool   m_noteDirty = false;

    // 笔记搜索
    QLineEdit *m_searchBox = nullptr;
    // Markdown 预览
    bool m_previewMode = false;
    QString m_savedEditText;             // 切预览时暂存编辑内容
    // 全局检索开关
    QCheckBox *m_globalRag = nullptr;
    // 自定义图片分析提示词
    QLineEdit *m_imgPrompt = nullptr;
};
