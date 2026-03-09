"""Command-line interface for clipboard-helper."""

import argparse
import sys

from .core import clear_clipboard, get_clipboard, set_clipboard
from .history import History


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="clipboard-helper",
        description="A macOS local clipboard management tool.",
    )
    sub = parser.add_subparsers(dest="command", metavar="COMMAND")

    # paste – read clipboard
    sub.add_parser("paste", help="Print the current clipboard content.")

    # copy – write to clipboard
    copy_p = sub.add_parser("copy", help="Copy text to the clipboard.")
    copy_p.add_argument("text", nargs="?", help="Text to copy. Reads from stdin if omitted.")

    # clear – empty clipboard
    sub.add_parser("clear", help="Clear the clipboard.")

    # history list
    history_p = sub.add_parser("history", help="Manage clipboard history.")
    history_sub = history_p.add_subparsers(dest="history_command", metavar="ACTION")

    h_list = history_sub.add_parser("list", help="List clipboard history.")
    h_list.add_argument(
        "-n",
        "--limit",
        type=int,
        default=10,
        metavar="N",
        help="Maximum number of entries to show (default: 10).",
    )

    h_get = history_sub.add_parser("get", help="Restore an entry from history to the clipboard.")
    h_get.add_argument("index", type=int, help="0-based index of the entry to restore.")

    h_del = history_sub.add_parser("delete", help="Delete a single history entry.")
    h_del.add_argument("index", type=int, help="0-based index of the entry to delete.")

    history_sub.add_parser("clear", help="Clear all clipboard history.")

    return parser


def run(argv=None) -> int:
    """Entry point. Returns exit code."""
    parser = build_parser()
    args = parser.parse_args(argv)

    history = History()

    if args.command == "paste":
        text = get_clipboard()
        sys.stdout.write(text)
        if text and not text.endswith("\n"):
            sys.stdout.write("\n")
        return 0

    if args.command == "copy":
        if args.text is not None:
            text = args.text
        else:
            text = sys.stdin.read()
        set_clipboard(text)
        history.add(text)
        return 0

    if args.command == "clear":
        clear_clipboard()
        return 0

    if args.command == "history":
        hcmd = args.history_command

        if hcmd == "list" or hcmd is None:
            limit = getattr(args, "limit", 10)
            entries = history.load()[:limit]
            if not entries:
                print("No history.")
                return 0
            for i, entry in enumerate(entries):
                ts = entry.get("timestamp", "")
                text = entry.get("text", "")
                preview = text.replace("\n", "↵")
                if len(preview) > 60:
                    preview = preview[:57] + "..."
                print(f"[{i}] {ts[:19]}  {preview}")
            return 0

        if hcmd == "get":
            entries = history.load()
            if args.index < 0 or args.index >= len(entries):
                print(f"Error: index {args.index} is out of range.", file=sys.stderr)
                return 1
            text = entries[args.index]["text"]
            set_clipboard(text)
            print(f"Restored entry [{args.index}] to clipboard.")
            return 0

        if hcmd == "delete":
            removed = history.remove(args.index)
            if not removed:
                print(f"Error: index {args.index} is out of range.", file=sys.stderr)
                return 1
            print(f"Deleted entry [{args.index}].")
            return 0

        if hcmd == "clear":
            history.clear()
            print("History cleared.")
            return 0

        # Unknown history sub-command
        parser.parse_args(["history", "--help"])
        return 1

    # No command given
    parser.print_help()
    return 0


def main() -> None:
    sys.exit(run())
