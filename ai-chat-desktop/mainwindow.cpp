#include "mainwindow.h"
#include <QMenuBar>
#include <QTimer>
#include <QTextEdit>
#include <QPushButton>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QUuid>
#include <QDateTime>
#include <QShortcut>
#include <QRegularExpression>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QListWidget>
#include <QCheckBox>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QPixmap>
#include <QDir>
#include <QSet>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include "ui_imageanalysispage.h"
#include "ui_settingspage.h"
#include <windows.h>
#include <psapi.h>

static QString mimeFromSuffix(const QString &path) {
    QString s = path.toLower();
    if (s.endsWith(".png")) return "image/png";
    if (s.endsWith(".jpg") || s.endsWith(".jpeg")) return "image/jpeg";
    if (s.endsWith(".gif")) return "image/gif";
    if (s.endsWith(".bmp")) return "image/bmp";
    if (s.endsWith(".webp")) return "image/webp";
    return "application/octet-stream";
}

static QString stableDocumentId(const QString &path) {
    QByteArray digest = QCryptographicHash::hash(
        QFileInfo(path).absoluteFilePath().toUtf8(), QCryptographicHash::Sha256
    ).toHex().left(32);
    QString raw = QString::fromLatin1(digest);
    return QString("%1-%2-%3-%4-%5")
        .arg(raw.mid(0, 8), raw.mid(8, 4), raw.mid(12, 4),
             raw.mid(16, 4), raw.mid(20, 12));
}

// 扫描系统已安装的 MySQL/MariaDB ODBC 驱动名（64 位视图）
static QStringList detectMysqlOdbcDrivers() {
    QStringList out;
    QSettings reg("HKEY_LOCAL_MACHINE\\SOFTWARE\\ODBC\\ODBCINST.INI\\ODBC Drivers",
                  QSettings::NativeFormat);
    for (const QString &name : reg.childKeys()) {
        if (name.contains("MySQL", Qt::CaseInsensitive) ||
            name.contains("MariaDB", Qt::CaseInsensitive)) {
            out.append(name);
        }
    }
    return out;
}

// 词级 LCS 差异高亮（原文红色删除线，润色后绿色）
static QString diffHtml(const QString &a, const QString &b) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QStringList ta = a.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QStringList tb = b.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
#else
    QStringList ta = a.split(QRegularExpression("\\s+"), QString::SkipEmptyParts);
    QStringList tb = b.split(QRegularExpression("\\s+"), QString::SkipEmptyParts);
