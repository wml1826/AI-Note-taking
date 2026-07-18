"""
智能笔记软件 - FastAPI 后端
代理云端大模型（DeepSeek / 通义千问），提供笔记场景接口：
  POST /api/summary          智能摘要
  POST /api/polish           语法润色
  POST /api/summary/stream   流式摘要
  POST /api/polish/stream    流式润色
  POST /api/rag/index        建立向量索引（切块 + 向量化，存 Chroma）
  POST /api/rag/retrieve     检索与问题最相关的 Top-K 片段（note_id 为空时全局检索）
  POST /api/rag/delete       删除指定笔记的向量索引
  POST /api/import            导入 PDF/Word 文档，提取纯文本
  GET  /health               健康检查

设计说明：
- 后端持有默认配置（见 .env），也可由请求体覆盖 api_key / base_url / model
- 使用 OpenAI 兼容协议调用云端 Chat Completions 接口
- CORS 放开，允许本地 Qt 客户端访问
- RAG 仅负责「切块 + 向量化 + 向量库检索」，最终 Prompt 组装与 LLM 生成由 C++ 客户端完成
"""
import os
import re
import json
from typing import List, Optional

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, UploadFile, File
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from pydantic import BaseModel
from pydantic import Field

load_dotenv()

# ---- RAG 检索所需：Chroma 向量库 + DashScope 文本向量化 ----
# 设计边界：Python 只负责「切块 + 向量化 + 向量库检索」这一段；
# 最终「组装 Prompt + 调 LLM 生成回答」由 C++ 客户端完成（复用笔记页聊天客户端）。
# 注意：chromadb 仅 RAG 接口用到，故惰性导入——未安装时其余接口（摘要/润色/问答）仍可正常启动。
_RAG_COLLECTION = None

def _coll():
    """惰性获取 Chroma 集合：首次调用时才 import chromadb 并建立持久化客户端。"""
    global _RAG_COLLECTION
    if _RAG_COLLECTION is None:
        import chromadb
        path = os.getenv("CHROMA_PATH", "./chroma_data")
        # 确保目录存在（chromadb 0.4.x 不会自动创建）
        os.makedirs(path, exist_ok=True)
        client = chromadb.PersistentClient(path=path)
        coll = client.get_or_create_collection(
            "notes_rag",
            metadata={"hnsw:space": "cosine"},
        )
        if coll is None:
            raise RuntimeError(
                f"ChromaDB get_or_create_collection 返回 None (path={path}, "
                f"chromadb={chromadb.__version__})，请检查路径权限或磁盘空间")
        _RAG_COLLECTION = coll
    return _RAG_COLLECTION

# 文本向量模型（通义千问 embedding，复用 DASHSCOPE_API_KEY，无需新 Key）
_EMBED_MODEL = os.getenv("EMBED_MODEL", "text-embedding-v3")
_EMBED_DIM = int(os.getenv("EMBED_DIM", "1024"))
_EMBED_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1/embeddings"

