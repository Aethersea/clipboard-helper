"""Tests for clipboard_helper.cli."""

from unittest.mock import patch

import pytest

from clipboard_helper.cli import run
from clipboard_helper.history import History


@pytest.fixture()
def history_file(tmp_path):
    return tmp_path / "history.json"


@pytest.fixture(autouse=True)
def patch_history(tmp_path):
    """Redirect History to a temp file for all CLI tests."""
    with patch(
        "clipboard_helper.cli.History",
        return_value=History(history_file=tmp_path / "history.json"),
    ):
        yield


class TestPasteCommand:
    def test_prints_clipboard(self, capsys):
        with patch("clipboard_helper.cli.get_clipboard", return_value="hello"):
            code = run(["paste"])
        assert code == 0
        captured = capsys.readouterr()
        assert "hello" in captured.out

    def test_appends_newline_if_missing(self, capsys):
        with patch("clipboard_helper.cli.get_clipboard", return_value="no newline"):
            run(["paste"])
        captured = capsys.readouterr()
        assert captured.out.endswith("\n")

    def test_does_not_double_newline(self, capsys):
        with patch("clipboard_helper.cli.get_clipboard", return_value="ends\n"):
            run(["paste"])
        captured = capsys.readouterr()
        assert captured.out == "ends\n"


class TestCopyCommand:
    def test_copies_inline_text(self):
        with patch("clipboard_helper.cli.set_clipboard") as mock_set:
            code = run(["copy", "my text"])
        assert code == 0
        mock_set.assert_called_once_with("my text")

    def test_copies_stdin_when_no_text(self):
        with patch("clipboard_helper.cli.set_clipboard") as mock_set, patch(
            "sys.stdin"
        ) as mock_stdin:
            mock_stdin.read.return_value = "from stdin"
            code = run(["copy"])
        assert code == 0
        mock_set.assert_called_once_with("from stdin")

    def test_adds_to_history(self, tmp_path):
        hist = History(history_file=tmp_path / "h.json")
        with patch("clipboard_helper.cli.History", return_value=hist), patch(
            "clipboard_helper.cli.set_clipboard"
        ):
            run(["copy", "track me"])
        assert hist.load()[0]["text"] == "track me"


class TestClearCommand:
    def test_clears_clipboard(self):
        with patch("clipboard_helper.cli.clear_clipboard") as mock_clear:
            code = run(["clear"])
        assert code == 0
        mock_clear.assert_called_once()


class TestHistoryCommand:
    def _make_hist(self, tmp_path, entries):
        hist = History(history_file=tmp_path / "h.json")
        for e in reversed(entries):
            hist.add(e)
        return hist

    def test_history_list_empty(self, capsys, tmp_path):
        hist = History(history_file=tmp_path / "h.json")
        with patch("clipboard_helper.cli.History", return_value=hist):
            code = run(["history", "list"])
        assert code == 0
        assert "No history" in capsys.readouterr().out

    def test_history_list_shows_entries(self, capsys, tmp_path):
        hist = self._make_hist(tmp_path, ["alpha", "beta"])
        with patch("clipboard_helper.cli.History", return_value=hist):
            code = run(["history", "list"])
        out = capsys.readouterr().out
        assert code == 0
        assert "alpha" in out
        assert "beta" in out

    def test_history_list_default_shows_history(self, capsys, tmp_path):
        hist = self._make_hist(tmp_path, ["item"])
        with patch("clipboard_helper.cli.History", return_value=hist):
            code = run(["history"])
        assert code == 0

    def test_history_get_restores_to_clipboard(self, tmp_path):
        hist = self._make_hist(tmp_path, ["restore me"])
        with patch("clipboard_helper.cli.History", return_value=hist), patch(
            "clipboard_helper.cli.set_clipboard"
        ) as mock_set:
            code = run(["history", "get", "0"])
        assert code == 0
        mock_set.assert_called_once_with("restore me")

    def test_history_get_out_of_range(self, capsys, tmp_path):
        hist = self._make_hist(tmp_path, ["only"])
        with patch("clipboard_helper.cli.History", return_value=hist):
            code = run(["history", "get", "99"])
        assert code == 1
        assert "out of range" in capsys.readouterr().err

    def test_history_delete_removes_entry(self, capsys, tmp_path):
        hist = self._make_hist(tmp_path, ["keep", "delete me"])
        with patch("clipboard_helper.cli.History", return_value=hist):
            code = run(["history", "delete", "0"])
        assert code == 0
        assert len(hist.load()) == 1

    def test_history_clear(self, capsys, tmp_path):
        hist = self._make_hist(tmp_path, ["a", "b"])
        with patch("clipboard_helper.cli.History", return_value=hist):
            code = run(["history", "clear"])
        assert code == 0
        assert hist.load() == []
        assert "cleared" in capsys.readouterr().out


class TestNoCommand:
    def test_no_command_prints_help(self, capsys):
        code = run([])
        assert code == 0
        out = capsys.readouterr().out
        assert "usage" in out.lower() or "clipboard-helper" in out.lower()