#endif
    int n = ta.size(), m = tb.size();
    QVector<QVector<int>> dp(n + 1, QVector<int>(m + 1, 0));
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            dp[i][j] = (ta[i] == tb[j]) ? dp[i + 1][j + 1] + 1
                                        : qMax(dp[i + 1][j], dp[i][j + 1]);
    auto esc = [](const QString &s) { return s.toHtmlEscaped(); };
    int i = 0, j = 0;
    QString out;
    while (i < n && j < m) {
        if (ta[i] == tb[j]) { out += esc(ta[i]) + " "; ++i; ++j; }
        else if (dp[i + 1][j] >= dp[i][j + 1]) {
            out += "<span style='color:#b91c1c;text-decoration:line-through'>" + esc(ta[i]) + "</span> ";
            ++i;
        } else {
            out += "<span style='color:#15803d'>" + esc(tb[j]) + "</span> ";
            ++j;
        }
    }
    while (i < n) { out += "<span style='color:#b91c1c;text-decoration:line-through'>" + esc(ta[i]) + "</span> "; ++i; }
    while (j < m) { out += "<span style='color:#15803d'>" + esc(tb[j]) + "</span> "; ++j; }
    return out;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    ui = new Ui::MainWindow;
    ui->setupUi(this);
    m_imageUi = new Ui::ImageAnalysisPage;
    m_imageUi->setupUi(ui->pageImage);
    m_settingsUi = new Ui::SettingsPage;
    m_settingsUi->setupUi(ui->pageSettings);
    loadSettings();

    m_notes = new NotesClient(this);
    m_imgClient = new ChatClient(this);   // 图片分析直连云端（视觉模型）
    m_chat = new ChatClient(this);        // RAG 最终生成（文本模型，直连云端）
    m_rag = new RagClient(this);          // RAG 检索（调用 Python 向量库）
    m_db = new DbManager(this);
    connect(m_notes, &NotesClient::result, this, &MainWindow::onNotesResult);
    connect(m_notes, &NotesClient::chunkReceived, this, &MainWindow::onNotesChunk);
    connect(m_imgClient, &ChatClient::finishedReply, this, &MainWindow::onImgReply);
    connect(m_rag, &RagClient::result, this, &MainWindow::onRagResult);
    connect(m_chat, &ChatClient::finishedReply, this, &MainWindow::onRagGenerated);
    connect(m_chat, &ChatClient::chunkReceived, this, &MainWindow::onRagChunk);

    // Static navigation and combo-box options are defined in mainwindow.ui.
    m_settingsUi->m_provider->setCurrentText(m_s.baseUrl.contains("dashscope", Qt::CaseInsensitive)
                                   ? "通义千问" : "DeepSeek");
    QStringList drivers = detectMysqlOdbcDrivers();
    for (const QString &driver : drivers) {
        if (m_settingsUi->m_odbc->findText(driver) == -1)
            m_settingsUi->m_odbc->addItem(driver);
    }
    if (m_settingsUi->m_odbc->findText(m_s.odbcDriver) == -1)
        m_settingsUi->m_odbc->addItem(m_s.odbcDriver);
    m_settingsUi->m_odbc->setCurrentText(m_s.odbcDriver);

    if (m_imageUi->m_imgModel->findText(m_s.visionModel) == -1 && !m_s.visionModel.isEmpty())
        m_imageUi->m_imgModel->addItem(m_s.visionModel);
    m_imageUi->m_imgModel->setCurrentText(m_s.visionModel);

    // 状态栏：内存标签放到右侧
    ui->statusbar->addPermanentWidget(ui->statusMem);

    // ---- 信号连接 ----
    connect(ui->btnSummary,  &QPushButton::clicked, this, &MainWindow::onSummary);
    connect(ui->btnPolish,   &QPushButton::clicked, this, &MainWindow::onPolish);
    connect(ui->btnNewNote,  &QPushButton::clicked, this, &MainWindow::onNewNote);
    connect(ui->btnSaveNote, &QPushButton::clicked, this, &MainWindow::onSaveNote);
    // 动态添加「导入」按钮到笔记按钮行
    {
        QPushButton *btnImport = new QPushButton("导入", this);
        btnImport->setToolTip("导入 PDF / Word / TXT / Markdown 为只读文献，并建立检索索引");
        ui->noteTitleRow->insertWidget(ui->noteTitleRow->count() - 1, btnImport);
        connect(btnImport, &QPushButton::clicked, this, &MainWindow::onImportFile);
    }
    connect(m_imageUi->btnImgSelect,   &QPushButton::clicked, this, &MainWindow::onImgSelect);
    connect(m_imageUi->btnImgAnalyze,  &QPushButton::clicked, this, &MainWindow::onImgAnalyze);
    connect(m_settingsUi->btnSave, &QPushButton::clicked, this, &MainWindow::onSaveSettings);
    connect(ui->btnIndex, &QPushButton::clicked, this, &MainWindow::onIndexNote);
    connect(ui->btnRagQA, &QPushButton::clicked, this, &MainWindow::onRagQA);
    connect(ui->navList, &QListWidget::currentRowChanged, this, &MainWindow::onNavChanged);
    connect(m_settingsUi->m_provider, &QComboBox::currentTextChanged, this, &MainWindow::onProviderChanged);
    connect(ui->actionNewNote, &QAction::triggered, this, &MainWindow::onNewNote);
    connect(ui->actionExit, &QAction::triggered, this, &QMainWindow::close);
    connect(ui->actionClear, &QAction::triggered, this, &MainWindow::onClearEdit);
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::onAbout);

    // 笔记列表 / 管理
    connect(ui->m_noteList, &QListWidget::currentItemChanged, this, &MainWindow::onNoteSelected);
    connect(ui->btnDeleteNote, &QPushButton::clicked, this, &MainWindow::onDeleteNote);
    // AI 结果历史点击回看
    connect(ui->m_summaryHistory, &QListWidget::itemClicked, this, &MainWindow::onSummaryHistoryClicked);
    connect(ui->m_polishHistory, &QListWidget::itemClicked, this, &MainWindow::onPolishHistoryClicked);
    connect(ui->m_ragHistory, &QListWidget::itemClicked, this, &MainWindow::onRagHistoryClicked);

    // 快捷键
    new QShortcut(QKeySequence("Ctrl+1"), this, SLOT(onSummary()));
    new QShortcut(QKeySequence("Ctrl+2"), this, SLOT(onPolish()));
    new QShortcut(QKeySequence("Ctrl+4"), this, SLOT(onRagQA()));
    new QShortcut(QKeySequence("Ctrl+S"), this, SLOT(onSaveNote()));
    new QShortcut(QKeySequence("Ctrl+P"), this, SLOT(togglePreviewMode()));

    // ---- 动态新增 UI 控件 ----
    // 笔记搜索框（插入到笔记列表上方）
    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText("搜索文献或笔记标题、内容…");
    ui->notesLayout->insertWidget(0, m_searchBox);
    connect(m_searchBox, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);

    // 全局检索复选框（插入到 RAG 按钮行）
    m_linkDocumentsButton = new QPushButton("关联文献", this);
    m_linkDocumentsButton->setToolTip("为当前笔记选择 RAG 问答使用的文献");
    ui->ragRow->addWidget(m_linkDocumentsButton);
    connect(m_linkDocumentsButton, &QPushButton::clicked,
            this, &MainWindow::onSelectLinkedDocuments);

    m_globalRag = new QCheckBox("全部文献", this);
    m_globalRag->setToolTip("主动跨全部已索引文献检索，可能混入无关内容");
    ui->ragRow->addWidget(m_globalRag);

    // 自定义图片分析提示词（插入到图片分析按钮行上方）
    m_imgPrompt = new QLineEdit(this);
    m_imgPrompt->setPlaceholderText("自定义分析提示词（留空则默认：请分析这张图片的内容）");
    m_imageUi->imgLayout->insertWidget(1, m_imgPrompt);

    // 从 exe 目录往上找项目根目录（含 .pro 文件的目录）
    // notes/ 和 ai_results.db 都存在这里，不随构建目录消失
    QString projectDir = QCoreApplication::applicationDirPath();
    {
        QDir d(projectDir);
        while (!d.exists("ai_chat_desktop.pro") && d.cdUp()) {}
        if (d.exists("ai_chat_desktop.pro"))
            projectDir = d.absolutePath();
    }

    // 本地笔记目录（存到源码目录，开箱即用）
    m_notesDir = projectDir + "/notes";
    QDir().mkpath(m_notesDir);

    // 编辑器内容变化 → 标记未保存
    connect(ui->m_noteEdit, &QTextEdit::textChanged, this, [this]() {
        if (!m_noteDirty && !ui->m_noteEdit->toPlainText().isEmpty()) {
            m_noteDirty = true;
            updateWindowTitle();
        }
    });
    connect(ui->m_noteTitle, &QLineEdit::textChanged, this, [this]() {
        if (!m_noteDirty) { m_noteDirty = true; updateWindowTitle(); }
    });

    m_memTimer = new QTimer(this);
    connect(m_memTimer, &QTimer::timeout, this, &MainWindow::onMemTimer);
    m_memTimer->start(5000);  // 5 秒轮询，降低 CPU 开销

    // ---- 打开数据库 ----
    // 本地 SQLite：保存 AI 结果历史（存到源码目录，不随构建目录消失）
    QString localDb = projectDir + "/ai_results.db";
    if (!m_db->openLocal(localDb)) {
        qWarning() << "本地结果库打开失败：" << m_db->lastError();
    } else {
        m_db->ensureResultsTable();
        m_db->ensureContentTables();
    }

    if (!m_db->open(m_s.odbcDriver, m_s.dbHost, m_s.dbPort,
                    m_s.dbName, m_s.dbUser, m_s.dbPassword)) {
        QMessageBox::warning(this, "数据库",
            "无法连接 MySQL：\n" + m_db->lastError() +
            "\n请确认已安装 MySQL ODBC Connector，且服务已启动。\n"
            "（AI 结果历史仍可正常使用，笔记需 MySQL 才能保存/浏览。）");
    } else {
        m_db->ensureNotesTable();
        m_db->ensureChatMessagesTable();
    }
    // 刷新笔记列表 & 载入 AI 历史
    refreshNoteList();
    populateHistories();
    applySettingsToClients();
    // 设置页视觉模型三项回填（与图片页共用同一份配置）
    m_settingsUi->m_visionApiKey->setText(m_s.visionApiKey);
    m_settingsUi->m_visionBaseUrl->setText(m_s.visionBaseUrl);
    m_settingsUi->m_visionModel->setText(m_s.visionModel);
    // 设置页文本模型四项回填（修复：此前未回填，打开设置页看不到已存 Key）
    m_settingsUi->m_apiKey->setText(m_s.apiKey);
    m_settingsUi->m_baseUrl->setText(m_s.baseUrl);
    m_settingsUi->m_model->setText(m_s.model);
    m_settingsUi->m_backend->setText(m_s.backend);
    updateStatusModel();
    newNote();
    checkApiKeyConfig();   // 启动检查：未配置 Key 则提示去设置页
}

MainWindow::~MainWindow() {
    delete m_settingsUi;
    delete m_imageUi;
    delete ui;
}

void MainWindow::loadSettings() {
    QSettings st("AiChat", "Desktop");
    m_s.apiKey = st.value("apiKey").toString();
    m_s.baseUrl = st.value("baseUrl", m_s.baseUrl).toString();
    m_s.model = st.value("model", m_s.model).toString();
    m_s.backend = st.value("backend", m_s.backend).toString();
    m_s.visionApiKey = st.value("visionApiKey").toString();
    m_s.visionBaseUrl = st.value("visionBaseUrl", m_s.visionBaseUrl).toString();
    m_s.visionModel = st.value("visionModel", m_s.visionModel).toString();
    m_s.odbcDriver = st.value("odbcDriver", m_s.odbcDriver).toString();
    m_s.dbHost = st.value("dbHost", m_s.dbHost).toString();
    m_s.dbPort = st.value("dbPort", m_s.dbPort).toInt();
    m_s.dbName = st.value("dbName", m_s.dbName).toString();
    m_s.dbUser = st.value("dbUser", m_s.dbUser).toString();
    m_s.dbPassword = st.value("dbPassword", m_s.dbPassword).toString();

    // 笔记用文本模型：若当前是视觉模型（如 qwen3-vl-flash），自动切到文本模型
    if (m_s.model.contains("vl", Qt::CaseInsensitive)) {
        m_s.model = m_s.baseUrl.contains("dashscope", Qt::CaseInsensitive)
                        ? "qwen-plus" : "deepseek-chat";
    }
}