app = FastAPI(title="智能笔记软件 API", version="1.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ---- 默认服务商配置（可被请求体覆盖）----
DEFAULT_PROVIDERS = {
    "deepseek": {
        "api_key": os.getenv("DEEPSEEK_API_KEY", ""),
        "base_url": os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com/v1"),
        "model": os.getenv("DEEPSEEK_MODEL", "deepseek-chat"),
    },
    "dashscope": {
        "api_key": os.getenv("DASHSCOPE_API_KEY", ""),
        "base_url": os.getenv("DASHSCOPE_BASE_URL",
                              "https://dashscope.aliyuncs.com/compatible-mode/v1"),
        "model": os.getenv("DASHSCOPE_MODEL", "qwen-plus"),
    },
}


# ---- 请求模型 ----
class TextRequest(BaseModel):
    text: str = Field(..., max_length=200000)
    provider: str = "deepseek"          # deepseek / dashscope
    detail: str = "medium"              # short / medium / detailed（仅摘要用）
    api_key: Optional[str] = None       # 可选：覆盖默认
    base_url: Optional[str] = None
    model: Optional[str] = None


# ---- RAG 请求模型 ----
class RagIndexRequest(BaseModel):
    note_id: str                 # 笔记唯一 id（C++ 端生成，与 MySQL notes.id 一致）
    text: str = Field(..., max_length=200000)
    api_key: Optional[str] = None   # 可选覆盖 embedding 用的 DashScope Key


class RagRetrieveRequest(BaseModel):
    note_id: Optional[str] = None     # 为空时全局检索（跨所有笔记）
    query: str = Field(..., max_length=5000)
    top_k: int = 3
    api_key: Optional[str] = None


class RagDeleteRequest(BaseModel):
    note_id: str


# ---- 切块：按段落切，长段落按句滑动窗口切分，短段落合并到 target_chars ----
def chunk_text(text: str, max_chars: int = 800, overlap: int = 100,
               target_chars: int = 400) -> List[str]:
    text = text.strip()
    if not text:
        return []
    paras = [p.strip() for p in text.split("\n") if p.strip()]
    raw: List[str] = []
    for p in paras:
        if len(p) <= max_chars:
            raw.append(p)
            continue
        # 长段落按句子切
        sents = re.split(r"(?<=[。！？!?；;\n])", p)
        buf = ""
        for s in sents:
            if len(buf) + len(s) <= max_chars:
                buf += s
            else:
                if buf:
                    raw.append(buf)
                # 滑动窗口重叠
                buf = (buf[-overlap:] if len(buf) > overlap else "") + s
        if buf:
            raw.append(buf)
    # 合并相邻短片段：当前片段不足 target_chars 时，把下一段拼进来（不超 max_chars）
    merged: List[str] = []
    for c in raw:
        if merged and len(merged[-1]) < target_chars \
           and len(merged[-1]) + len(c) + 1 <= max_chars:
            merged[-1] += "\n" + c
        else:
            merged.append(c)
    # 去重相邻重复
    out: List[str] = []
    for c in merged:
        if not out or c != out[-1]:
            out.append(c)
    return out


# ---- 文本向量化（DashScope text-embedding-v3，OpenAI 兼容协议）----
_EMBED_BATCH_SIZE = 10  # DashScope text-embedding-v3 单次最多 10 条
_EMBED_MAX_TOTAL_CHARS = 200000  # 单次索引总字符上限，防止超大文档触发海量 embedding 调用

async def embed_texts(texts: List[str], api_key: Optional[str] = None) -> List[List[float]]:
    """将文本列表向量化，自动按 DashScope 限制分批调用（每批 ≤10 条）。
    使用 httpx.AsyncClient 避免阻塞事件循环。"""
    key = api_key or os.getenv("DASHSCOPE_API_KEY", "")
    if not key:
        raise HTTPException(status_code=400,
                            detail="未配置 DASHSCOPE_API_KEY（embedding 需要通义千问 Key）")
    # 输入大小校验
    total_chars = sum(len(t) for t in texts)
    if total_chars > _EMBED_MAX_TOTAL_CHARS:
        raise HTTPException(status_code=413,
                            detail=f"文本总长度 {total_chars} 超过限制 {_EMBED_MAX_TOTAL_CHARS}，"
                                  f"请缩短笔记内容后重试")
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
    all_embeddings: List[List[float]] = []
    async with httpx.AsyncClient(timeout=60.0) as client:
        for i in range(0, len(texts), _EMBED_BATCH_SIZE):
            batch = texts[i : i + _EMBED_BATCH_SIZE]
            payload = {"model": _EMBED_MODEL, "input": batch, "dimensions": _EMBED_DIM}
            try:
                resp = await client.post(_EMBED_URL, headers=headers, json=payload)
            except httpx.ConnectError:
                raise HTTPException(status_code=502,
                                    detail="无法连接 DashScope embedding 服务（网络不通）")
            if resp.status_code != 200:
                raise HTTPException(status_code=resp.status_code,
                                    detail=f"embedding 服务错误（批次 {i//_EMBED_BATCH_SIZE+1}）：{resp.text[:300]}")
            data = resp.json().get("data", [])
            all_embeddings.extend(d["embedding"] for d in sorted(data, key=lambda x: x["index"]))
    return all_embeddings


async def call_llm(provider: str, system_prompt: str, user_prompt: str,
                   req_key: Optional[str], req_url: Optional[str],
                   req_model: Optional[str], temperature: float = 0.3) -> str:
    """调用云端 OpenAI 兼容接口，返回模型文本。temperature 默认 0.3（摘要/问答），
    润色用 0.5 在保持原意的前提下适度改写句式。"""
    if provider not in DEFAULT_PROVIDERS:
        provider = "deepseek"
    cfg = DEFAULT_PROVIDERS[provider]

    api_key = req_key or cfg["api_key"]
    base_url = (req_url or cfg["base_url"]).rstrip("/")
    model = req_model or cfg["model"]

    if not api_key:
        raise HTTPException(
            status_code=400,
            detail=f"服务商 [{provider}] 未配置 API Key（请在 .env 或请求中提供）")

    url = f"{base_url}/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": temperature,
    }

    async with httpx.AsyncClient(timeout=90.0) as client:
        try:
            resp = await client.post(url, headers=headers, json=payload)
        except httpx.ConnectError:
            raise HTTPException(status_code=502,
                                detail="无法连接模型服务（BaseURL 错误或网络不通）")
        if resp.status_code != 200:
            # 透传服务端错误文本，便于排错
            raise HTTPException(status_code=resp.status_code,
                                detail=f"模型服务返回错误：{resp.text[:500]}")
        data = resp.json()
        # 防御性解析：API 可能返回格式异常
        choices = data.get("choices") or []
        if not choices:
            raise HTTPException(status_code=502,
                                detail=f"模型返回异常（choices 为空）：{str(data)[:300]}")
        msg = choices[0].get("message", {})
        content = msg.get("content", "")
        if not content:
            raise HTTPException(status_code=502,
                                detail="模型返回内容为空")
        return content.strip()


