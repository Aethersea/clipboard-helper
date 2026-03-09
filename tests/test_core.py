"""Tests for clipboard_helper.core."""

import subprocess
from unittest.mock import MagicMock, patch

import pytest

from clipboard_helper.core import clear_clipboard, get_clipboard, set_clipboard


class TestGetClipboard:
    def test_returns_clipboard_text(self):
        mock_result = MagicMock()
        mock_result.stdout = "hello world"
        with patch("subprocess.run", return_value=mock_result) as mock_run:
            text = get_clipboard()
        mock_run.assert_called_once_with(
            ["pbpaste"], capture_output=True, text=True, check=True
        )
        assert text == "hello world"

    def test_raises_when_pbpaste_missing(self):
        with patch("subprocess.run", side_effect=FileNotFoundError):
            with pytest.raises(RuntimeError, match="pbpaste not found"):
                get_clipboard()

    def test_raises_on_process_error(self):
        with patch(
            "subprocess.run",
            side_effect=subprocess.CalledProcessError(1, "pbpaste", stderr="oops"),
        ):
            with pytest.raises(RuntimeError, match="pbpaste failed"):
                get_clipboard()


class TestSetClipboard:
    def test_calls_pbcopy_with_text(self):
        with patch("subprocess.run") as mock_run:
            set_clipboard("some text")
        mock_run.assert_called_once_with(
            ["pbcopy"],
            input="some text",
            text=True,
            capture_output=True,
            check=True,
        )

    def test_raises_when_pbcopy_missing(self):
        with patch("subprocess.run", side_effect=FileNotFoundError):
            with pytest.raises(RuntimeError, match="pbcopy not found"):
                set_clipboard("text")

    def test_raises_on_process_error(self):
        with patch(
            "subprocess.run",
            side_effect=subprocess.CalledProcessError(1, "pbcopy", stderr="fail"),
        ):
            with pytest.raises(RuntimeError, match="pbcopy failed"):
                set_clipboard("text")


class TestClearClipboard:
    def test_clears_by_sending_empty_string(self):
        with patch("clipboard_helper.core.set_clipboard") as mock_set:
            clear_clipboard()
        mock_set.assert_called_once_with("")
