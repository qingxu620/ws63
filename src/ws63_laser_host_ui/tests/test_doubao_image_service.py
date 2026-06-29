from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import requests

from app.doubao_image_service import DoubaoImageError, DoubaoImageService


class _FakeResponse:
    def __init__(
        self,
        status_code: int,
        payload: dict | None = None,
        text: str = "",
        headers: dict[str, str] | None = None,
        content: bytes = b"",
    ) -> None:
        self.status_code = status_code
        self._payload = payload
        self.text = text
        self.headers = headers or {}
        self.content = content

    def json(self):
        if self._payload is None:
            raise ValueError("no json")
        return self._payload


class _FakeSession:
    def __init__(
        self,
        post_response: _FakeResponse | Exception,
        get_response: _FakeResponse | Exception | None = None,
    ) -> None:
        self.post_response = post_response
        self.get_response = get_response
        self.last_post_kwargs = {}
        self.last_get_kwargs = {}

    def post(self, *args, **kwargs):
        self.last_post_kwargs = kwargs
        if isinstance(self.post_response, Exception):
            raise self.post_response
        return self.post_response

    def get(self, *args, **kwargs):
        self.last_get_kwargs = kwargs
        if isinstance(self.get_response, Exception):
            raise self.get_response
        return self.get_response


class DoubaoImageServiceTests(unittest.TestCase):
    def test_build_request_body_contains_required_defaults(self) -> None:
        service = DoubaoImageService()

        body = service.build_request_body("线条图", "2k", True)

        self.assertEqual(service.endpoint, "https://ark.cn-beijing.volces.com/api/v3/images/generations")
        self.assertEqual(body["model"], "doubao-seedream-5-0-260128")
        self.assertEqual(body["prompt"], "线条图")
        self.assertEqual(body["sequential_image_generation"], "disabled")
        self.assertEqual(body["response_format"], "url")
        self.assertEqual(body["size"], "2k")
        self.assertFalse(body["stream"])
        self.assertTrue(body["watermark"])

    def test_parse_openai_style_image_url(self) -> None:
        payload = {"data": [{"url": "https://example.com/generated.png"}]}

        url = DoubaoImageService.parse_image_url(payload)

        self.assertEqual(url, "https://example.com/generated.png")

    def test_parse_top_level_image_url(self) -> None:
        url = DoubaoImageService.parse_image_url({"url": "https://example.com/top.png"})

        self.assertEqual(url, "https://example.com/top.png")

    def test_parse_b64_json_reports_clear_unsupported_error(self) -> None:
        with self.assertRaisesRegex(DoubaoImageError, "base64.*只支持 URL"):
            DoubaoImageService.parse_image_url({"data": [{"b64_json": "abcd"}]})

    def test_parse_error_message_payload(self) -> None:
        with self.assertRaisesRegex(DoubaoImageError, "service rejected"):
            DoubaoImageService.parse_image_url({"error": {"message": "service rejected"}})

    def test_parse_message_payload(self) -> None:
        with self.assertRaisesRegex(DoubaoImageError, "plain message"):
            DoubaoImageService.parse_image_url({"message": "plain message"})

    def test_generate_image_uses_mock_success_response(self) -> None:
        session = _FakeSession(
            _FakeResponse(200, {"data": [{"url": "https://example.com/generated.png"}]})
        )
        service = DoubaoImageService(session=session)

        with patch.dict("os.environ", {"ARK_API_KEY": "test-key"}):
            url = service.generate_image("线条图", "2k", True)

        self.assertEqual(url, "https://example.com/generated.png")
        self.assertEqual(session.last_post_kwargs["json"]["size"], "2k")
        self.assertTrue(session.last_post_kwargs["json"]["watermark"])
        self.assertIn("Authorization", session.last_post_kwargs["headers"])

    def test_parse_missing_image_url_reports_error(self) -> None:
        with self.assertRaisesRegex(DoubaoImageError, "未找到图片 URL"):
            DoubaoImageService.parse_image_url({"data": [{}]})

    def test_generate_image_uses_mock_error_response(self) -> None:
        session = _FakeSession(_FakeResponse(401, {"error": {"message": "bad token"}}))
        service = DoubaoImageService(session=session)

        with patch.dict("os.environ", {"ARK_API_KEY": "test-key"}):
            with self.assertRaisesRegex(DoubaoImageError, "HTTP 401.*ARK_API_KEY.*bad token"):
                service.generate_image("线条图", "2k", True)

    def test_generate_image_requires_ark_api_key(self) -> None:
        service = DoubaoImageService(session=_FakeSession(_FakeResponse(200)))

        with patch.dict("os.environ", {}, clear=True):
            with self.assertRaisesRegex(DoubaoImageError, "ARK_API_KEY"):
                service.generate_image("线条图")

    def test_generate_image_timeout_reports_user_message(self) -> None:
        service = DoubaoImageService(session=_FakeSession(requests.Timeout()))

        with patch.dict("os.environ", {"ARK_API_KEY": "test-key"}):
            with self.assertRaisesRegex(DoubaoImageError, "超时"):
                service.generate_image("线条图")

    def test_401_error_message_mentions_key_or_permission(self) -> None:
        response = _FakeResponse(
            401,
            {"error": {"message": "unauthorized"}},
        )

        message = DoubaoImageService._format_http_error(response)

        self.assertIn("HTTP 401", message)
        self.assertIn("ARK_API_KEY", message)
        self.assertIn("unauthorized", message)

    def test_404_error_message_mentions_region_and_model(self) -> None:
        response = _FakeResponse(404, {"message": "not found"})

        message = DoubaoImageService._format_http_error(response)

        self.assertIn("HTTP 404", message)
        self.assertIn("cn-beijing", message)
        self.assertIn("not found", message)

    def test_download_rejects_non_image_content_type(self) -> None:
        service = DoubaoImageService(
            session=_FakeSession(
                _FakeResponse(200),
                _FakeResponse(200, headers={"Content-Type": "application/json"}, content=b"{}"),
            )
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            with self.assertRaisesRegex(DoubaoImageError, "不是 image/\\*"):
                service.download_image("https://example.com/not-image", Path(tmpdir) / "bad.png")

    def test_download_creates_missing_output_dir(self) -> None:
        image_bytes = b"\x89PNG\r\n\x1a\n"
        service = DoubaoImageService(
            session=_FakeSession(
                _FakeResponse(200),
                _FakeResponse(200, headers={"Content-Type": "image/png"}, content=image_bytes),
            )
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            output_path = Path(tmpdir) / "new" / "nested" / "image.png"
            saved = service.download_image("https://example.com/image.png", output_path)

            self.assertEqual(saved, output_path)
            self.assertEqual(output_path.read_bytes(), image_bytes)


if __name__ == "__main__":
    unittest.main()