async def call_llm_stream(provider: str, system_prompt: str, user_prompt: str,
                          req_key: Optional[str], req_url: Optional[str],
                          req_model: Optional[str], temperature: float = 0.3):
    """流式调用云端 OpenAI 兼容接口，逐块 yield 文本片段。"""
    if provider not in DEFAULT_PROVIDERS:
        provider = "deepseek"
    cfg = DEFAULT_PROVIDERS[provider]

    api_key = req_key or cfg["api_key"]
    base_url = (req_url or cfg["base_url"]).rstrip("/")
    model = req_model or cfg["model"]

    if not api_key:
        raise HTTPException(
            status_code=400,
            detail=f"服务商 [{provider}] 未配置 API Key（请在 .env 或请求中提供）")

    url = f"{base_url}/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": temperature,
        "stream": True,
    }
    print(f"[call_llm_stream] url={url}  model={model}  provider={provider}")

    # 重试：RemoteProtocolError 等瞬时错误重试 2 次
    import asyncio
    yielded = False
    last_error = None
    for attempt in range(3):
        try:
            async with httpx.AsyncClient(timeout=120.0) as client:
                async with client.stream("POST", url, headers=headers, json=payload) as resp:
                    if resp.status_code != 200:
                        body = await resp.aread()
                        raise HTTPException(status_code=resp.status_code,
                                            detail=f"模型服务返回错误：{body.decode()[:500]}")
                    async for line in resp.aiter_lines():
                        if line.startswith("data: "):
                            chunk = line[6:]
                            if chunk.strip() == "[DONE]":
                                break
                            try:
                                obj = json.loads(chunk)
                                delta = (obj.get("choices") or [{}])[0].get("delta", {})
                                text = delta.get("content", "")
                                if text:
                                    yielded = True
                                    yield text
                            except (json.JSONDecodeError, IndexError, KeyError):
                                continue
            return  # 成功，退出重试循环
        except (httpx.RemoteProtocolError, httpx.ConnectError,
                httpx.ReadTimeout, httpx.ConnectTimeout) as e:
            last_error = e
            if yielded:
                # 已发送部分数据，不能重试（会导致重复）
                raise HTTPException(status_code=502,
                                    detail=f"流式传输中断：{e}")
            if attempt < 2:
                print(f"[call_llm_stream] 第 {attempt+1} 次失败({e})，{attempt+1}s 后重试…")
                await asyncio.sleep(attempt + 1)
            continue
    raise HTTPException(status_code=502,
                        detail=f"多次重试后仍失败：{last_error}")


# ---- 三个场景接口 ----
@app.post("/api/summary")
async def summary(req: TextRequest):
    level_map = {
        "short": "一句话的",
        "medium": "一段简洁的",
        "detailed": "详细的、保留关键数据与细节的",
    }
    level = level_map.get(req.detail, "一段简洁的")
    system = (f"你是一个专业的文本摘要助手。请为用户输入的笔记文本生成{level}摘要，"
              f"保留核心观点、关键信息和数据，不要编造原文没有的内容。")
    result = await call_llm(req.provider, system, req.text,
                            req.api_key, req.base_url, req.model)
    return {"summary": result}


