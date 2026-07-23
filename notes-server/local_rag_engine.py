"""Local literature RAG engine backed by Jina embeddings and Milvus Lite."""

from __future__ import annotations

import gc
import json
import os
import re
import sqlite3
import sys
import threading
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence


# This service is local-first. Set offline mode before Transformers or
# huggingface_hub can cache their environment configuration.
os.environ.setdefault("HF_HUB_OFFLINE", "1")
os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")
os.environ.setdefault(
    "HF_MODULES_CACHE",
    str(Path(__file__).resolve().parent / ".hf_modules"),
)

_TOKEN_RE = re.compile(r"[a-zA-Z0-9_]+|[\u4e00-\u9fff]+")
_ORIGINAL_RENAME = os.rename
_MILVUS_RENAME_PATCHED = False
_REQUIRED_MODEL_FILES = ("config.json", "tokenizer_config.json")


def _is_complete_model_dir(path: Path) -> bool:
    if not path.is_dir():
        return False
    if not all((path / name).exists() for name in _REQUIRED_MODEL_FILES):
        return False
    return any(
        (path / name).exists()
        for name in ("model.safetensors", "pytorch_model.bin")
    )


def resolve_local_model_path(model_name: str) -> Path:
    """Resolve a model name to a complete local snapshot without Hub access."""
    explicit_path = os.getenv("LOCAL_EMBED_MODEL_PATH", "").strip()
    candidates: List[Path] = []
    if explicit_path:
        candidates.append(Path(explicit_path).expanduser())

    model_path = Path(model_name).expanduser()
    if model_path.is_absolute() or model_path.exists():
        candidates.append(model_path)

    cache_roots: List[Path] = []
    hub_cache = os.getenv("HUGGINGFACE_HUB_CACHE", "").strip()
    if hub_cache:
        cache_roots.append(Path(hub_cache).expanduser())
    hf_home = os.getenv("HF_HOME", "").strip()
    if hf_home:
        cache_roots.append(Path(hf_home).expanduser() / "hub")
    cache_roots.append(Path.home() / ".cache" / "huggingface" / "hub")

    cache_folder = f"models--{model_name.replace('/', '--')}"
    for cache_root in cache_roots:
        model_cache = cache_root / cache_folder
        ref_path = model_cache / "refs" / "main"
        if ref_path.is_file():
            revision = ref_path.read_text(encoding="utf-8").strip()
            if revision:
                candidates.append(model_cache / "snapshots" / revision)
        snapshots = model_cache / "snapshots"
        if snapshots.is_dir():
            candidates.extend(
                sorted(
                    (path for path in snapshots.iterdir() if path.is_dir()),
                    key=lambda path: path.stat().st_mtime,
                    reverse=True,
                )
            )

    seen = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if _is_complete_model_dir(resolved):
            return resolved

    searched = "\n".join(f"- {path}" for path in candidates) or "- 未发现候选路径"
    raise FileNotFoundError(
        f"未找到完整的本地 Embedding 模型：{model_name}\n"
        f"已检查：\n{searched}\n"
        "请将 LOCAL_EMBED_MODEL_PATH 指向模型快照目录。"
    )


def _patch_windows_milvus_rename() -> None:
    """Work around Milvus Lite manifest replacement on Windows."""
    global _MILVUS_RENAME_PATCHED
    if sys.platform != "win32" or _MILVUS_RENAME_PATCHED:
        return

    def safe_rename(source, destination):
        destination_text = os.fspath(destination).replace("\\", "/").lower()
        is_milvus_manifest = (
            "literature_milvus.db/collections/" in destination_text
            and destination_text.endswith("/manifest.json")
        )
        if is_milvus_manifest and os.path.exists(destination):
            os.remove(destination)
        return _ORIGINAL_RENAME(source, destination)

    os.rename = safe_rename
    _MILVUS_RENAME_PATCHED = True


