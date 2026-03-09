# clipboard-helper

一個 macOS 本地剪貼簿管理工具 (A macOS local clipboard management tool).

## Features

- **paste** – Print the current clipboard content
- **copy** – Copy text (inline or from stdin) to the clipboard
- **clear** – Clear the clipboard
- **history** – Browse, restore and manage clipboard history stored locally

## Requirements

- macOS (relies on `pbcopy` / `pbpaste`)
- Python 3.8+

## Installation

```bash
pip install .
```

Or run directly without installing:

```bash
python -m clipboard_helper <command>
```

## Usage

```
clipboard-helper <command> [options]
```

### Commands

| Command | Description |
|---------|-------------|
| `paste` | Print the current clipboard content |
| `copy [TEXT]` | Copy TEXT to the clipboard (reads stdin if TEXT is omitted) |
| `clear` | Clear the clipboard |
| `history list [-n N]` | List the last N history entries (default 10) |
| `history get INDEX` | Restore a history entry to the clipboard |
| `history delete INDEX` | Delete a single history entry |
| `history clear` | Clear all clipboard history |

### Examples

```bash
# Copy text to clipboard
clipboard-helper copy "Hello, world!"

# Copy from stdin
echo "from stdin" | clipboard-helper copy

# Print clipboard content
clipboard-helper paste

# Clear clipboard
clipboard-helper clear

# Show last 5 history entries
clipboard-helper history list -n 5

# Restore entry 2 from history to the clipboard
clipboard-helper history get 2

# Delete entry 0 from history
clipboard-helper history delete 0

# Clear all history
clipboard-helper history clear
```

## History Storage

Clipboard history is saved to `~/.clipboard_helper/history.json` (up to 50 entries by default). Duplicate entries are automatically collapsed.

## Development

```bash
# Install in editable mode
pip install -e .

# Run tests
pytest

# Lint
ruff check .
```