@app.post("/api/polish")
async def polish(req: TextRequest):
    system = (
        "你是一个文字编辑与润色专家。你的任务是对用户文本进行**适度润色**，而非推翻重写。\n\n"
        "润色原则（按优先级排序）：\n"
        "1. **修正错误**：修正语法错误、错别字、标点误用、病句；\n"
        "2. **精炼冗余**：删去重复啰嗦的表述、无意义的口头禅（如「然后」「就是」「的话」滥用）；\n"
        "3. **提升流畅度**：调整语序让句子更通顺自然，补全缺省的主语或连接词；\n"
        "4. **适度升级词汇**：仅当原文用词明显不当时替换为更精准的表达；若原文已通顺则保留原词。\n\n"
        "**硬性约束**：\n"
        "- 必须保留原文的核心观点、事实信息、原有语气和表达风格不变；\n"
        "- 不要为了显得「高级」而把简单的话改成生僻晦涩的表达；\n"
        "- 若某句话本身已经通顺得体，则**原样保留**，不要强行改写；\n"
        "- 仅返回润色后的完整正文，不要任何解释、前缀、引号或元评论。\n\n"
        "示例（注意：改动是局部的，不是整句推翻）：\n\n"
        "示例1\n"
        "原文：今天天气不错，我和朋友去公园玩，我们在草地上晒太阳，感觉特别放松。\n"
        "润色：今天天气很好，我和朋友到公园散步，在草地上晒了会儿太阳，感到格外放松。\n\n"
        "示例2\n"
        "原文：这个产品有很多功能，但是价格太贵了，所以我觉得不太划算。\n"
        "润色：这款产品功能丰富，但定价偏高，性价比不太理想。\n\n"
        "示例3（几乎不需改动的通顺原文）\n"
        "原文：RAG 通过检索相关片段来增强模型回答的准确性。\n"
        "润色：RAG 通过检索相关片段来增强模型回答的准确性。"
    )
    result = await call_llm(req.provider, system, req.text,
                            req.api_key, req.base_url, req.model,
                            temperature=0.5)
    return {"polished": result}


# ---- 流式接口（逐块返回 LLM 文本）----
@app.post("/api/summary/stream")
async def summary_stream(req: TextRequest):
    level_map = {
        "short": "一句话的",
        "medium": "一段简洁的",
        "detailed": "详细的、保留关键数据与细节的",
    }
    level = level_map.get(req.detail, "一段简洁的")
    system = (f"你是一个专业的文本摘要助手。请为用户输入的笔记文本生成{level}摘要，"
              f"保留核心观点、关键信息和数据，不要编造原文没有的内容。")

    async def gen():
        async for chunk in call_llm_stream(req.provider, system, req.text,
                                          req.api_key, req.base_url, req.model):
            yield chunk
    return StreamingResponse(gen(), media_type="text/plain; charset=utf-8")


@app.post("/api/polish/stream")
async def polish_stream(req: TextRequest):
    system = (
        "你是一个文字编辑与润色专家。你的任务是对用户文本进行**适度润色**，而非推翻重写。\n\n"
        "润色原则（按优先级排序）：\n"
        "1. **修正错误**：修正语法错误、错别字、标点误用、病句；\n"
        "2. **精炼冗余**：删去重复啰嗦的表述、无意义的口头禅；\n"
        "3. **提升流畅度**：调整语序让句子更通顺自然，补全缺省的主语或连接词；\n"
        "4. **适度升级词汇**：仅当原文用词明显不当时替换为更精准的表达；若原文已通顺则保留原词。\n\n"
        "**硬性约束**：\n"
        "- 必须保留原文的核心观点、事实信息、原有语气和表达风格不变；\n"
        "- 不要为了显得「高级」而把简单的话改成生僻晦涩的表达；\n"
        "- 若某句话本身已经通顺得体，则**原样保留**，不要强行改写；\n"
        "- 仅返回润色后的完整正文，不要任何解释、前缀、引号或元评论。\n"
    )
    async def gen():
        async for chunk in call_llm_stream(req.provider, system, req.text,
                                          req.api_key, req.base_url, req.model,
                                          temperature=0.5):
            yield chunk
    return StreamingResponse(gen(), media_type="text/plain; charset=utf-8")