def _bm25_tokens(text: str) -> List[str]:
    tokens: List[str] = []
    for raw in _TOKEN_RE.findall(text.lower()):
        if re.fullmatch(r"[\u4e00-\u9fff]+", raw):
            tokens.extend(raw)
            for size in range(2, min(5, len(raw)) + 1):
                tokens.extend(
                    raw[index:index + size]
                    for index in range(len(raw) - size + 1)
                )
        else:
            tokens.append(raw)
    return tokens


def _build_bm25():
    from pymilvus.model.sparse import BM25EmbeddingFunction
    from pymilvus.model.sparse.bm25.tokenizers import Analyzer, Tokenizer

    class LiteratureTokenizer(Tokenizer):
        def tokenize(self, text: str):
            return _bm25_tokens(text)

    analyzer = Analyzer(
        name="literature_zh_en_ngram",
        tokenizer=LiteratureTokenizer(),
        preprocessors=[],
        filters=[],
    )
    return BM25EmbeddingFunction(analyzer=analyzer, num_workers=1)


def _sparse_to_dict(result, index: int = 0) -> dict:
    if isinstance(result, list):
        if index < len(result) and isinstance(result[index], dict):
            return result[index]
        raise ValueError("BM25 returned an invalid list result")
    if isinstance(result, dict):
        return result

    import scipy.sparse as sp

    if sp.issparse(result):
        coo = result.tocoo()
        mask = coo.row == index
        return {
            int(column): float(value)
            for column, value in zip(coo.col[mask], coo.data[mask])
        }
    raise ValueError(f"Unsupported BM25 result type: {type(result)}")


