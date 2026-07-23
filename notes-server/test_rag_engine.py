import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

from local_rag_engine import _bm25_tokens, resolve_local_model_path
from main import (
    RagRetrieveRequest,
    bm25_scores,
    chunk_content_hash,
    merge_semantic_sentences,
    reciprocal_rank_fusion,
    rewrite_retrieval_query,
    semantic_breakpoints,
    split_document_pages,
    tokenize,
)


class RagEngineTests(unittest.TestCase):
    def test_local_model_path_uses_explicit_snapshot(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            snapshot = Path(temp_dir)
            for name in (
                "config.json",
                "tokenizer_config.json",
                "model.safetensors",
            ):
                (snapshot / name).write_text("test", encoding="utf-8")
            with patch.dict(
                os.environ,
                {"LOCAL_EMBED_MODEL_PATH": str(snapshot)},
                clear=False,
            ):
                self.assertEqual(
                    resolve_local_model_path("unused/model"),
                    snapshot.resolve(),
                )

    def test_tokenize_supports_chinese_and_english(self):
        tokens = tokenize("RAG 检索-v2")
        self.assertIn("rag", tokens)
        self.assertIn("检索", tokens)
        self.assertIn("v2", tokens)

    def test_bm25_prioritizes_exact_terms(self):
        documents = ["向量检索适合语义匹配", "BM25 擅长精确关键词检索"]
        scores = bm25_scores("BM25 关键词", documents)
        self.assertGreater(scores[1], scores[0])

    def test_bm25_keeps_exact_chinese_technical_term(self):
        documents = [
            "正则表达式用于描述字符串匹配规则。",
            "Linux 进程管理与网络套接字编程。",
        ]
        scores = bm25_scores("正则表达式", documents)
        self.assertGreater(scores[0], scores[1])

    def test_local_milvus_tokenizer_keeps_chinese_terms(self):
        tokens = _bm25_tokens("Linux 正则表达式 regex")
        self.assertIn("linux", tokens)
        self.assertIn("正则表达式", tokens)
        self.assertIn("regex", tokens)

    def test_rrf_combines_dense_and_sparse_rankings(self):
        result = reciprocal_rank_fusion(
            [(["a", "b"], 0.7), (["b", "c"], 0.3)],
            limit=3,
        )
        self.assertEqual(result[0], "b")
        self.assertEqual(set(result), {"a", "b", "c"})

    def test_semantic_breakpoint_and_merge(self):
        embeddings = [[1.0, 0.0], [0.9, 0.1], [0.0, 1.0]]
        points = semantic_breakpoints(embeddings, percentile=25)
        self.assertEqual(points, [2])
        chunks = merge_semantic_sentences(
            ["第一句。", "第二句。", "新主题。"],
            points,
            target_chars=1,
        )
        self.assertEqual(chunks, ["第一句。第二句。", "新主题。"])

    def test_pdf_page_markers_preserve_page_numbers(self):
        pages = split_document_pages(
            "[[PAGE:1]]\n第一页内容\n\n[[PAGE:3]]\n第三页内容"
        )
        self.assertEqual(pages, [(1, "第一页内容"), (3, "第三页内容")])

    def test_chunk_hash_is_content_addressed(self):
        self.assertEqual(
            chunk_content_hash("相同内容"),
            chunk_content_hash("相同内容"),
        )
        self.assertNotEqual(
            chunk_content_hash("原始内容"),
            chunk_content_hash("修改内容"),
        )


class QueryRewriteTests(unittest.IsolatedAsyncioTestCase):
    def test_retrieve_request_accepts_selected_documents(self):
        request = RagRetrieveRequest(
            query="比较两篇文献的方法",
            document_ids=["doc-a", "doc-b"],
        )
        self.assertEqual(request.document_ids, ["doc-a", "doc-b"])

    async def test_first_question_does_not_call_llm(self):
        request = RagRetrieveRequest(query="什么是RAG？")
        with patch("main.call_llm", new_callable=AsyncMock) as call:
            result = await rewrite_retrieval_query(request)
        self.assertEqual(result, "什么是RAG？")
        call.assert_not_awaited()

    async def test_follow_up_is_rewritten(self):
        request = RagRetrieveRequest(
            query="为什么？",
            history=[
                {"role": "user", "content": "项目使用什么向量数据库？"},
                {"role": "assistant", "content": "项目使用 Milvus Lite。"},
            ],
            rewrite_api_key="test-key",
        )
        with patch(
            "main.call_llm",
            new=AsyncMock(return_value="项目为什么使用 Milvus Lite 向量数据库？"),
        ):
            result = await rewrite_retrieval_query(request)
        self.assertEqual(result, "项目为什么使用 Milvus Lite 向量数据库？")

    async def test_rewrite_failure_falls_back_to_original(self):
        request = RagRetrieveRequest(
            query="它有什么优点？",
            history=[{"role": "assistant", "content": "项目使用 Milvus Lite。"}],
        )
        with patch("main.call_llm", new=AsyncMock(side_effect=RuntimeError("offline"))):
            result = await rewrite_retrieval_query(request)
        self.assertEqual(result, "它有什么优点？")


if __name__ == "__main__":
    unittest.main()