void MainWindow::saveSettings() {
    QSettings st("AiChat", "Desktop");
    st.setValue("apiKey", m_s.apiKey);
    st.setValue("baseUrl", m_s.baseUrl);
    st.setValue("model", m_s.model);
    st.setValue("backend", m_s.backend);
    st.setValue("visionApiKey", m_s.visionApiKey);
    st.setValue("visionBaseUrl", m_s.visionBaseUrl);
    st.setValue("visionModel", m_s.visionModel);
    st.setValue("odbcDriver", m_s.odbcDriver);
    st.setValue("dbHost", m_s.dbHost);
    st.setValue("dbPort", m_s.dbPort);
    st.setValue("dbName", m_s.dbName);
    st.setValue("dbUser", m_s.dbUser);
    st.setValue("dbPassword", m_s.dbPassword);
}

void MainWindow::applySettingsToClients() {
    m_notes->setBackend(m_s.backend);
    m_notes->setApiKey(m_s.apiKey);
    m_notes->setBaseUrl(m_s.baseUrl);
    m_notes->setModel(m_s.model);
    m_notes->setProvider(m_s.baseUrl.contains("dashscope", Qt::CaseInsensitive)
                             ? "dashscope" : "deepseek");

    // 图片分析用视觉模型（通义千问 qwen3-vl-flash），使用独立的视觉 Key / BaseURL
    m_imgClient->setApiKey(m_s.visionApiKey);
    m_imgClient->setBaseUrl(m_s.visionBaseUrl);
    m_imgClient->setModel(m_s.visionModel);

    // RAG：最终生成用文本模型（直连云端，与图片视觉模型区分）
    m_chat->setApiKey(m_s.apiKey);
    m_chat->setBaseUrl(m_s.baseUrl);
    m_chat->setModel(m_s.model);
    // RAG 向量化由 FastAPI 后端的本地 Jina 模型完成，无需 API Key。
    m_rag->setApiKey(QString());
    m_rag->setBackend(m_s.backend);
    m_rag->setRewriteConfig(
        m_s.baseUrl.contains("dashscope", Qt::CaseInsensitive)
            ? "dashscope" : "deepseek",
        m_s.apiKey, m_s.baseUrl, m_s.model);
}

void MainWindow::updateStatusModel() {
    QString prov = m_s.baseUrl.contains("dashscope", Qt::CaseInsensitive)
                       ? "通义千问" : "DeepSeek";
    ui->statusModel->setText(QString("模型: %1 (%2)").arg(m_s.model, prov));
}

// ---- MySQL 全自动静默连接（不暴露给用户）----

bool MainWindow::tryReconnectDbSilently() {
    // 静默重试 MySQL 连接，返回是否成功。用于 Ctrl+S 时自动重连。
    if (m_db && m_db->isOpen()) return true;
    if (m_s.odbcDriver.isEmpty() || m_s.dbHost.isEmpty()) return false;
    if (m_db->open(m_s.odbcDriver, m_s.dbHost, m_s.dbPort,
                    m_s.dbName, m_s.dbUser, m_s.dbPassword)) {
        m_db->ensureNotesTable();
        m_db->ensureChatMessagesTable();
        qInfo() << "[MySQL] 自动重连成功:" << m_s.dbHost << m_s.dbName;
        refreshNoteList();
        return true;
    }
    qWarning() << "[MySQL] 自动重连失败:" << m_db->lastError();
    return false;
}

void MainWindow::checkApiKeyConfig() {
    // 文本生成使用 m_s.apiKey；仅文献图表解析需要视觉模型 Key。
    if (!m_s.apiKey.isEmpty() && !m_s.visionApiKey.isEmpty())
        return;  // 两个都配了，正常
    QString missing;
    if (m_s.apiKey.isEmpty())       missing += "\n• 文本模型 API Key（DeepSeek，用于摘要/润色/RAG 生成）";
    if (m_s.visionApiKey.isEmpty()) missing += "\n• 视觉模型 API Key（通义千问，仅用于文献图表解析）";
    QMessageBox::warning(this, "未配置 API Key",
        "检测到以下大模型密钥尚未在设置页填写，相关 AI 功能将无法使用：" + missing +
        "\n\n请打开左侧「设置」页填写后保存。");
    ui->navList->setCurrentRow(2);   // 自动跳到设置页
}

void MainWindow::newNote() {
    ui->m_noteTitle->clear();
    ui->m_noteEdit->clear();
    ui->m_qaInput->clear();
    ui->m_summaryView->clear();
    ui->m_polishView->clear();
    ui->m_ragView->clear();
    ui->m_noteList->setCurrentItem(nullptr);
    m_currentNoteId.clear();   // 新笔记尚未分配稳定 id
    m_currentLocalPath.clear();
    m_importSourcePath.clear();
    m_currentContentType = "note";
    m_linkedDocumentIds.clear();
    m_ragHistory.clear();
    m_noteDirty = false;
    updateWindowTitle();
    ui->m_noteStatus->setText("就绪（新建笔记）");
    applyContentMode();
    populateHistories();   // 新笔记无历史，清空历史列表
}

void MainWindow::setNoteButtonsEnabled(bool on) {
    ui->btnSummary->setEnabled(on);
    ui->btnPolish->setEnabled(on);
    ui->btnNewNote->setEnabled(on);
    ui->btnSaveNote->setEnabled(on);
    ui->btnIndex->setEnabled(on);
    ui->btnRagQA->setEnabled(on);
    if (on) applyContentMode();
}

void MainWindow::applyContentMode() {
    const bool isDocument = m_currentContentType == "document";
    ui->m_noteEdit->setReadOnly(isDocument || m_previewMode);
    ui->m_noteTitle->setReadOnly(isDocument);
    ui->btnPolish->setEnabled(!isDocument && !m_notesBusy && !m_ragBusy);
    ui->btnIndex->setVisible(isDocument);
    m_linkDocumentsButton->setVisible(!isDocument);
    if (isDocument) {
        ui->btnRagQA->setText("当前文献问答 (Ctrl+4)");
        m_globalRag->setChecked(false);
    } else {
        ui->btnRagQA->setText("关联文献问答 (Ctrl+4)");
        m_linkDocumentsButton->setText(
            QString("关联文献 (%1)").arg(m_linkedDocumentIds.size()));
    }
}

void MainWindow::onSummary() {
    QString text = ui->m_noteEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, "提示", "请先在左侧写入笔记内容。");
        return;
    }
    autoSaveDraft();
    if (m_notesBusy) return;
    m_notesBusy = true;
    setNoteButtonsEnabled(false);
    m_lastNoteText = text;
    m_streamingText.clear();
    ui->m_noteStatus->setText("摘要生成中…");
    ui->m_summaryView->clear();
    m_notes->requestSummaryStream(text, "medium");
}

void MainWindow::onPolish() {
    if (m_currentContentType == "document") {
        QMessageBox::information(this, "提示", "文献原文为只读内容，不能进行学术改写。");
        return;
    }
    QString text = ui->m_noteEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, "提示", "请先在左侧写入笔记内容。");
        return;
    }
    autoSaveDraft();
    if (m_notesBusy) return;
    m_notesBusy = true;
    setNoteButtonsEnabled(false);
    m_lastNoteText = text;
    m_streamingText.clear();
    ui->m_noteStatus->setText("学术改写中…");
    ui->m_polishView->clear();
    m_notes->requestPolishStream(text);
}

void MainWindow::onNotesChunk(const QString &chunk) {
    m_streamingText += chunk;
    // 判断当前是摘要还是润色：看状态栏文本
    if (ui->m_noteStatus->text().contains("摘要"))
        ui->m_summaryView->setPlainText(m_streamingText);
    else if (ui->m_noteStatus->text().contains("改写"))
        ui->m_polishView->setPlainText(m_streamingText);
}

void MainWindow::onNewNote() {
    if (m_notesBusy) return;
    newNote();
}

