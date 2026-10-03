#!/usr/bin/env python3
"""Three source checks whose failures are silent, confusing, or Linux-only.

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

  3. Apple-isms in code Android also compiles. Apple's arm64 deviates from
     AAPCS64 in ways that make iOS a poor proxy for Android: va_list is a
     plain char* there but a struct on bionic, and jmp_buf is int[] rather
     than long[]. Decompiled code that spells either type out by hand builds
     clean on iOS and fails on Android. The same goes for libc and pthread
     entry points that only Apple ships. Checked outside __APPLE__ guards
     only, so the iOS port keeps using them freely.

Run from the repository root; exits non-zero on the first category that
fails, printing every instance.
"""

from __future__ import annotations

import os
import re
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


# Functions and types bionic does not provide, that are easy to reach for
# when the reference implementation is Apple's.
APPLE_ONLY_SYMBOLS = {
    "pthread_mach_thread_np": "use pthread_gettid_np on Android",
    "mach_absolute_time": "use clock_gettime(CLOCK_MONOTONIC)",
    "mach_timebase_info": "use clock_gettime(CLOCK_MONOTONIC)",
    "mach_task_self": "no bionic equivalent",
    "host_statistics": "no bionic equivalent",
    "sysctlbyname": "read /proc or use __system_property_get",
    "_NSGetExecutablePath": "read /proc/self/exe",
    "malloc_size": "use malloc_usable_size",
    "fgetln": "use getline",
    "memset_s": "use explicit_bzero or a volatile loop",
    "OSAtomicAdd32": "use std::atomic",
    "dispatch_async": "no libdispatch on Android",
    "dispatch_queue_create": "no libdispatch on Android",
}

# Parameter names that, when typed char*, are really a va_list smuggled
# through Apple's ABI.
VA_LIST_PARAM_NAMES = r"(?:va|vargs|varg|ap|argptr|args|arglist|va_args|valist)"

VA_LIST_AS_CHAR = re.compile(
    r"\bchar\s*\*\s*(?:const\s+)?" + VA_LIST_PARAM_NAMES + r"\s*(?=[,)])"
)

# longjmp/setjmp handed a hand-spelled element type instead of a jmp_buf.
JMP_BUF_CAST = re.compile(r"\b(?:set|long|_setjmp|siglongjmp|sigsetjmp)jmp\s*\(\s*\(\s*(?:unsigned\s+)?(?:int|long|char|void|short)\b[^)]*\*")

APPLE_SYMBOL_RE = re.compile(r"\b(" + "|".join(sorted(APPLE_ONLY_SYMBOLS)) + r")\s*\(")

# Trees Android compiles. ports/ios is excluded wholesale: most of it is
# Apple-only by construction, and the handful of files Android reuses are
# already covered by the engine sweep.
PORTABILITY_TREES = ["src", "ports/android"]

# Groups the Android build never adds to a source list.
PORTABILITY_SKIP_DIRS = {"src/radiant", "src/groupvoice", "src/win32"}


def _condition_reaches_android(word: str, condition: str) -> bool | None:
    """Whether Android compiles the branch guarded by this condition.

    Returns True (Android reaches it), False (Android does not), or None
    when the condition says nothing about the platform.
    """
    names_apple = "__APPLE__" in condition or "TARGET_OS_" in condition
    names_android = "__ANDROID__" in condition
    if not names_apple and not names_android:
        return None
    negated = word == "ifndef" or "!" in condition
    if names_android:
        return not negated
    return negated  # names Apple: Android reaches it only when negated


def _apple_guarded_lines(text: str) -> set[int]:
    """Line numbers inside a preprocessor region Android never compiles.

    Tracks #if nesting, flipping the verdict at #else/#elif, so the Apple
    half of an "#ifdef __ANDROID__ ... #else ... #endif" pair counts as
    guarded. Deliberately crude: it only has to be right about whether
    Android reaches the line, and erring towards "guarded" just keeps the
    check quiet.
    """
    guarded: set[int] = set()
    # Each entry: (verdict for the current branch, verdict of the #if).
    stack: list[tuple[bool | None, bool | None]] = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("#"):
            directive = stripped[1:].lstrip()
            word = directive.split(None, 1)[0] if directive else ""
            condition = directive[len(word):]
            if word in ("if", "ifdef", "ifndef"):
                verdict = _condition_reaches_android(word, condition)
                stack.append((verdict, verdict))
                continue
            if word in ("elif", "else") and stack:
                _, opening = stack[-1]
                if word == "else":
                    # The else is reached exactly when the #if was not.
                    verdict = None if opening is None else not opening
                else:
                    verdict = _condition_reaches_android("if", condition)
                    if verdict is None and opening is not None:
                        verdict = not opening
                stack[-1] = (verdict, opening)
                continue
            if word == "endif" and stack:
                stack.pop()
                continue
        if any(branch is False for branch, _ in stack):
            guarded.add(number)
    return guarded


def check_portability() -> list[str]:
    problems: list[str] = []
    for tree in PORTABILITY_TREES:
        for root, dirs, files in os.walk(tree):
            dirs[:] = sorted(d for d in dirs if os.path.join(root, d).replace("\\", "/") not in PORTABILITY_SKIP_DIRS)
            if root.replace("\\", "/") in PORTABILITY_SKIP_DIRS:
                continue
            for name in sorted(files):
                if not name.endswith((".c", ".cpp", ".h", ".hpp", ".inc")):
                    continue
                path = os.path.join(root, name)
                try:
                    text = open(path, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                guarded = _apple_guarded_lines(text)
                for number, line in enumerate(text.splitlines(), 1):
                    if number in guarded:
                        continue
                    code = line.split("//", 1)[0]
                    if VA_LIST_AS_CHAR.search(code):
                        problems.append(
                            f"{path}:{number}: va_list parameter typed char* "
                            f"(char* only on Apple; a struct on bionic)"
                        )
                    if JMP_BUF_CAST.search(code):
                        problems.append(
                            f"{path}:{number}: jmp_buf cast to a spelled-out element type "
                            f"(int[] on Apple, long[] on bionic) - dereference a jmp_buf* instead"
                        )
                    match = APPLE_SYMBOL_RE.search(code)
                    if match:
                        symbol = match.group(1)
                        problems.append(
                            f"{path}:{number}: {symbol}() is Apple-only - {APPLE_ONLY_SYMBOLS[symbol]}"
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

    problems = check_portability()
    if problems:
        failed = True
        print(f"Apple-only constructs reachable from the Android build ({len(problems)}):")
        for problem in problems:
            print(f"  {problem}")
    else:
        print("Apple-isms outside __APPLE__ guards: ok")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
