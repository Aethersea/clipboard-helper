"""Clipboard history management."""

import json
from datetime import datetime
from pathlib import Path
from typing import List, Optional

DEFAULT_HISTORY_FILE = Path.home() / ".clipboard_helper" / "history.json"
DEFAULT_MAX_ENTRIES = 50


class History:
    """Manages a local clipboard history stored as JSON."""

    def __init__(
        self,
        history_file: Optional[Path] = None,
        max_entries: int = DEFAULT_MAX_ENTRIES,
    ) -> None:
        self.history_file = history_file or DEFAULT_HISTORY_FILE
        self.max_entries = max_entries

    def load(self) -> List[dict]:
        """Load history entries from disk.

        Returns:
            A list of history entry dicts ordered newest-first.
        """
        if not self.history_file.exists():
            return []
        try:
            with open(self.history_file, encoding="utf-8") as fh:
                data = json.load(fh)
                if isinstance(data, list):
                    return data
        except (json.JSONDecodeError, OSError):
            pass
        return []

    def save(self, entries: List[dict]) -> None:
        """Persist history entries to disk.

        Args:
            entries: List of entry dicts to save.
        """
        self.history_file.parent.mkdir(parents=True, exist_ok=True)
        with open(self.history_file, "w", encoding="utf-8") as fh:
            json.dump(entries, fh, indent=2, ensure_ascii=False)

    def add(self, text: str) -> None:
        """Add a new text entry to history, deduplicating and trimming.

        Args:
            text: Clipboard text to record.
        """
        if not text:
            return
        entries = self.load()
        # Remove any pre-existing duplicate entry
        entries = [e for e in entries if e.get("text") != text]
        entries.insert(
            0,
            {
                "text": text,
                "timestamp": datetime.now().isoformat(),
            },
        )
        entries = entries[: self.max_entries]
        self.save(entries)

    def clear(self) -> None:
        """Remove all history entries."""
        self.save([])

    def remove(self, index: int) -> bool:
        """Remove a single entry by its 0-based index.

        Args:
            index: 0-based position in the history list.

        Returns:
            True if an entry was removed, False if index was out of range.
        """
        entries = self.load()
        if index < 0 or index >= len(entries):
            return False
        del entries[index]
        self.save(entries)
        return True