class LocalRagEngine:
    """Owns the local model, chunk catalog, and Milvus hybrid index."""

    def __init__(
        self,
        base_dir: Path,
        model_name: str = "jinaai/jina-embeddings-v5-text-nano",
        collection_name: str = "literature_chunks",
    ) -> None:
        self.base_dir = Path(base_dir).resolve()
        _patch_windows_milvus_rename()
        self.base_dir.mkdir(parents=True, exist_ok=True)
        self.model_name = model_name
        self.collection_name = collection_name
        self.catalog_path = self.base_dir / "literature_catalog.sqlite3"
        self.milvus_path = self.base_dir / "literature_milvus.db"
        self._model = None
        self._client = None
        self._bm25 = None
        self._lock = threading.RLock()
        self._init_catalog()

    def _connect_catalog(self) -> sqlite3.Connection:
        conn = sqlite3.connect(str(self.catalog_path), timeout=60)
        conn.row_factory = sqlite3.Row
        return conn

    def _init_catalog(self) -> None:
        with self._connect_catalog() as conn:
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS chunks (
                    chunk_id TEXT PRIMARY KEY,
                    note_id TEXT NOT NULL,
                    title TEXT NOT NULL,
                    source TEXT NOT NULL,
                    page INTEGER NOT NULL,
                    chunk_index INTEGER NOT NULL,
                    content_hash TEXT NOT NULL,
                    text TEXT NOT NULL,
                    dense_vector TEXT NOT NULL,
                    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                )
                """
            )
            conn.execute(
                "CREATE INDEX IF NOT EXISTS idx_chunks_note_id ON chunks(note_id)"
            )

    def _load_model(self):
        if self._model is None:
            from sentence_transformers import SentenceTransformer

            local_model_path = resolve_local_model_path(self.model_name)
            print(f"[embedding] loading local model: {local_model_path}")
            model_dir = str(local_model_path)
            added_to_path = model_dir not in sys.path
            if added_to_path:
                sys.path.insert(0, model_dir)
            try:
                self._model = SentenceTransformer(
                    model_dir,
                    trust_remote_code=True,
                    local_files_only=True,
                    model_kwargs={"default_task": "retrieval"},
                )
            finally:
                if added_to_path:
                    try:
                        sys.path.remove(model_dir)
                    except ValueError:
                        pass
            self._model.default_task = "retrieval"
        return self._model

    def encode_documents(self, texts: Sequence[str]) -> List[List[float]]:
        if not texts:
            return []
        model = self._load_model()
        vectors = model.encode(
            list(texts),
            batch_size=int(os.getenv("LOCAL_EMBED_BATCH_SIZE", "16")),
            normalize_embeddings=True,
            task="retrieval",
            prompt_name="document",
            show_progress_bar=False,
        )
        return vectors.tolist()

    def encode_query(self, text: str) -> List[float]:
        model = self._load_model()
        vector = model.encode(
            text,
            normalize_embeddings=True,
            task="retrieval",
            prompt_name="query",
            show_progress_bar=False,
        )
        return vector.tolist()

    def replace_note(self, note_id: str, records: Sequence[dict]) -> dict:
        """Persist changed chunks and rebuild the sparse index consistently."""
        with self._lock:
            with self._connect_catalog() as conn:
                existing_rows = conn.execute(
                    "SELECT chunk_id, dense_vector FROM chunks WHERE note_id = ?",
                    (note_id,),
                ).fetchall()
                existing_vectors = {
                    row["chunk_id"]: json.loads(row["dense_vector"])
                    for row in existing_rows
                }
                desired_ids = {record["id"] for record in records}
                existing_ids = set(existing_vectors)
                new_records = [
                    record for record in records if record["id"] not in existing_ids
                ]
                new_vectors = self.encode_documents(
                    [record["text"] for record in new_records]
                )
                vector_by_id = dict(existing_vectors)
                vector_by_id.update({
                    record["id"]: vector
                    for record, vector in zip(new_records, new_vectors)
                })

                conn.execute("DELETE FROM chunks WHERE note_id = ?", (note_id,))
                conn.executemany(
                    """
                    INSERT INTO chunks (
                        chunk_id, note_id, title, source, page, chunk_index,
                        content_hash, text, dense_vector
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    [
                        (
                            record["id"],
                            note_id,
                            record["title"],
                            record["source"],
                            int(record["page"]),
                            int(record["idx"]),
                            record["hash"],
                            record["text"],
                            json.dumps(vector_by_id[record["id"]]),
                        )
                        for record in records
                    ],
                )
                conn.commit()

            self._rebuild_milvus()
            return {
                "indexed": len(records),
                "added": len(new_records),
                "removed": len(existing_ids - desired_ids),
                "reused": len(records) - len(new_records),
            }

    def delete_note(self, note_id: str) -> None:
        with self._lock:
            with self._connect_catalog() as conn:
                conn.execute("DELETE FROM chunks WHERE note_id = ?", (note_id,))
                conn.commit()
            self._rebuild_milvus()

    def _all_rows(self) -> List[sqlite3.Row]:
        with self._connect_catalog() as conn:
            return conn.execute(
                "SELECT * FROM chunks ORDER BY note_id, chunk_index"
            ).fetchall()

    def _close_client(self) -> None:
        if self._client is not None:
            try:
                self._client.close()
            except Exception:
                pass
            self._client = None
            gc.collect()

    def _create_collection(self, client) -> None:
        from pymilvus import DataType

        schema = client.create_schema(auto_id=False, enable_dynamic_field=False)
        schema.add_field(
            "chunk_id", DataType.VARCHAR, is_primary=True, max_length=128
        )
        schema.add_field("dense_vector", DataType.FLOAT_VECTOR, dim=768)
        schema.add_field("sparse_vector", DataType.SPARSE_FLOAT_VECTOR)
        schema.add_field("text", DataType.VARCHAR, max_length=65535)
        schema.add_field("note_id", DataType.VARCHAR, max_length=128)
        schema.add_field("title", DataType.VARCHAR, max_length=1000)
        schema.add_field("source", DataType.VARCHAR, max_length=4000)
        schema.add_field("page", DataType.INT64)
        schema.add_field("chunk_index", DataType.INT64)
        schema.add_field("content_hash", DataType.VARCHAR, max_length=64)

        indexes = client.prepare_index_params()
        indexes.add_index(
            "dense_vector", index_type="FLAT", metric_type="COSINE"
        )
        indexes.add_index(
            "sparse_vector",
            index_type="SPARSE_INVERTED_INDEX",
            metric_type="IP",
        )
        client.create_collection(
            collection_name=self.collection_name,
            schema=schema,
            index_params=indexes,
        )

    def _rebuild_milvus(self) -> None:
        from pymilvus import MilvusClient
        rows = self._all_rows()
        texts = [row["text"] for row in rows]
        bm25 = _build_bm25()
        if texts:
            bm25.fit(texts)

        self._close_client()
        client = MilvusClient(str(self.milvus_path))
        if client.has_collection(self.collection_name):
            client.drop_collection(self.collection_name)
        self._create_collection(client)

        if rows:
            sparse_vectors = bm25.encode_documents(texts)
            batch_size = int(os.getenv("MILVUS_INSERT_BATCH_SIZE", "100"))
            for start in range(0, len(rows), batch_size):
                batch = rows[start:start + batch_size]
                data = []
                for offset, row in enumerate(batch):
                    index = start + offset
                    data.append({
                        "chunk_id": row["chunk_id"],
                        "dense_vector": json.loads(row["dense_vector"]),
                        "sparse_vector": _sparse_to_dict(
                            sparse_vectors, index=index
                        ),
                        "text": row["text"],
                        "note_id": row["note_id"],
                        "title": row["title"],
                        "source": row["source"],
                        "page": row["page"],
                        "chunk_index": row["chunk_index"],
                        "content_hash": row["content_hash"],
                    })
                client.insert(self.collection_name, data)
        client.load_collection(self.collection_name)
        self._client = client
        self._bm25 = bm25

    def _ensure_ready(self) -> bool:
        if self._client is None or self._bm25 is None:
            self._rebuild_milvus()
        return bool(self._all_rows())

    def search(
        self,
        query: str,
        top_k: int,
        document_ids: Optional[Sequence[str]] = None,
    ) -> List[dict]:
        with self._lock:
            if not self._ensure_ready():
                return []

            from pymilvus import AnnSearchRequest, WeightedRanker

            dense_vector = self.encode_query(query)
            sparse_vector = _sparse_to_dict(
                self._bm25.encode_queries([query]), index=0
            )
            expression = ""
            if document_ids:
                safe_ids = [
                    document_id.replace("\\", "\\\\").replace('"', '\\"')
                    for document_id in document_ids
                ]
                quoted = ", ".join(f'"{document_id}"' for document_id in safe_ids)
                expression = f"note_id in [{quoted}]"
            limit = max(top_k * 4, top_k)
            common = {"limit": limit}
            if expression:
                common["expr"] = expression
            dense_request = AnnSearchRequest(
                data=[dense_vector],
                anns_field="dense_vector",
                param={"metric_type": "COSINE", "params": {}},
                **common,
            )
            sparse_request = AnnSearchRequest(
                data=[sparse_vector],
                anns_field="sparse_vector",
                param={"metric_type": "IP", "params": {}},
                **common,
            )
            results = self._client.hybrid_search(
                collection_name=self.collection_name,
                reqs=[dense_request, sparse_request],
                ranker=WeightedRanker(0.7, 0.3),
                limit=top_k,
                output_fields=[
                    "chunk_id", "text", "note_id", "title", "source",
                    "page", "chunk_index", "content_hash",
                ],
            )
            output = []
            for hit in results[0]:
                entity = hit["entity"]
                output.append({
                    "chunk_id": entity["chunk_id"],
                    "text": entity["text"],
                    "note_id": entity["note_id"],
                    "title": entity["title"],
                    "source": entity["source"],
                    "page": entity["page"],
                    "chunk_index": entity["chunk_index"],
                    "score": float(hit["distance"]),
                })
            return output

    def stats(self) -> Dict[str, object]:
        rows = self._all_rows()
        return {
            "backend": "milvus_lite",
            "embedding": self.model_name,
            "dimensions": 768,
            "chunks": len(rows),
            "documents": len({row["note_id"] for row in rows}),
            "database": str(self.milvus_path),
        }
