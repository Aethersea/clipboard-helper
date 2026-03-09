"""Core clipboard operations for macOS."""

import subprocess


def get_clipboard() -> str:
    """Read the current content of the macOS clipboard.

    Returns:
        The current clipboard content as a string.

    Raises:
        RuntimeError: If pbpaste fails or is unavailable.
    """
    try:
        result = subprocess.run(
            ["pbpaste"],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout
    except FileNotFoundError:
        raise RuntimeError(
            "pbpaste not found. This tool requires macOS."
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(f"pbpaste failed: {exc.stderr}") from exc


def set_clipboard(text: str) -> None:
    """Write text to the macOS clipboard.

    Args:
        text: The text to place on the clipboard.

    Raises:
        RuntimeError: If pbcopy fails or is unavailable.
    """
    try:
        subprocess.run(
            ["pbcopy"],
            input=text,
            text=True,
            capture_output=True,
            check=True,
        )
    except FileNotFoundError:
        raise RuntimeError(
            "pbcopy not found. This tool requires macOS."
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(f"pbcopy failed: {exc.stderr}") from exc


def clear_clipboard() -> None:
    """Clear the macOS clipboard.

    Raises:
        RuntimeError: If pbcopy fails or is unavailable.
    """
    set_clipboard("")