void MainWindow::onSaveNote() {
    QString content = ui->m_noteEdit->toPlainText();
    if (content.isEmpty()) {
        QMessageBox::information(this, "提示",
            m_currentContentType == "document"
                ? "文献内容为空，未保存。"
                : "笔记内容为空，未保存。");
        return;
    }
    QString title = ui->m_noteTitle->text().trimmed();
    if (title.isEmpty()) title = "未命名笔记";

    // ---- 1) 本地 .md 文件（始终执行，不依赖 MySQL）----
    bool localOk = saveNoteToLocal();
    if (localOk && m_currentNoteId.isEmpty())
        m_currentNoteId = stableDocumentId(m_currentLocalPath);
    if (localOk) {
        ContentItem item;
        item.id = m_currentNoteId;
        item.title = title;
        item.filePath = m_currentLocalPath;
        item.type = m_currentContentType;
        item.sourcePath = m_importSourcePath;
        m_db->upsertContentItem(item);
        if (m_currentContentType == "note")
            m_db->setLinkedDocuments(m_currentNoteId, m_linkedDocumentIds);
    }

    // ---- 2) MySQL（全自动：未连接时静默重试一次）----
    bool dbOk = false;
    if (!m_db->isOpen()) {
        // 用户可能刚在设置页填了参数但还没触发重连，Ctrl+S 时自动尝试一次
        tryReconnectDbSilently();
    }
    if (m_db->isOpen()) {
        Note n;
        n.id = m_currentNoteId;
        n.title = title;
        n.content = content;
        dbOk = m_db->saveNote(n);

        // 写入验证：立刻回查确认数据确实在库里
        if (dbOk) {
            Note verify = m_db->loadNote(m_currentNoteId);
            if (verify.id.isEmpty()) {
                dbOk = false;
                qWarning() << "[MySQL写入验证失败] saveNote返回true但SELECT找不到记录"
                           << "note_id=" << m_currentNoteId;
                // 不弹窗——本地已保存成功，只在状态栏提示
                ui->m_noteStatus->setText("已保存 ✓ 本地（MySQL 写入未生效）");
            }
        }

    }
    if (localOk && m_currentContentType == "document")
        indexCurrentNote();

    // 状态反馈
    m_noteDirty = false;
    updateWindowTitle();
    if (localOk && dbOk)
        ui->m_noteStatus->setText("已保存 ✓ 本地 + MySQL");
    else if (localOk)
        ui->m_noteStatus->setText("已保存 ✓ 仅本地（MySQL 未连接）");
    else
        ui->m_noteStatus->setText("保存失败");

    refreshNoteList();    // 刷新左侧列表
    // 选中刚保存的笔记（优先按本地路径，其次按 DB id）
    for (int i = 0; i < ui->m_noteList->count(); ++i) {
        auto *it = ui->m_noteList->item(i);
        if (!m_currentNoteId.isEmpty() &&
            it->data(Qt::UserRole).toString() == m_currentNoteId) {
            ui->m_noteList->setCurrentRow(i); break;
        } else if (!m_currentLocalPath.isEmpty() &&
                   it->data(Qt::UserRole).toString() == m_currentLocalPath) {
            ui->m_noteList->setCurrentRow(i); break;
        }
    }
}

void MainWindow::indexCurrentNote() {
    if (m_currentContentType != "document") return;
    QString text = ui->m_noteEdit->toPlainText().trimmed();
    if (text.isEmpty() || m_currentNoteId.isEmpty()) return;
    ui->m_noteStatus->setText(
        "正在增量更新文献索引… 首次处理长 PDF 可能需要数分钟，请勿关闭程序");
    QString source = !m_importSourcePath.isEmpty()
        ? m_importSourcePath : m_currentLocalPath;
    m_rag->requestIndex(
        m_currentNoteId, text, ui->m_noteTitle->text().trimmed(), source);
}

void MainWindow::onImportFile() {
    QString filePath = QFileDialog::getOpenFileName(
        this, "导入文档", QString(),
        "支持的文档 (*.pdf *.docx *.txt *.md *.markdown);;PDF 文档 (*.pdf);;Word 文档 (*.docx);;文本/Markdown (*.txt *.md *.markdown)");
    if (filePath.isEmpty()) return;
    if (m_notesBusy) return;
    m_notesBusy = true;
    setNoteButtonsEnabled(false);
    ui->m_noteStatus->setText("导入解析中…");
    m_importSourcePath = filePath;
    m_notes->requestImport(filePath);
}

void MainWindow::onNotesResult(int type, const QString &text, const QString &error) {
    // Import 类型单独处理（不走流式，直接加载到编辑器）
    if (type == NotesClient::Import) {
        m_notesBusy = false;
        setNoteButtonsEnabled(true);
        if (!error.isEmpty()) {
            ui->m_noteStatus->setText("导入失败");
            QMessageBox::warning(this, "导入失败", error);
            return;
        }
        // text 格式："title\ncontent"，按第一个 \n 拆分
        int sep = text.indexOf('\n');
        QString title = (sep > 0) ? text.left(sep) : "导入文献";
        QString content = (sep > 0) ? text.mid(sep + 1) : text;
        // 清空当前编辑状态，加载导入内容
        m_currentNoteId.clear();
        m_currentLocalPath.clear();
        m_ragHistory.clear();
        m_currentContentType = "document";
        m_linkedDocumentIds.clear();
        ui->m_noteTitle->setText(title);
        ui->m_noteEdit->setPlainText(content);
        m_noteDirty = true;
        updateWindowTitle();
        applyContentMode();
        ui->m_noteStatus->setText("文献导入成功 ✓（Ctrl+S 保存并建立索引）");
        return;
    }

    m_notesBusy = false;
    setNoteButtonsEnabled(true);
    if (!error.isEmpty()) {
        if (type == NotesClient::Summary)
            ui->m_summaryView->setPlainText("[出错] " + error);
        else
            ui->m_polishView->setPlainText("[出错] " + error);
        ui->m_noteStatus->setText("失败");
        return;
    }
    // 流式模式：text 为空，使用累积的 m_streamingText
    QString result = text.isEmpty() ? m_streamingText : text;
    if (type == NotesClient::Summary) {
        ui->m_summaryView->setPlainText(result);
        ui->m_noteStatus->setText("摘要完成 ✓");
        saveAiResult("summary", result);
        ui->m_resultTabs->setCurrentWidget(ui->tabSummary);
    } else if (type == NotesClient::Polish) {
        showPolishDiff(m_lastNoteText, result);
        ui->m_noteStatus->setText("学术改写完成 ✓（绿=新增，红删除线=原文）");
        saveAiResult("polish", result);
        ui->m_resultTabs->setCurrentWidget(ui->tabPolish);
    }
}

void MainWindow::showPolishDiff(const QString &orig, const QString &polished) {
    QString html = "<p style='line-height:1.6'>";
    html += diffHtml(orig, polished);
    html += "</p>";
    ui->m_polishView->setHtml(html);
}

