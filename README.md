# AI Chat Desktop · 智能笔记软件

一个**本地优先**的桌面笔记应用，内置 AI 能力：智能摘要、语法润色、**基于笔记的 RAG 问答**、以及**图片分析**。

- 前端：Qt 5（C++17）桌面客户端，`ai-chat-desktop/`
- 后端：FastAPI（Python）本地服务，代理云端大模型并负责向量检索，`notes-server/`

> 笔记内容默认以 `.md` 文件保存在本地，**无需联网即可使用**；AI 功能需要启动本地后端并配置模型 API Key。

---

## 功能特性

- **智能摘要 / 语法润色**：调用 DeepSeek 等 OpenAI 兼容接口，支持**流式输出**，润色结果带「绿=新增 / 红删除线=原文」差异对照。
- **笔记管理**：新建 / 编辑 / 删除 / 搜索；支持**本地 `.md` 文件存储**与 **MySQL** 双通道（无 MySQL 时自动降级为仅本地保存）。
- **RAG 问答**：为笔记建立向量索引（切块 + DashScope 向量化 + Chroma 向量库），检索相关片段后由大模型生成回答；支持「当前笔记内检索」与「跨笔记全局检索」。
- **图片分析**：多模态视觉模型（默认通义千问 `qwen3-vl-flash`）分析本地图片，支持自定义提示词。
- **文档导入**：上传 PDF / Word（`.docx`），后端提取纯文本写入笔记（不支持扫描版 PDF）。
- **Markdown 编辑 / 预览**：`Ctrl+P` 一键切换。
- **AI 结果历史**：摘要 / 润色 / RAG 结果持久化到本地 SQLite，跨会话保留，可点击回看。
- **集中设置页**：配置 API Key、BaseURL、模型、后端地址与数据库连接（设置页填写的 Key 优先级高于后端 `.env`）。

**职责边界**：后端（Python）负责「切块 + 向量化 + 向量库检索 + 代理 LLM 流式调用」；前端（C++）负责「UI、Prompt 组装、最终生成调用、本地存储」。

---

## 技术栈

| 层 | 技术 |
| --- | --- |
| 桌面客户端 | Qt 5.12.11 · C++17 · MinGW 64-bit · Qt Widgets · `QNetworkAccessManager` · `QODBC` · SQLite |
| 后端 | Python · FastAPI · uvicorn · httpx · python-dotenv |
| 向量库 | Chroma（持久化） |
| 文本模型 | DeepSeek（`deepseek-chat`，可在设置中替换任意 OpenAI 兼容端点） |
| 视觉模型 | 通义千问 DashScope（`qwen3-vl-flash`） |
| 向量化 | DashScope `text-embedding-v3`（dim=1024） |
| 文档解析 | pdfplumber（PDF）· python-docx（Word） |
| 笔记存储 | 本地 `.md` 文件（必需）+ MySQL 8.0（可选） |

---

## 环境要求

- **Qt 5.12.11（MinGW 64-bit）** + Qt Creator（构建客户端）
- **Python 3.10+** 与 `pip`（运行后端）
- **MySQL Server 8.0 + MySQL ODBC 8.0 驱动**（*可选*，仅在使用 MySQL 存储笔记时需要；缺失时自动降级为仅本地 `.md` 存储）
- **OpenSSL 1.1 x64** 运行时（已捆绑在 `ai-chat-desktop/`，无需另行安装）

---

## 后端 API 参考

基础地址：`http://127.0.0.1:8000`

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/api/summary` | 智能摘要（非流式），body：`text, provider, detail(short/medium/detailed), api_key?, base_url?, model?` |
| `POST` | `/api/polish` | 语法润色（非流式），body 同上 |
| `POST` | `/api/summary/stream` | 流式摘要（`text/plain` 逐块返回） |
| `POST` | `/api/polish/stream` | 流式润色 |
| `POST` | `/api/rag/index` | 为笔记建向量索引，body：`note_id, text, api_key?` |
| `POST` | `/api/rag/retrieve` | 检索 Top-K 片段，body：`note_id?(空=全局), query, top_k, api_key?` |
| `POST` | `/api/rag/delete` | 删除某笔记索引，body：`note_id` |
| `POST` | `/api/import` | 上传 PDF/Word 提取纯文本（`multipart/form-data`，字段 `file`） |
| `GET` | `/health` | 健康检查 |

所有 LLM 调用走 OpenAI 兼容协议；`CORS` 已放开以允许本地 Qt 客户端访问。

---

## 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+1` | 智能摘要 |
| `Ctrl+2` | 语法润色 |
| `Ctrl+4` | RAG 问答 |
| `Ctrl+S` | 保存当前笔记 |
| `Ctrl+P` | Markdown 预览 / 编辑切换 |

---

## 说明与限制

- **本地优先**：笔记以 `.md` 存于本地目录，断网也能查看与编辑；MySQL 仅为可选的同步/集中存储层。
- **RAG 边界**：后端只负责检索（切块+向量化+向量库），最终「组装 Prompt + 调 LLM 生成」在客户端完成。
- **文档导入**：支持**文字版** PDF 与 `.docx`，扫描版 PDF（图片）需 OCR，暂不支持。
- **向量库大小**：单次索引文本总长上限约 20 万字符（防止海量 embedding 调用）。
- 本项目为个人/学习用途的桌面工具，未内置用户系统与远程同步。