# ---- RAG 端点：Python 仅负责检索（切块 + 向量化 + 向量库）----
@app.post("/api/rag/index")
async def rag_index(req: RagIndexRequest):
    """为某条笔记建立向量索引：切块 → 向量化 → 存入 Chroma（先清旧索引）。"""
    chunks = chunk_text(req.text)
    if not chunks:
        return {"indexed": 0}
    print(f"[rag_index] note_id={req.note_id}  原文{len(req.text)}字  "
          f"切块{len(chunks)}个  平均{sum(len(c) for c in chunks)//len(chunks)}字/块")
    embeddings = await embed_texts(chunks, req.api_key)
    ids = [f"{req.note_id}:{i}" for i in range(len(chunks))]
    metadatas = [{"note_id": req.note_id, "idx": i} for i in range(len(chunks))]
    coll = _coll()
    # 覆盖式重建该笔记索引（按 note_id 元数据删除旧片段；chromadb 不支持 ids 通配）
    try:
        coll.delete(where={"note_id": req.note_id})
    except Exception:
        pass
    coll.add(ids=ids, embeddings=embeddings, documents=chunks, metadatas=metadatas)
    return {"indexed": len(chunks)}


@app.post("/api/rag/retrieve")
async def rag_retrieve(req: RagRetrieveRequest):
    """检索与 query 最相关的 Top-K 片段（供 C++ 端组装 Prompt 用）。
    note_id 为空时全局检索（跨所有笔记）。"""
    coll = _coll()
    # 检查是否有索引数据
    try:
        cnt = coll.count()
    except Exception:
        cnt = 0
    if cnt == 0:
        return {"chunks": []}
    q_emb = (await embed_texts([req.query], req.api_key))[0]
    # note_id 为空时全局检索，否则限定在该笔记内
    where_filter = {"note_id": req.note_id} if req.note_id else None
    if where_filter:
        res = coll.query(query_embeddings=[q_emb], n_results=req.top_k,
                         where=where_filter)
    else:
        res = coll.query(query_embeddings=[q_emb], n_results=req.top_k)
    docs = (res.get("documents") or [[]])[0]
    # 调试日志：查看检索到的片段内容
    print(f"[rag_retrieve] query='{req.query}'  note_id={req.note_id}  "
          f"top_k={req.top_k}  检索到 {len(docs)} 个片段:")
    for i, d in enumerate(docs):
        print(f"  [{i+1}] ({len(d)}字) {d[:80]}...")
    return {"chunks": docs}


@app.post("/api/rag/delete")
async def rag_delete(req: RagDeleteRequest):
    """删除某条笔记的所有向量索引（删笔记时调用）。"""
    coll = _coll()
    try:
        coll.delete(where={"note_id": req.note_id})
    except Exception:
        pass
    return {"deleted": True}


# ---- 文档导入：从 PDF / Word 提取纯文本 ----
@app.post("/api/import")
async def import_document(file: UploadFile = File(...)):
    """上传 PDF 或 Word 文件，提取纯文本返回。
    支持文字版 PDF（.pdf）和 Word（.docx），不支持扫描版 PDF。"""
    import io

    filename = file.filename or "未命名"
    ext = filename.rsplit(".", 1)[-1].lower() if "." in filename else ""
    title = filename.rsplit(".", 1)[0] if "." in filename else filename

    raw = await file.read()
    if len(raw) > 50 * 1024 * 1024:
        raise HTTPException(status_code=413, detail="文件过大（超过 50MB），请拆分后导入")

    try:
        if ext == "pdf":
            import pdfplumber
            pages = []
            with pdfplumber.open(io.BytesIO(raw)) as doc:
                for pg in doc.pages:
                    t = pg.extract_text() or ""
                    pages.append(t)
            text = "\n\n".join(pages).strip()
            if not text:
                raise HTTPException(status_code=422,
                                    detail="未能从 PDF 中提取到文字，可能是扫描版 PDF（需 OCR 支持）")

        elif ext == "docx":
            from docx import Document
            doc = Document(io.BytesIO(raw))
            paragraphs = [p.text.strip() for p in doc.paragraphs if p.text.strip()]
            # 也提取表格中的文本
            for tbl in doc.tables:
                for row in tbl.rows:
                    for cell in row.cells:
                        if cell.text.strip():
                            paragraphs.append(cell.text.strip())
            text = "\n\n".join(paragraphs)
            if not text:
                raise HTTPException(status_code=422, detail="Word 文档内容为空")

        else:
            raise HTTPException(status_code=400,
                                detail=f"不支持的文件类型 .{ext}，仅支持 .pdf 和 .docx")

    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"文档解析失败：{e}")

    return {"title": title, "content": text}


@app.get("/health")
async def health():
    return {"status": "ok"}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8000)