// ---------------- 笔记列表 / 管理（合并本地 .md + MySQL）----------------
void MainWindow::refreshNoteList() {
    ui->m_noteList->blockSignals(true);
    ui->m_noteList->clear();

    QSet<QString> databaseTitles;

    // Database records are authoritative and always carry the stable UUID.
    if (m_db->isOpen()) {
        QList<Note> dbNotes = m_db->loadAllNotes();
        for (const Note &n : dbNotes) {
            const QString title = n.title.isEmpty() ? "未命名笔记" : n.title;
            ContentItem meta = m_db->loadContentItem(n.id);
            if (meta.type.isEmpty() && n.content.contains("[[PAGE:")) {
                meta.id = n.id;
                meta.title = title;
                meta.type = "document";
                m_db->upsertContentItem(meta);
            }
            QString kind = meta.type == "document" ? "文献" : "笔记";
            QListWidgetItem *it = new QListWidgetItem(
                QString("[%1] %2\n%3").arg(kind, title, n.updatedAt));
            it->setData(Qt::UserRole, n.id);       // 存储 DB id
            it->setToolTip(title);
            it->setData(Qt::UserRole + 1, "db");   // 标记为 DB 来源
            it->setData(Qt::UserRole + 2, title);
            ui->m_noteList->addItem(it);
            databaseTitles.insert(title);
        }
    }

    // Only show local-only drafts. A matching database note is already listed above.
    QList<LocalNote> localNotes = scanLocalNotes();
    for (const LocalNote &ln : localNotes) {
        if (databaseTitles.contains(ln.title)) continue;
        const QString contentId = stableDocumentId(ln.filePath);
        ContentItem meta = m_db->loadContentItem(contentId);
        if (meta.type.isEmpty()) {
            QFile file(ln.filePath);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString content = QString::fromUtf8(file.readAll());
                if (content.contains("[[PAGE:")) {
                    meta.id = contentId;
                    meta.title = ln.title;
                    meta.filePath = ln.filePath;
                    meta.type = "document";
                    m_db->upsertContentItem(meta);
                }
            }
        }
        QString kind = meta.type == "document" ? "文献" : "笔记";
        QListWidgetItem *it = new QListWidgetItem(
            QString("[%1] %2\n%3").arg(kind, ln.title, ln.mtime));
        it->setData(Qt::UserRole, ln.filePath);
        it->setToolTip(ln.filePath);
        it->setData(Qt::UserRole + 1, "local");
        it->setData(Qt::UserRole + 2, ln.title);
        ui->m_noteList->addItem(it);
    }
    ui->m_noteList->blockSignals(false);
}

static QString localNotePath(const QString &notesDir, const QString &title) {
    QString safe = title.isEmpty() ? "未命名笔记" : title;
    safe.remove(QRegularExpression(R"([\\/:*?"<>|])"));
    if (safe.isEmpty()) safe = "未命名笔记";
    return notesDir + "/" + safe + ".md";
}

void MainWindow::loadNoteIntoEditor(const QString &id) {
    Note n = m_db->loadNote(id);
    if (n.id.isEmpty()) return;
    m_currentNoteId = n.id;
    m_currentLocalPath.clear();
    m_importSourcePath.clear();
    ContentItem item = m_db->loadContentItem(id);
    m_currentContentType = item.type.isEmpty() ? "note" : item.type;
    if (item.type.isEmpty() && n.content.contains("[[PAGE:"))
        m_currentContentType = "document";
    m_importSourcePath = item.sourcePath;
    m_linkedDocumentIds = m_db->loadLinkedDocuments(id);
    m_ragHistory.clear();
    ui->m_noteTitle->setText(n.title);
    ui->m_noteEdit->setPlainText(n.content);
    ui->m_qaInput->clear();
    ui->m_noteStatus->setText(
        (m_currentContentType == "document" ? "已载入文献：" : "已载入笔记：")
        + (n.title.isEmpty() ? "未命名笔记" : n.title));
    applyContentMode();
}

void MainWindow::onNoteSelected(QListWidgetItem *current, QListWidgetItem *) {
    if (!current) return;
    QString id = current->data(Qt::UserRole).toString();
    QString source = current->data(Qt::UserRole + 1).toString();
    if (source == "local" && !id.isEmpty()) {
        loadLocalNoteIntoEditor(id);
    } else if (!id.isEmpty()) {
        // 尝试从 DB 载入；如果 DB 没有对应记录（已被删），尝试找本地同名文件
        Note n = m_db->loadNote(id);
        if (!n.id.isEmpty()) {
            m_currentNoteId = n.id;
            m_currentLocalPath.clear();
            m_importSourcePath.clear();
            ContentItem item = m_db->loadContentItem(n.id);
            m_currentContentType = item.type.isEmpty() ? "note" : item.type;
            if (item.type.isEmpty() && n.content.contains("[[PAGE:"))
                m_currentContentType = "document";
            m_importSourcePath = item.sourcePath;
            m_linkedDocumentIds = m_db->loadLinkedDocuments(n.id);
            m_ragHistory.clear();
            ui->m_noteTitle->setText(n.title);
            ui->m_noteEdit->setPlainText(n.content);
            ui->m_qaInput->clear();
            m_noteDirty = false;
            updateWindowTitle();
            ui->m_noteStatus->setText(
                (m_currentContentType == "document" ? "已载入文献：" : "已载入笔记：")
                + (n.title.isEmpty() ? "未命名笔记" : n.title));
            applyContentMode();
        }
        populateHistories();   // 切换笔记后刷新该笔记的 AI 历史
    }
}

