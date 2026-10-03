#!/usr/bin/env python3
"""Two source checks whose failures are silent, confusing, or Linux-only.

Both of these cost a full CI run once already, and neither is visible to a
developer working on macOS, so they are cheap to re-break:

  1. Case-only #include mismatches. macOS volumes are case-insensitive by
     default, so <Windows.h> happily resolves to windows.h in the Win32
     compat layer. The Android build cross-compiles on Linux, where it does
     not, and the error arrives tens of minutes into the native build.

  2. Unbalanced Kotlin block comments. Kotlin block comments NEST, unlike
     C's, so a stray "/*" inside a doc comment - "main/*.iwd" in a path
     listing, say - opens a second level that the closing "*/" only half
     closes. The rest of the file is swallowed and the compiler reports an
     unclosed comment at EOF plus a cascade of unresolved references
     pointing anywhere but the real line.

Run from the repository root; exits non-zero on the first category that
fails, printing every instance.
"""

from __future__ import annotations

import os
import sys

# Mirrors KISAK_ANDROID_INCLUDES in ports/android/CMakeLists.txt. Only the
# roots matter here, not their order.
INCLUDE_ROOTS = [
    "src",
    "deps",
    "ports/android/gfx/compat",
    "ports/android/gfx",
    "ports/ios/engine",
    "ports/ios/compat/native/windows",
    "ports/ios/compat/native/directx",
]

SOURCE_TREES = ["src", "ports/android", "ports/ios", "deps/binklib"]
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inc")


def parse_include(line: str) -> tuple[str, str] | None:
    """Returns (delimiter, path) for an #include line, else None."""
    s = line.strip()
    if not s.startswith("#"):
        return None
    s = s[1:].strip()
    if not s.startswith("include"):
        return None
    s = s[len("include"):].strip()
    if s[:1] == "<":
        end = s.find(">")
        return ("<", s[1:end].replace("\\", "/")) if end > 0 else None
    if s[:1] == '"':
        end = s.find('"', 1)
        return ('"', s[1:end].replace("\\", "/")) if end > 0 else None
    return None


def check_include_case() -> list[str]:
    # Case-insensitive index of what is actually on disk, per include root.
    index: dict[str, dict[str, str]] = {}
    for root in INCLUDE_ROOTS:
        entries: dict[str, str] = {}
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                rel = os.path.relpath(os.path.join(dirpath, name), root)
                entries[rel.lower().replace("\\", "/")] = rel.replace("\\", "/")
        index[root] = entries

    problems: list[str] = []
    for tree in SOURCE_TREES:
        for dirpath, _dirs, files in os.walk(tree):
            if "tests" in dirpath.split(os.sep):
                continue
            siblings = {name.lower(): name for name in files}
            for name in files:
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, encoding="utf-8", errors="ignore") as handle:
                    for number, line in enumerate(handle, 1):
                        parsed = parse_include(line)
                        if not parsed:
                            continue
                        delim, want = parsed
                        # A quoted include resolves next to the including file first.
                        if delim == '"':
                            if os.path.exists(os.path.normpath(os.path.join(dirpath, want))):
                                continue
                            if "/" not in want and want.lower() in siblings:
                                problems.append(
                                    f"{path}:{number}: includes '{want}', "
                                    f"the file is '{siblings[want.lower()]}'"
                                )
                                continue
                        if any(os.path.exists(os.path.join(r, want)) for r in INCLUDE_ROOTS):
                            continue
                        for root in INCLUDE_ROOTS:
                            actual = index[root].get(want.lower())
                            if actual:
                                problems.append(
                                    f"{path}:{number}: includes '{want}', "
                                    f"the file is '{root}/{actual}'"
                                )
                                break
    return problems


def check_kotlin_comments(tree: str = "ports/android/launcher") -> list[str]:
    problems: list[str] = []
    for dirpath, _dirs, files in os.walk(tree):
        for name in files:
            if not name.endswith(".kt"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="utf-8") as handle:
                text = handle.read()

            i = 0
            line = 1
            depth = 0
            opened_at: list[int] = []
            while i < len(text):
                pair = text[i:i + 2]
                if text[i] == "\n":
                    line += 1
                    i += 1
                    continue
                if depth > 0:
                    # Inside a comment only the nesting delimiters matter.
                    if pair == "/*":
                        depth += 1
                        opened_at.append(line)
                        i += 2
                    elif pair == "*/":
                        depth -= 1
                        opened_at.pop()
                        i += 2
                    else:
                        i += 1
                    continue
                if pair == "/*":
                    depth += 1
                    opened_at.append(line)
                    i += 2
                elif pair == "//":
                    newline = text.find("\n", i)
                    i = newline if newline > 0 else len(text)
                elif text[i] == '"':
                    if text[i:i + 3] == '"""':
                        end = text.find('"""', i + 3)
                        i = end + 3 if end > 0 else len(text)
                    else:
                        i += 1
                        while i < len(text) and text[i] != '"':
                            if text[i] == "\\":
                                i += 1
                            if i < len(text) and text[i] == "\n":
                                line += 1
                            i += 1
                        i += 1
                else:
                    i += 1

            if depth:
                where = ", ".join(str(n) for n in opened_at)
                problems.append(
                    f"{path}: {depth} unclosed block comment(s), opened at line(s) {where}. "
                    f"Kotlin block comments nest, so a bare '/*' inside one - in a path "
                    f"like main/*.iwd, for instance - opens another level."
                )
    return problems


def main() -> int:
    failed = False

    problems = check_include_case()
    if problems:
        failed = True
        print(f"Includes that only resolve on a case-insensitive filesystem ({len(problems)}):")
        for problem in problems:
            print(f"  {problem}")
    else:
        print("Include case: ok")

    problems = check_kotlin_comments()
    if problems:
        failed = True
        print(f"Kotlin files with unbalanced block comments ({len(problems)}):")
        for problem in problems:
            print(f"  {problem}")
    else:
        print("Kotlin block comments: ok")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
