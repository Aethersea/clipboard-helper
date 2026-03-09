"""Tests for clipboard_helper.history."""

import json

import pytest

from clipboard_helper.history import History


@pytest.fixture()
def history(tmp_path):
    return History(history_file=tmp_path / "history.json", max_entries=5)


class TestHistoryLoad:
    def test_empty_when_file_missing(self, history):
        assert history.load() == []

    def test_loads_existing_entries(self, history):
        history.history_file.write_text(
            json.dumps([{"text": "hi", "timestamp": "2024-01-01T00:00:00"}]),
            encoding="utf-8",
        )
        entries = history.load()
        assert len(entries) == 1
        assert entries[0]["text"] == "hi"

    def test_returns_empty_on_corrupt_file(self, history):
        history.history_file.parent.mkdir(parents=True, exist_ok=True)
        history.history_file.write_text("not json", encoding="utf-8")
        assert history.load() == []


class TestHistoryAdd:
    def test_adds_entry(self, history):
        history.add("hello")
        entries = history.load()
        assert len(entries) == 1
        assert entries[0]["text"] == "hello"

    def test_deduplicates(self, history):
        history.add("hello")
        history.add("world")
        history.add("hello")
        entries = history.load()
        assert len(entries) == 2
        assert entries[0]["text"] == "hello"

    def test_respects_max_entries(self, history):
        for i in range(10):
            history.add(f"entry {i}")
        assert len(history.load()) == 5

    def test_ignores_empty_text(self, history):
        history.add("")
        assert history.load() == []

    def test_newest_first(self, history):
        history.add("first")
        history.add("second")
        entries = history.load()
        assert entries[0]["text"] == "second"
        assert entries[1]["text"] == "first"

    def test_entry_has_timestamp(self, history):
        history.add("timestamped")
        entry = history.load()[0]
        assert "timestamp" in entry
        assert entry["timestamp"]


class TestHistoryClear:
    def test_clears_all_entries(self, history):
        history.add("a")
        history.add("b")
        history.clear()
        assert history.load() == []


class TestHistoryRemove:
    def test_removes_entry_by_index(self, history):
        history.add("a")
        history.add("b")
        history.add("c")
        removed = history.remove(0)
        assert removed is True
        entries = history.load()
        assert len(entries) == 2
        assert entries[0]["text"] == "b"

    def test_returns_false_for_out_of_range(self, history):
        history.add("only")
        assert history.remove(5) is False

    def test_returns_false_when_empty(self, history):
        assert history.remove(0) is False