void MainWindow::onDeleteNote() {
    QListWidgetItem *cur = ui->m_noteList->currentItem();
    if (!cur) {
        QMessageBox::information(this, "提示", "请先在左侧列表中选择要删除的内容。");
        return;
    }
    QString id = cur->data(Qt::UserRole).toString();
    QString source = cur->data(Qt::UserRole + 1).toString();
    if (id.isEmpty()) return;
    QString title = cur->data(Qt::UserRole + 2).toString();
    if (title.isEmpty()) title = cur->text().split("\n").first();
    const QString contentId = source == "local" ? stableDocumentId(id) : id;
    ContentItem deletedItem = m_db->loadContentItem(contentId);
    const bool isDocument = deletedItem.type == "document"
        || cur->text().startsWith("[文献]");
    const QString kind = isDocument ? "文献" : "笔记";
    if (QMessageBox::question(this, "删除" + kind,
            QString("确定删除%1「%2」？此操作不可撤销。").arg(kind, title),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    bool deleted = false;
    // 删除本地 .md 文件（如果有）
    if (source == "local") {
        deleted = deleteLocalNote(id);
        if (!deleted)
            QMessageBox::warning(this, "删除失败", "无法删除本地文件：" + id);
    }

    // 删除 MySQL 记录（如果连接了）
    if (source == "db" && m_db->isOpen()) {
        deleted = m_db->deleteNote(id);
        if (deleted)
            deleteLocalNote(localNotePath(m_notesDir, title));
        if (!deleted)
            QMessageBox::warning(this, "删除失败", m_db->lastError());
    } else if (source == "db") {
        // DB 没连接，只删本地可能存在的同名文件
        deleteLocalNote(noteFilePath(title));
        deleted = true;
    }

    if (deleted) {
        if (isDocument)
            m_rag->requestDelete(contentId);
        m_db->deleteContentItem(contentId);
        if ((source == "local" && id == m_currentLocalPath) ||
            (source == "db" && id == m_currentNoteId))
            newNote();
        refreshNoteList();
        ui->m_noteStatus->setText("已删除" + kind + "：" + title);
    }
}

// ---------------- 本地文件存储（.md，不依赖 MySQL）----------------

QString MainWindow::noteFilePath(const QString &title) {
    // 如果已有本地路径（编辑已有笔记），复用它
    if (!m_currentLocalPath.isEmpty())
        return m_currentLocalPath;
    return localNotePath(m_notesDir, title);
}

bool MainWindow::saveNoteToLocal() {
    QString title = ui->m_noteTitle->text().trimmed();
    QString content = ui->m_noteEdit->toPlainText();
    QString path = noteFilePath(title);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(content.toUtf8());
    f.close();
    m_currentLocalPath = path;
    return true;
}

QList<LocalNote> MainWindow::scanLocalNotes() {
    QList<LocalNote> list;
    QDir dir(m_notesDir);
    auto entries = dir.entryInfoList(
        QStringList() << "*.md", QDir::Files, QDir::Time | QDir::Reversed);
    for (const QFileInfo &fi : entries) {
        LocalNote ln;
        ln.filePath = fi.absoluteFilePath();
        ln.title = fi.completeBaseName();   // 文件名去掉 .md
        ln.mtime = fi.lastModified().toString("yyyy-MM-dd hh:mm");
        list.append(ln);
    }
    return list;
}

void MainWindow::autoSaveDraft() {
    // AI 操作前静默保存当前内容到本地草稿，防止丢失
    // 仅在用户已输入内容时才存
    if (ui->m_noteEdit->toPlainText().trimmed().isEmpty()) return;
    if (saveNoteToLocal() && m_currentNoteId.isEmpty())
        m_currentNoteId = stableDocumentId(m_currentLocalPath);
}

void MainWindow::updateWindowTitle() {
    QString base = "智能笔记软件";
    if (m_noteDirty) base = "* " + base;  // 标准未保存标记
    setWindowTitle(base);
}

bool MainWindow::deleteLocalNote(const QString &filePath) {
    if (filePath.isEmpty()) return false;
    QFile f(filePath);
    return f.remove();
}

void MainWindow::loadLocalNoteIntoEditor(const QString &filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    m_currentLocalPath = filePath;
    m_importSourcePath.clear();
    // 本地笔记用文件路径作为历史记录关联标识（Ctrl+S 同步 MySQL 后换为 UUID）
    m_currentNoteId = stableDocumentId(filePath);
    ContentItem item = m_db->loadContentItem(m_currentNoteId);
    m_currentContentType = item.type.isEmpty() ? "note" : item.type;
    if (item.type.isEmpty() && content.contains("[[PAGE:"))
        m_currentContentType = "document";
    m_importSourcePath = item.sourcePath;
    m_linkedDocumentIds = m_db->loadLinkedDocuments(m_currentNoteId);
    m_ragHistory.clear();
    ui->m_noteTitle->setText(QFileInfo(filePath).completeBaseName());
    ui->m_noteEdit->setPlainText(content);
    ui->m_qaInput->clear();
    m_noteDirty = false;
    updateWindowTitle();
    ui->m_noteStatus->setText(
        (m_currentContentType == "document" ? "已载入本地文献：" : "已载入本地笔记：")
        + QFileInfo(filePath).completeBaseName());
    applyContentMode();
    populateHistories();   // 切换到本地笔记后刷新历史
}
void MainWindow::populateHistories() {
    // 用当前笔记 ID 筛选：每个笔记只看自己的 AI 历史记录
    QString nid = m_currentNoteId;
    if (nid.isEmpty()) {
        ui->m_summaryHistory->clear();
        ui->m_polishHistory->clear();
        ui->m_ragHistory->clear();
        return;
    }
    auto fill = [](QListWidget *w, const QList<AiResult> &list) {
        w->clear();
        for (const AiResult &r : list) {
            QListWidgetItem *it = new QListWidgetItem(r.createdAt);
            it->setData(Qt::UserRole, r.content);
            it->setToolTip("点击回看该次结果");
            w->addItem(it);
        }
    };
    fill(ui->m_summaryHistory, m_db->loadResults("summary", nid));
    fill(ui->m_polishHistory, m_db->loadResults("polish",   nid));
    fill(ui->m_ragHistory,    m_db->loadResults("rag",      nid));
}

void MainWindow::saveAiResult(const QString &type, const QString &content) {
    QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    // 1) 持久化到本地 SQLite（不依赖 MySQL）
    if (m_db->isLocalOpen()) {
        AiResult r;
        r.noteId = m_currentNoteId;
        r.type = type;
        r.content = content;
        r.createdAt = stamp;
        m_db->saveResult(r);
    }
    // 2) 追加到对应标签页的历史列表（最新在最上方）
    QListWidget *hist = nullptr;
    if (type == "summary") hist = ui->m_summaryHistory;
    else if (type == "polish") hist = ui->m_polishHistory;
    else if (type == "rag") hist = ui->m_ragHistory;
    if (hist) {
        QListWidgetItem *it = new QListWidgetItem(stamp);
        it->setData(Qt::UserRole, content);
        it->setToolTip("点击回看该次结果");
        hist->insertItem(0, it);
    }
}

void MainWindow::onSummaryHistoryClicked(QListWidgetItem *item) {
    if (item) ui->m_summaryView->setPlainText(item->data(Qt::UserRole).toString());
}

void MainWindow::onPolishHistoryClicked(QListWidgetItem *item) {
    if (item) ui->m_polishView->setPlainText(item->data(Qt::UserRole).toString());
}

void MainWindow::onRagHistoryClicked(QListWidgetItem *item) {
    if (item) ui->m_ragView->setPlainText(item->data(Qt::UserRole).toString());
}

// ---------------- 笔记页 · RAG ----------------
// 架构：C++ 负责切块触发/检索结果组装/Prompt 拼装/LLM 生成；
//       「本地 Jina 向量化 + Milvus Lite 混合检索」由 Python 完成。
void MainWindow::onIndexNote() {
    if (m_currentContentType != "document") {
        QMessageBox::information(this, "提示", "只有导入的文献需要建立检索索引。");
        return;
    }
    QString text = ui->m_noteEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        QMessageBox::information(this, "提示", "当前文献没有可建立索引的文本内容。");
        return;
    }
    if (m_currentNoteId.isEmpty())
        m_currentNoteId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    indexCurrentNote();
}

void MainWindow::onSelectLinkedDocuments() {
    if (m_currentContentType == "document") return;
    if (m_currentNoteId.isEmpty())
        autoSaveDraft();
    QList<ContentItem> documents = m_db->loadDocuments();
    if (documents.isEmpty()) {
        QMessageBox::information(this, "关联文献",
            "当前没有已保存的文献，请先导入文献并保存。");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("选择关联文献");
    dialog.resize(520, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel("RAG 问答只会检索勾选的文献：", &dialog);
    auto *list = new QListWidget(&dialog);
    for (const ContentItem &document : documents) {
        auto *item = new QListWidgetItem(document.title, list);
        item->setData(Qt::UserRole, document.id);
        item->setToolTip(document.sourcePath);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(
            m_linkedDocumentIds.contains(document.id) ? Qt::Checked : Qt::Unchecked);
    }
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(label);
    layout->addWidget(list);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    m_linkedDocumentIds.clear();
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->checkState() == Qt::Checked)
            m_linkedDocumentIds.append(item->data(Qt::UserRole).toString());
    }
    if (!m_currentNoteId.isEmpty())
        m_db->setLinkedDocuments(m_currentNoteId, m_linkedDocumentIds);
    m_noteDirty = true;
    updateWindowTitle();
    applyContentMode();
}

void MainWindow::onRagQA() {
    QString q = ui->m_qaInput->text().trimmed();
    if (q.isEmpty()) {
        QMessageBox::information(this, "提示", "请在右上角输入框输入你的问题。");
        return;
    }
    QStringList documentIds;
    if (!m_globalRag->isChecked()) {
        if (m_currentContentType == "document") {
            if (m_currentNoteId.isEmpty()) {
                QMessageBox::information(this, "提示", "请先保存文献并建立索引。");
                return;
            }
            documentIds << m_currentNoteId;
        } else {
            documentIds = m_linkedDocumentIds;
            if (documentIds.isEmpty()) {
                QMessageBox::information(this, "提示",
                    "当前笔记尚未关联文献，请点击“关联文献”进行选择。");
                return;
            }
        }
    }
    m_ragQuestion = q;
    if (m_currentContentType == "note") autoSaveDraft();
    if (m_ragBusy) return;
    m_ragBusy = true;
    setNoteButtonsEnabled(false);
    ui->m_noteStatus->setText("RAG 检索中…");
    ui->m_ragView->clear();
    m_streamingText.clear();
    m_ragSourcesText.clear();
    m_rag->requestRetrieve(documentIds, q, m_ragHistory, 10);
}

void MainWindow::onRagResult(int type, const QString &text, const QString &error) {
    if (type == RagClient::Index) {
        // 建立索引完成（也可能是保存时自动触发的）
        if (!error.isEmpty()) {
            ui->m_noteStatus->setText("索引失败");
            QMessageBox::warning(this, "索引失败", error);
            return;
        }
        int n = text.toInt();
        ui->m_noteStatus->setText(QString("向量索引已建立 ✓ 共 %1 个片段").arg(n));
        return;
    }

    // Retrieve 结果
    if (!error.isEmpty()) {
        m_ragBusy = false;
        setNoteButtonsEnabled(true);
        ui->m_ragView->setPlainText("[检索出错] " + error);
        ui->m_noteStatus->setText("检索失败");
        return;
    }
    // 解析 {"chunks":["...","..."]}
    QJsonObject obj = QJsonDocument::fromJson(text.toUtf8()).object();
    QJsonArray arr = obj.value("chunks").toArray();
    if (arr.isEmpty()) {
        m_ragBusy = false;
        setNoteButtonsEnabled(true);
        if (m_globalRag->isChecked()) {
            ui->m_ragView->setPlainText(
                "全部文献中未检索到相关内容。请确认文献已保存并完成索引，或换一种问法。");
        } else if (m_currentContentType == "document") {
            ui->m_ragView->setPlainText(
                "当前文献未检索到相关内容。请确认索引已完成，或换一种更具体的问法。");
        } else {
            ui->m_ragView->setPlainText(
                "关联文献中未检索到相关内容。请检查关联范围和文献索引，或换一种问法。");
        }
        ui->m_noteStatus->setText("未检索到相关内容");
        return;
    }
    QJsonArray sources = obj.value("sources").toArray();
    QStringList chunks;
    QStringList sourceLines;
    for (int i = 0; i < arr.size(); ++i) {
        QString chunk = arr.at(i).toString();
        QJsonObject source = i < sources.size()
            ? sources.at(i).toObject() : QJsonObject();
        QString title = source.value("title").toString("未命名文献");
        int page = source.value("page").toInt();
        QString label = QString("[%1] %2").arg(i + 1).arg(title);
        if (page > 0)
            label += QString("，第 %1 页").arg(page);
        chunks.append(label + "\n" + chunk);

        QString sourceLine = label;
        QString path = source.value("source").toString();
        if (!path.isEmpty())
            sourceLine += "\n    " + path;
        sourceLines.append(sourceLine);
    }
    QString context = chunks.join("\n\n----\n\n");
    if (!sourceLines.isEmpty())
        m_ragSourcesText = "\n\n【参考来源】\n" + sourceLines.join("\n");

    // 组装增强 Prompt：基于检索片段 + 对话历史回答
    QString sys = "你是严谨的文献问答助手。请综合【参考片段】和【之前的对话上下文】回答用户问题。"
                  "若用户追问，优先结合之前的对话上下文理解问题意图。"
                  "回答中的关键事实必须使用对应片段编号标注来源，例如[1][2]。"
                  "若参考片段和对话历史中都未提及，请说明「文献中未提及该内容」，不要凭空编造。";
    // 追问时加提示，帮助 LLM 理解上下文
    QString user;
    if (!m_ragHistory.isEmpty()) {
        user = "【追问】请结合之前的对话上下文理解本次问题。\n"
               "【参考片段】\n" + context + "\n\n【问题】\n" + m_ragQuestion;
    } else {
        user = "【参考片段】\n" + context + "\n\n【问题】\n" + m_ragQuestion;
    }

    QList<ChatMessage> msgs;
    ChatMessage sm; sm.role = "system"; sm.content = sys; msgs << sm;
    // 多轮：加入之前的问答历史
    msgs << m_ragHistory;
    ChatMessage um; um.role = "user";   um.content = user; msgs << um;
    qDebug() << "[RAG] 发送" << msgs.size() << "条消息（含"
             << m_ragHistory.size() << "条历史）到 LLM";
    m_streamingText.clear();
    m_chat->sendStream(msgs);   // 流式调 LLM 生成（m_ragBusy 在 onRagGenerated 中复位）
}

void MainWindow::onRagChunk(const QString &chunk) {
    m_streamingText += chunk;
    ui->m_ragView->setPlainText(m_streamingText);
}

void MainWindow::onRagGenerated(const QString &reply, const QString &error) {
    m_ragBusy = false;
    setNoteButtonsEnabled(true);
    if (!error.isEmpty()) {
        ui->m_ragView->setPlainText("[生成出错] " + error);
        ui->m_noteStatus->setText("生成失败");
        return;
    }
    // 流式模式：reply 为空，使用累积的 m_streamingText
    QString answer = reply.isEmpty() ? m_streamingText : reply;
    answer += m_ragSourcesText;
    ui->m_ragView->setPlainText(answer);
    ui->m_noteStatus->setText("文献问答完成 ✓（已附来源引用）");
    saveAiResult("rag", answer);
    ui->m_resultTabs->setCurrentWidget(ui->tabRag);
    // 多轮：将本轮 Q&A 追加到历史，保留最近 10 轮
    ChatMessage q; q.role = "user";      q.content = m_ragQuestion;
    ChatMessage a; a.role = "assistant"; a.content = answer;
    m_ragHistory << q << a;
    while (m_ragHistory.size() > 20)  // 10 轮 = 20 条消息
        m_ragHistory.removeFirst();
}

// ---------------- 图片分析（保留） ----------------
void MainWindow::onImgSelect() {
    QString f = QFileDialog::getOpenFileName(this, "选择图片", QString(),
        "图片 (*.png *.jpg *.jpeg *.gif *.bmp *.webp)");
    if (f.isEmpty()) return;
    QFile file(f);
    if (!file.open(QIODevice::ReadOnly)) return;
    m_imgPart.base64 = file.readAll().toBase64();
    m_imgPart.mime = mimeFromSuffix(f);
    QPixmap pm(f);
    if (!pm.isNull())
        m_imageUi->imgPreview->setPixmap(pm.scaled(QSize(360, 240), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        m_imageUi->imgPreview->setText("（图片加载失败）");
}

void MainWindow::onImgAnalyze() {
    if (m_imgPart.base64.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择一张图片。");
        return;
    }
    if (m_s.visionApiKey.isEmpty()) {
        QMessageBox::information(this, "提示",
            "请先在「设置」页填写视觉 API Key（通义千问）。\n"
            "图片分析使用设置页中的视觉模型配置。");
        return;
    }
    // 实时应用视觉设置（Key / BaseURL / 模型），以本页下拉框为准并持久化
    m_s.visionModel = m_imageUi->m_imgModel->currentText().trimmed();
    m_imgClient->setApiKey(m_s.visionApiKey);
    m_imgClient->setBaseUrl(m_s.visionBaseUrl);
    m_imgClient->setModel(m_s.visionModel);
    saveSettings();
    m_settingsUi->m_visionApiKey->setText(m_s.visionApiKey);  // 回写设置页
    m_settingsUi->m_visionModel->setText(m_s.visionModel);

    if (m_imgBusy) return;
    m_imgBusy = true;
    m_imageUi->btnImgAnalyze->setEnabled(false);
    m_imgUserContent = m_imgPrompt->text().trimmed();
    if (m_imgUserContent.isEmpty())
        m_imgUserContent =
            "请作为文献图表分析助手，识别图表类型、标题、坐标轴、图例、关键数值与趋势，"
            "解释图表支持的结论及可能的限制。无法确认的信息请明确说明，不要猜测。";
    ChatMessage um;
    um.role = "user";
    um.content = m_imgUserContent;
    um.images << m_imgPart;
    QList<ChatMessage> req;
    req << um;
    m_imageUi->imgResult->setPlainText("（分析中…）");
    m_imgClient->send(req);
}

void MainWindow::onImgReply(const QString &reply, const QString &error) {
    m_imgBusy = false;
    m_imageUi->btnImgAnalyze->setEnabled(true);
    if (!error.isEmpty()) {
        m_imageUi->imgResult->setPlainText("[出错] " + error);
        return;
    }
    m_imageUi->imgResult->setPlainText(reply);
    QString conv = "img-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_db->saveMessage(conv, "user", m_imgUserContent, m_s.visionModel);
    m_db->saveMessage(conv, "assistant", reply, m_s.visionModel);
}

// ---------------- 设置 ----------------
void MainWindow::onProviderChanged(const QString &p) {
    if (p == "通义千问") {
        m_settingsUi->m_baseUrl->setText("https://dashscope.aliyuncs.com/compatible-mode/v1");
        QString m = m_settingsUi->m_model->text().trimmed();
        if (m.isEmpty() || m.contains("deepseek", Qt::CaseInsensitive))
            m_settingsUi->m_model->setText("qwen-plus");   // 文本模型（笔记用）
    } else {
        m_settingsUi->m_baseUrl->setText("https://api.deepseek.com/v1");
        QString m = m_settingsUi->m_model->text().trimmed();
        if (m.isEmpty() || m.contains("qwen", Qt::CaseInsensitive))
            m_settingsUi->m_model->setText("deepseek-chat");
    }
}

void MainWindow::onSaveSettings() {
    m_s.apiKey = m_settingsUi->m_apiKey->text().trimmed();
    m_s.baseUrl = m_settingsUi->m_baseUrl->text().trimmed();
    m_s.model = m_settingsUi->m_model->text().trimmed();
    m_s.backend = m_settingsUi->m_backend->text().trimmed();
    // 视觉模型三项（图片分析用，与文本模型独立）
    m_s.visionApiKey = m_settingsUi->m_visionApiKey->text().trimmed();
    m_s.visionBaseUrl = m_settingsUi->m_visionBaseUrl->text().trimmed();
    m_s.visionModel = m_settingsUi->m_visionModel->text().trimmed();
    m_s.odbcDriver = m_settingsUi->m_odbc->currentText().trimmed();
    m_s.dbHost = m_settingsUi->m_dbHost->text().trimmed();
    m_s.dbPort = m_settingsUi->m_dbPort->text().toInt();
    m_s.dbName = m_settingsUi->m_dbName->text().trimmed();
    m_s.dbUser = m_settingsUi->m_dbUser->text().trimmed();
    m_s.dbPassword = m_settingsUi->m_dbPwd->text();

    saveSettings();
    applySettingsToClients();
    updateStatusModel();
    if (m_imageUi->m_imgModel->findText(m_s.visionModel) == -1 && !m_s.visionModel.isEmpty())
        m_imageUi->m_imgModel->addItem(m_s.visionModel);
    m_imageUi->m_imgModel->setCurrentText(m_s.visionModel);
    if (!m_db->open(m_s.odbcDriver, m_s.dbHost, m_s.dbPort,
                    m_s.dbName, m_s.dbUser, m_s.dbPassword)) {
        QMessageBox::warning(this, "数据库",
            "无法连接 MySQL：\n" + m_db->lastError());
    } else {
        m_db->ensureNotesTable();
        m_db->ensureChatMessagesTable();
        refreshNoteList();
    }
    // MySQL 连接状态不再显示在状态栏（全自动静默处理）
}

void MainWindow::onNavChanged(int row) {
    ui->stack->setCurrentIndex(row);
}

void MainWindow::onMemTimer() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        ui->statusMem->setText(QString("内存: %1 MB")
            .arg(pmc.WorkingSetSize / (1024.0 * 1024.0), 0, 'f', 1));
}

void MainWindow::onClearEdit() {
    if (m_currentContentType == "document") {
        QMessageBox::information(this, "文献只读",
            "文献原文不能清空或改写；已仅清空右侧 AI 结果。");
    } else {
        ui->m_noteEdit->clear();
        m_noteDirty = true;
        updateWindowTitle();
    }
    ui->m_summaryView->clear();
    ui->m_polishView->clear();
    ui->m_ragView->clear();
}

void MainWindow::onAbout() {
    QMessageBox::about(this, "关于 文献智能笔记",
        "Qt5 + FastAPI 文献智能笔记\n\n"
        "架构：Qt 客户端（C++ 主导）→ 本地 FastAPI 后端 → 云端大模型\n"
        "  • 文献：导入后只读，可生成摘要、建立索引并进行带引用问答\n"
        "  • 笔记：自由写作，可进行摘要和学术改写，不进入向量库\n"
        "  • 笔记可指定关联文献；“全部文献”仅在主动勾选时启用\n"
        "  • 本地 Jina Embedding + Milvus Lite + BM25 混合检索\n"
        "  • 文献图表解析支持自定义提示词\n"
        "  • 文献与笔记全文搜索（Ctrl+F 搜索框）\n"
        "  • Markdown 预览模式（Ctrl+P 切换）\n"
        "快捷键：Ctrl+1 摘要，Ctrl+2 学术改写，Ctrl+4 文献问答，Ctrl+P 预览。");
}

// ---------------- 笔记搜索 ----------------
void MainWindow::onSearchChanged(const QString &keyword) {
    QString kw = keyword.trimmed().toLower();
    for (int i = 0; i < ui->m_noteList->count(); ++i) {
        QListWidgetItem *it = ui->m_noteList->item(i);
        if (kw.isEmpty()) {
            it->setHidden(false);
            continue;
        }
        // 搜索标题（列表项文本的第一行）
        QString title = it->text().split("\n").first().toLower();
        bool match = title.contains(kw);
        // 如果是 DB 笔记，也搜索内容
        if (!match && it->data(Qt::UserRole + 1).toString() == "db") {
            QString id = it->data(Qt::UserRole).toString();
            Note n = m_db->loadNote(id);
            if (n.content.toLower().contains(kw))
                match = true;
        }
        it->setHidden(!match);
    }
}

// ---------------- Markdown 预览模式 ----------------
void MainWindow::togglePreviewMode() {
    m_previewMode = !m_previewMode;
    if (m_previewMode) {
        // 切到预览：暂存编辑内容，渲染 Markdown 显示
        m_savedEditText = ui->m_noteEdit->toPlainText();
        QString html = renderMarkdown(m_savedEditText);
        ui->m_noteEdit->setReadOnly(true);
        ui->m_noteEdit->setHtml(html);
    } else {
        // 切回编辑：恢复纯文本
        ui->m_noteEdit->setPlainText(m_savedEditText);
        applyContentMode();
    }
}

QString MainWindow::renderMarkdown(const QString &md) {
    // 轻量 Markdown → HTML 渲染（不引入外部依赖）
    QString html = md.toHtmlEscaped();
    // 标题：# ## ###
    html.replace(QRegularExpression("^### (.+)$", QRegularExpression::MultilineOption),
                 "<h3>\\1</h3>");
    html.replace(QRegularExpression("^## (.+)$", QRegularExpression::MultilineOption),
                 "<h2>\\1</h2>");
    html.replace(QRegularExpression("^# (.+)$", QRegularExpression::MultilineOption),
                 "<h1>\\1</h1>");
    // 粗体 **text**
    html.replace(QRegularExpression("\\*\\*(.+?)\\*\\*"), "<b>\\1</b>");
    // 斜体 *text*
    html.replace(QRegularExpression("\\*(.+?)\\*"), "<i>\\1</i>");
    // 行内代码 `code`
    html.replace(QRegularExpression("`(.+?)`"), "<code>\\1</code>");
    // 无序列表 - item
    html.replace(QRegularExpression("^- (.+)$", QRegularExpression::MultilineOption),
                 "&nbsp;&nbsp;• \\1");
    // 换行
    html.replace("\n", "<br>");
    return "<div style='font-family:Microsoft YaHei;font-size:14px;line-height:1.6'>" + html + "</div>";
}
