# 文献智能笔记

面向科研、法律、咨询及其他需要高频阅读长文档的用户，本项目将内容明确分为“文献”和“笔记”，提供本地优先的文献管理、结构化摘要、学术写作辅助、带引用问答和图表解析能力。

- 桌面端：Qt 5 / C++17
- 本地服务：FastAPI / Python
- 检索：本地 Jina Embedding、Milvus Lite、BM25、WeightedRanker
- 模型：DeepSeek、通义千问及其他 OpenAI 兼容服务

## 核心能力

### 文献与笔记双工作区

- **文献**：由 PDF、Word、TXT 或 Markdown 导入，正文只读，可生成摘要、建立 Milvus 索引并进行当前文献问答。
- **笔记**：用于阅读记录和写作草稿，可编辑、生成摘要和学术改写，但不会进入向量库，避免草稿污染文献检索。
- 笔记可显式关联一篇或多篇文献，RAG 默认只检索这些文献。
- “全部文献”是主动勾选的独立范围，不会因笔记开启问答而自动混合整个文献库。

### 文献导入与阅读

- 导入 PDF、Word、TXT 和 Markdown。
- PDF 解析时保留页码标记，用于检索结果的来源定位。
- 文献和笔记正文以本地 Markdown 保存，MySQL 作为可选同步层。
- SQLite 持久化内容类型、笔记与文献的关联关系，以及各类 AI 结果历史。

### 结构化摘要

摘要围绕以下维度生成：

- 研究背景与问题
- 方法与数据
- 核心发现
- 结论与价值
- 局限性
- 关键词

模型被要求保留关键数字与术语，并明确标记原文未说明的信息。

### 带引用的 RAG 问答

1. 基于句向量相似度识别主题边界并进行语义分块。
2. 语义分块异常时自动回退为带重叠窗口的规则分块。
3. 使用本地 `jinaai/jina-embeddings-v5-text-nano` 生成 768 维稠密向量。
4. 使用中英文 n-gram BM25 生成稀疏向量。
5. 通过 Milvus Lite 原生混合检索和 `WeightedRanker(0.7, 0.3)` 排序。
6. 结合最近 6 条对话历史改写指代或省略类追问。
7. 回答使用 `[1]`、`[2]` 标注文献依据，并展示标题、页码和来源路径。

支持当前文献、笔记关联文献和主动选择的全部文献三种检索范围。

### 增量索引与缓存

- 通过 SHA-256 内容哈希生成分块标识。
- 更新文献时只向量化新增或变化的分块，复用未变化分块。
- 自动删除已经失效的旧分块。
- Dense 向量保存在本地 `literature_catalog.sqlite3`，未变化分块不重复推理。
- BM25 词表变化时根据本地目录安全重建 Milvus Sparse 索引，保证词项编号一致。
- 文献本地保存成功即可自动更新索引，不依赖 MySQL；普通笔记永不建立索引。
- 单次索引请求上限为 100 万字符，本地 Embedding 默认每批处理 16 个片段。

### 文献图表解析

视觉模型围绕图表类型、标题、坐标轴、图例、关键数值、变化趋势、结论与限制进行解析，不再定位为通用图片描述。

### 学术写作辅助

原“语法润色”调整为笔记专用的学术写作辅助，在不改变事实、论点和引用关系的前提下，优化表达、逻辑衔接、术语一致性和语言简洁度。导入文献保持只读，不开放学术改写。

## 本地优先

- 笔记和文献正文保存在本地 Markdown 文件中。
- AI 结果历史保存在本地 SQLite。
- Dense/Sparse 混合索引保存在本地 Milvus Lite。
- 分块目录和 Dense 向量缓存在本地 SQLite。
- MySQL 不可用时，本地文献保存与索引、笔记写作和 AI 历史仍可使用。

Embedding 完全在本机运行；结构化摘要、学术写作、RAG 最终生成和图表解析仍使用配置的大模型 API。

后端会优先从 `LOCAL_EMBED_MODEL_PATH` 或标准 Hugging Face 缓存目录解析 Jina 模型的本地快照，并使用绝对路径离线加载，不会在索引或检索时访问 Hugging Face。若模型存放在自定义目录，可在 `.env` 中配置：

```env
LOCAL_EMBED_MODEL_PATH=C:\path\to\models--jinaai--jina-embeddings-v5-text-nano\snapshots\<revision>
```

## API

基础地址：`http://127.0.0.1:8000`

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/api/summary` | 结构化文献摘要 |
| POST | `/api/summary/stream` | 流式结构化摘要 |
| POST | `/api/polish` | 学术写作辅助 |
| POST | `/api/polish/stream` | 流式学术写作辅助 |
| POST | `/api/rag/index` | 增量建立或更新文献索引 |
| POST | `/api/rag/retrieve` | 混合检索并返回来源元数据 |
| POST | `/api/rag/delete` | 删除指定文献索引 |
| POST | `/api/import` | 导入 PDF、Word、TXT 或 Markdown |
| GET | `/health` | 健康检查 |

`/api/rag/retrieve` 的 `document_ids` 字段用于限定检索范围；传入一个或多个文献 ID 时只检索指定文献，传空数组时表示主动检索全部文献。

## 运行

### 后端

```powershell
cd notes-server
python -m pip install -r requirements.txt
python main.py
```

### 桌面端

使用 Qt Creator 打开 `ai-chat-desktop/ai_chat_desktop.pro`，选择 Qt 5.12.11 MinGW 64-bit 构建。

## 测试

```powershell
cd notes-server
python -m unittest -v test_rag_engine.py
```

测试覆盖中英文分词、BM25、语义分块、多轮查询改写、异常回退、页码解析和内容哈希。

## 后续扩展

- 增加章节级摘要、单篇文献摘要和跨文献综述。
- 增加作者、年份、期刊、DOI 和章节等文献元数据。
- 增加 PDF 阅读器中的引用点击跳转与图表自动提取。
- 建立 Recall@K、MRR、引用正确率和响应时间评估集。
