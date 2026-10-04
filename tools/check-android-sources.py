#!/usr/bin/env python3
"""Source checks whose failures are silent, confusing, or Linux-only.

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

  4. Link-time breakage the compiler cannot see. Two shapes of it have
     reached CI already: a block-scope `extern` whose signature disagrees
     with the definition, and the Vulkan backend drifting out of step with
     the Metal interface the shared D3D9 layer is written against. Both
     compile cleanly in every translation unit and only fail at link,
     twenty minutes in.

  5. Backup rules lint rejects. An <exclude> whose path lies outside every
     <include> in its section is dead configuration, and lintVitalRelease
     treats it as a fatal error rather than a warning, so it fails the
     release build and nothing else.

  6. Definitions that only exist for Apple. A function the engine guards
     with #ifdef __APPLE__ still compiles everywhere - the guard removes
     the body, not the calls - so a shared translation unit referencing it
     links on iOS and nowhere else.

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


LONG_DOUBLE_RE = re.compile(r"\blong\s+double\b")


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


# Sources both engine libraries compile, mirroring KISAK_ANDROID_PORT_SOURCES
# in ports/android/CMakeLists.txt plus the gfx and perf object libraries.
PORT_SOURCE_DIRS = ["ports/android/engine", "ports/android/platform",
                    "ports/android/app", "ports/android/perf", "ports/android/gfx"]

# iOS translation units the Android libraries compile verbatim. A symbol
# defined only in a .mm file is not available to them.
SHARED_IOS_SOURCES = [
    "ports/ios/engine/controller_input.cpp",
    "ports/ios/engine/controller_icons.cpp",
    "ports/ios/engine/cinematic_apple.cpp",
    "ports/ios/engine/db_zoneload_apple.cpp",
    "ports/ios/compat/d3dx9shader_apple.cpp",
    "ports/ios/compat/steam_apple.cpp",
    "ports/ios/d3d9/d3d9_apple.cpp",
    "ports/ios/network/cod4x_transport.cpp",
]

# Linked in as libkisakcod_zoneload.a, so its definitions count too.
ZONELOAD_SOURCE_DIR = "ports/ios/zoneload/bridge"

BLOCK_SCOPE_EXTERN = re.compile(r"^\s+extern\s+(?!\"C\")[A-Za-z_][\w:<>\*&\s]*\s[A-Za-z_]\w*\s*\(")

PORT_SYMBOL = re.compile(r"\b(Kisak\w+|\w+_Apple)\s*\(")


def _strip_comments_and_strings(text: str) -> str:
    """Blank out anything a symbol must not be found inside.

    Keeps offsets intact so reported line numbers stay right.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif two == "/*":
            while i < n and text[i:i + 2] != "*/":
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            for j in range(i, min(i + 2, n)):
                out[j] = " "
            i += 2
        elif text[i] in "\"'":
            quote = text[i]
            out[i] = " "
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n and text[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
            i += 1
        else:
            i += 1
    return "".join(out)


def _scan_symbols(path: str) -> tuple[set[str], dict[str, int]]:
    """Port symbols defined in, and mentioned by, one file.

    A name is *defined* when the token after its closing parenthesis is an
    opening brace. Everything else - calls, prototypes, pointers taken - is
    a mention. Done by matching parentheses rather than by line, because
    both "extern \"C\" void F(...)" and a one-line "int F() { return x; }"
    defeat anything simpler.
    """
    try:
        text = _strip_comments_and_strings(open(path, encoding="utf-8", errors="replace").read())
    except OSError:
        return set(), {}
    defined: set[str] = set()
    mentioned: dict[str, int] = {}
    for match in PORT_SYMBOL.finditer(text):
        start = text.index("(", match.end() - 1)
        depth, i, n = 0, start, len(text)
        while i < n:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        rest = text[i + 1:i + 200].lstrip()
        # Skip trailing specifiers that may sit between ) and {.
        for word in ("const", "noexcept", "override", "final"):
            if rest.startswith(word):
                rest = rest[len(word):].lstrip()
        name = match.group(1)
        if rest.startswith("{"):
            defined.add(name)
        else:
            mentioned.setdefault(name, text.count("\n", 0, match.start()) + 1)
    return defined, mentioned


def _port_sources() -> list[str]:
    found = []
    for directory in PORT_SOURCE_DIRS:
        for root, _, files in os.walk(directory):
            for name in sorted(files):
                if name.endswith((".c", ".cpp")):
                    found.append(os.path.join(root, name))
    return sorted(found)


def check_block_scope_externs() -> list[str]:
    """Function declarations hidden inside a function body.

    These bypass the header that would have checked them against the
    definition. KisakAndroid_ShowRestartPrompt was declared this way with
    two parameters and defined with one; it linked nowhere.
    """
    problems = []
    for path in _port_sources() + SHARED_IOS_SOURCES:
        for number, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
            if BLOCK_SCOPE_EXTERN.match(line.split("//", 1)[0]):
                problems.append(
                    f"{path}:{number}: function declared extern at block scope; "
                    f"put it in a header so the signature is checked"
                )
    return problems


def check_backend_interfaces() -> list[str]:
    """kisak::vk must still cover everything kisak::metal declares.

    ports/ios/d3d9/d3d9_apple.cpp is compiled by both ports and binds its
    `gpu::` alias to one namespace or the other. Anything the Metal header
    declares and the Vulkan header does not is an undefined symbol in the
    Android libraries.
    """
    metal = "ports/ios/d3d9/metal/metal_backend.h"
    vulkan = "ports/android/gfx/gpu_backend.h"
    if not (os.path.exists(metal) and os.path.exists(vulkan)):
        return []

    def declarations(path: str) -> dict[str, str]:
        text = open(path, encoding="utf-8", errors="replace").read()
        text = re.sub(r"//[^\n]*", "", text)
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        found = {}
        for match in re.finditer(
            r"^\s*([A-Za-z_][\w:<>,\s\*&]*?[\s\*&])([A-Za-z_]\w*)\s*\(([^;{]*)\)\s*;",
            text, re.M,
        ):
            arguments = re.sub(r"\s+", " ", match.group(3)).strip()
            arguments = re.sub(r"\s*=\s*[^,]+", "", arguments)   # defaults are not ABI
            # Parameter names are not part of the signature either.
            arguments = ", ".join(
                re.sub(r"(?<=[\s\*&])[A-Za-z_]\w*$", "", a.strip()).strip()
                for a in arguments.split(",")
            )
            found[match.group(2)] = f"{match.group(1).strip()} ({arguments})"
        return found

    problems = []

    # The binding itself. d3d9_apple.cpp is compiled by both ports, so every
    # mention of the Metal backend in it has to sit in a region Android does
    # not reach - otherwise the Android libraries carry calls into
    # kisak::metal that nothing defines, which is exactly how this broke.
    shared = "ports/ios/d3d9/d3d9_apple.cpp"
    if os.path.exists(shared):
        text = open(shared, encoding="utf-8", errors="replace").read()
        guarded = _apple_guarded_lines(text)
        reaches_vulkan = False
        for number, line in enumerate(text.splitlines(), 1):
            code = line.split("//", 1)[0]
            if "kisak::vk" in code and number not in guarded:
                reaches_vulkan = True
            if number in guarded:
                continue
            if "kisak::metal" in code or "metal/metal_backend.h" in code:
                problems.append(
                    f"{shared}:{number}: the Metal backend is referenced outside an "
                    f"__APPLE__ guard; Android compiles this file too"
                )
        if not reaches_vulkan:
            problems.append(
                f"{shared}: nothing binds the Android build to kisak::vk"
            )

    declared_metal = declarations(metal)
    declared_vulkan = declarations(vulkan)
    for name in sorted(set(declared_metal) - set(declared_vulkan)):
        problems.append(f"{vulkan}: missing {name}(), which {metal} declares")
    for name in sorted(set(declared_metal) & set(declared_vulkan)):
        if declared_metal[name] != declared_vulkan[name]:
            problems.append(
                f"{vulkan}: {name} is {declared_vulkan[name]}, "
                f"but {metal} declares {declared_metal[name]}"
            )
    return problems


def _type_names() -> set[str]:
    """Names introduced by struct/class declarations.

    `new KisakVideoPlayer()` looks exactly like a call; it is not one.
    """
    names: set[str] = set()
    for path in _port_sources() + SHARED_IOS_SOURCES:
        text = open(path, encoding="utf-8", errors="replace").read()
        names |= set(re.findall(r"\b(?:struct|class)\s+([A-Za-z_]\w*)", text))
    for root, _, files in os.walk("ports/android"):
        for name in files:
            if name.endswith((".h", ".hpp")):
                text = open(os.path.join(root, name), encoding="utf-8", errors="replace").read()
                names |= set(re.findall(r"\b(?:struct|class)\s+([A-Za-z_]\w*)", text))
    return names


def check_port_symbols() -> list[str]:
    """Every KisakAndroid_/KisakApple_ name the port calls must be defined.

    Not a linker, and it does not try to be: it only reports names that
    nothing anywhere in the tree defines, which is cheap and catches
    typos and deleted functions before a twenty-minute CI build does.
    """
    defined: set[str] = _type_names()
    referenced: dict[str, str] = {}
    zoneload = [
        os.path.join(ZONELOAD_SOURCE_DIR, name)
        for name in sorted(os.listdir(ZONELOAD_SOURCE_DIR))
        if name.endswith((".c", ".cpp"))
    ] if os.path.isdir(ZONELOAD_SOURCE_DIR) else []
    for path in zoneload:
        defined |= _scan_symbols(path)[0]
    for path in _port_sources() + SHARED_IOS_SOURCES:
        file_defined, file_mentioned = _scan_symbols(path)
        defined |= file_defined
        for name, number in file_mentioned.items():
            referenced.setdefault(name, f"{path}:{number}")

    # The engine tree defines a few of these itself.
    for root, _, files in os.walk("src"):
        for name in files:
            if name.endswith((".c", ".cpp")):
                defined |= _scan_symbols(os.path.join(root, name))[0]

    return [
        f"{where}: {name}() is called but nothing defines it"
        for name, where in sorted(referenced.items())
        if name not in defined
    ]


BACKUP_RULES = [
    "ports/android/launcher/app/src/main/res/xml/data_extraction_rules.xml",
    "ports/android/launcher/app/src/main/res/xml/backup_rules.xml",
]


def check_backup_rules() -> list[str]:
    """Reimplements lint's FullBackupContent rule.

    An <include> narrows its section to the paths it names, so an <exclude>
    only means something when it sits under one of them. Lint reports the
    rest as fatal during lintVitalRelease, which is the one lint run that
    blocks a release build.
    """
    import xml.etree.ElementTree as ElementTree

    problems: list[str] = []
    for path in BACKUP_RULES:
        if not os.path.exists(path):
            continue
        try:
            root = ElementTree.parse(path).getroot()
        except ElementTree.ParseError as error:
            problems.append(f"{path}: not well-formed XML ({error})")
            continue

        # <full-backup-content> holds the rules directly; the Android 12
        # <data-extraction-rules> groups them per transfer mode.
        sections = [root] if root.tag == "full-backup-content" else list(root)
        for section in sections:
            includes = [
                (element.get("domain"), element.get("path", "."))
                for element in section.findall("include")
            ]
            for element in section.findall("exclude"):
                domain = element.get("domain")
                excluded = element.get("path", ".")
                covered = any(
                    domain == include_domain
                    and (include_path == "." or excluded.startswith(include_path))
                    for include_domain, include_path in includes
                )
                if includes and not covered:
                    problems.append(
                        f"{path}: <exclude domain=\"{domain}\" path=\"{excluded}\"> in "
                        f"<{section.tag}> is outside every <include>; lint fails the "
                        f"release build on this"
                    )
                elif any(
                    domain == include_domain and excluded == include_path
                    for include_domain, include_path in includes
                ):
                    problems.append(
                        f"{path}: <exclude domain=\"{domain}\" path=\"{excluded}\"> in "
                        f"<{section.tag}> contradicts an identical <include>"
                    )
    return problems


# Engine directories the Android libraries compile. Everything bar the
# Windows-only groups, which ports/android/CMakeLists.txt drops wholesale.
ENGINE_SKIP_DIRS = {"src/radiant", "src/groupvoice", "src/win32"}


def check_apple_only_definitions() -> list[str]:
    """Calls into function bodies that #ifdef __APPLE__ removes on Android.

    The guard takes out the definition, never the call, so every
    translation unit still compiles and the link fails instead.
    DB_AddXAsset_Apple reached CI this way: defined under __APPLE__ in
    src/database/db_registry.cpp, called from the zone loader both ports
    share.
    """
    guarded_definitions: dict[str, str] = {}
    for tree in ("src", "ports/ios"):
        for root, dirs, files in os.walk(tree):
            dirs[:] = [d for d in dirs
                       if os.path.join(root, d).replace("\\", "/") not in ENGINE_SKIP_DIRS]
            if root.replace("\\", "/") in ENGINE_SKIP_DIRS:
                continue
            for name in sorted(files):
                if not name.endswith((".c", ".cpp")):
                    continue
                path = os.path.join(root, name)
                text = open(path, encoding="utf-8", errors="replace").read()
                if "__APPLE__" not in text and "TARGET_OS_" not in text:
                    continue
                guarded = _apple_guarded_lines(text)
                if not guarded:
                    continue
                for match in re.finditer(
                    r"^[A-Za-z_][\w:<>,\*&\s]*?[\s\*&]([A-Za-z_]\w*)\s*\([^;{]*\)\s*(?:const\s*)?\{",
                    text, re.M,
                ):
                    line = text.count("\n", 0, match.start()) + 1
                    if line in guarded:
                        guarded_definitions.setdefault(match.group(1), f"{path}:{line}")

    problems = []
    for path in _port_sources() + SHARED_IOS_SOURCES:
        raw = open(path, encoding="utf-8", errors="replace").read()
        text = _strip_comments_and_strings(raw)
        guarded = _apple_guarded_lines(raw)
        defined_here, _ = _scan_symbols(path)
        for name, origin in guarded_definitions.items():
            if name in defined_here or origin.startswith(path + ":"):
                continue
            for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
                line = text.count("\n", 0, match.start()) + 1
                if line in guarded:
                    continue
                problems.append(
                    f"{path}:{line}: {name}() is only defined under __APPLE__ "
                    f"({origin}); Android compiles this call but not the body"
                )
                break
    return problems


def _android_shared_ios_sources() -> list[str]:
    """Files under ports/ios that ports/android/CMakeLists.txt also compiles."""
    try:
        text = open("ports/android/CMakeLists.txt", encoding="utf-8").read()
    except OSError:
        return []
    found = set()
    for match in re.finditer(r"\$\{IOS_DIR\}/([\w/.\-]+\.cpp)", text):
        path = os.path.join("ports/ios", match.group(1))
        if os.path.exists(path):
            found.add(path)
    return sorted(found)


def check_apple_gated_shared_calls() -> list[str]:
    """Engine calls to port functions the Android libraries already contain.

    The Android build compiles a set of files out of ports/ios, so the
    functions they define are linked into libkisakcod_*.so. When the engine
    calls one of those from inside an __APPLE__ guard, Android silently takes
    the other branch - the original 32-bit Windows code - and the symbol is
    present but never reached. Nothing fails at build time: the definition
    links, the call simply is not compiled.

    The zone loader failed exactly this way. db_zoneload_apple.cpp was in the
    Android source list, but DB_LoadXFileInternal only called
    DB_LoadXFileContent_Apple under __APPLE__, so Android kept loading zones
    with 32-bit structure layouts and zlib decompressed off the end of the
    block. The same guard had hidden KisakApple_ControllerMove, which left
    the touch stick and the gamepad unable to move the view.
    """
    shared: set[str] = set()
    for path in _android_shared_ios_sources():
        defined, _ = _scan_symbols(path)
        shared |= defined
    if not shared:
        return []

    problems: list[str] = []
    for root, _, files in os.walk("src"):
        for name in sorted(files):
            if not name.endswith((".c", ".cpp", ".h")):
                continue
            path = os.path.join(root, name)
            try:
                raw = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            if "__APPLE__" not in raw:
                continue
            guarded = _apple_guarded_lines(raw)
            if not guarded:
                continue
            text = _strip_comments_and_strings(raw)
            for match in PORT_SYMBOL.finditer(text):
                symbol = match.group(1)
                if symbol not in shared:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                if line in guarded:
                    problems.append(
                        f"{path}:{line}: {symbol}() is compiled into the Android "
                        "libraries, but this use is behind an __APPLE__ guard "
                        "that Android never reaches")
    return problems


# Every "#ifdef __APPLE__" in src/ is a fork where iOS takes the ported path
# and Android silently keeps the original 32-bit Windows one. That asymmetry
# caused the zone-loader crash, the dead controller look input and the sound
# crash, so the default is now the opposite: shared code is expected to say
# "defined(__APPLE__) || defined(__ANDROID__)".
#
# What is left below is the set of guards that must stay Apple-only, with the
# reason and the number of guard lines in that file. Adding a new Apple-only
# guard without a reason fails this check. Lowering a count is progress; just
# update the number in the same commit.
APPLE_ONLY_POLICY = {
    # Bound to an Apple framework. Android needs its own implementation, not
    # the same code.
    "src/qcommon/dl_main.cpp": (4, "NSURLSession download client (apple_download.mm); Android needs its own HTTP client"),
    "src/game/savedevice_pc.cpp": (2, "iOS NativeSaveFile; Android needs an equivalent save backend"),
    "src/client/screen_placement.cpp": (2, "KisakApple_GetDisplaySafeArea lives in apple_sys.cpp, which Android does not compile"),
    "src/universal/assertive.cpp": (2, "<execinfo.h>; bionic has no backtrace API, Android uses KisakAndroid_LogBacktrace"),
    "src/qcommon/common.cpp": (2, "<execinfo.h>, same as assertive.cpp"),
    "src/universal/timing.h": (1, "mach_absolute_time tick calibration"),
    "src/universal/q_shared.h": (1, "Apple CPUSTRING/MAC_STATIC block"),
    "src/universal/memfile.cpp": (1, "zlib header spelling; Android uses the vendored <zlib/zlib.h>"),
    "src/client/cl_main.cpp": (2, "apple_engine_mode.h and the dylib-based mode switch; Android has its own branch"),

    # Diagnostics that only ever ran on an Apple device. Harmless to leave off;
    # Android has its own logging.
    "src/gfx_d3d/r_image.cpp": (3, "iPhone texture-memory budget and Apple-only texture corruption diagnostics"),
    "src/gfx_d3d/r_rendercmds.cpp": (2, "Apple-only text corruption diagnostics"),
    "src/gfx_d3d/r_image_load_obj.cpp": (1, "Apple-only texture corruption diagnostics"),
    "src/script/scr_parser.cpp": (1, "KISAK_DUMP_SCRIPT local debugging aid"),
    "src/game/g_scr_main.cpp": (9, "Apple-side script debug counters and level-transition traces"),
    "src/game/actor.cpp": (2, "Apple-side AI debug counters"),
    "src/game/actor_animapi.cpp": (1, "Apple-side animation debug counter"),
    "src/game/g_utils.cpp": (1, "Apple-side DObj debug counter"),
    "src/game/g_main.cpp": (1, "Apple-side script registration counter"),
    "src/game/g_scr_vehicle.cpp": (2, "Apple-side vehicle script diagnostics"),
    "src/script/scr_vm.cpp": (2, "Apple-side VM diagnostics"),

    # Tuned to iPhone screen geometry. Enabling ui_atoms.cpp on Android
    # collapsed the whole main menu, so these stay off until each one is
    # validated against a phone.
    "src/ui/ui_atoms.cpp": (2, "iPhone canvas reshaping; blanked the Android menu when enabled"),
    "src/ui/ui_main.cpp": (1, "same canvas family as ui_atoms.cpp"),

    # Multiplayer. The Android MP path is not validated yet; revisit together.
    "src/client_mp/cl_main_mp.cpp": (9, "CoD4X multiplayer client; Android MP not validated yet"),
    "src/client_mp/cl_cod4x.cpp": (4, "CoD4X multiplayer client; Android MP not validated yet"),
    "src/client_mp/cl_main_pc_mp.cpp": (1, "CoD4X server browser refresh; Android MP not validated yet"),
}


def check_apple_only_policy() -> list[str]:
    """Check 10: no unclassified Apple-only guard may exist in shared code."""
    found: dict[str, int] = {}
    for root, dirs, files in os.walk("src"):
        dirs[:] = [d for d in dirs
                   if os.path.join(root, d).replace("\\", "/") not in ENGINE_SKIP_DIRS]
        if root.replace("\\", "/") in ENGINE_SKIP_DIRS:
            continue
        for name in sorted(files):
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(root, name).replace("\\", "/")
            text = open(path, encoding="utf-8", errors="replace").read()
            count = sum(1 for line in text.splitlines()
                        if "__APPLE__" in line and "__ANDROID__" not in line)
            if count:
                found[path] = count

    problems: list[str] = []
    for name, count in sorted(found.items()):
        if name not in APPLE_ONLY_POLICY:
            problems.append(
                f"{name}: {count} Apple-only guard(s) with no entry in APPLE_ONLY_POLICY. "
                f"Extend them with '#if defined(__APPLE__) || defined(__ANDROID__)', "
                f"or record why Android must not take that path."
            )
        elif APPLE_ONLY_POLICY[name][0] != count:
            problems.append(
                f"{name}: {count} Apple-only guard(s), policy says "
                f"{APPLE_ONLY_POLICY[name][0]}; update the count."
            )
    for name in sorted(APPLE_ONLY_POLICY):
        if name not in found:
            problems.append(f"{name}: in APPLE_ONLY_POLICY but has no Apple-only guards left; drop the entry.")
    return problems


def check_signed_char() -> list[str]:
    """Check 11: the Android build must force signed char.

    The engine is decompiled from an x86 Windows build, where plain char is
    signed, and Apple's ARM64 ABI keeps it signed, which is why iOS never saw
    this. The generic AArch64 ABI makes it unsigned, so every char the engine
    uses as a small signed number flips: Glyph::x0 in src/gfx_d3d/r_font.h is
    a negative left side bearing, and unsigned it threw glyphs hundreds of
    pixels sideways.
    """
    text = open("ports/android/CMakeLists.txt", encoding="utf-8").read()
    if "-fsigned-char" not in text:
        return ["ports/android/CMakeLists.txt: -fsigned-char is missing from "
                "KISAK_ANDROID_COMPILE_OPTIONS; plain char is unsigned on AArch64 and the "
                "engine assumes signed."]
    return []


def check_long_double() -> list[str]:
    """Check 12: no `long double` in the decompiled engine.

    IDA emitted `long double` for x86 FPU temporaries and the decompiled code
    reads them straight back with `*(double *)&v`. That is an identity cast
    only where `long double` is eight bytes: MSVC x86, and Apple's ARM64 ABI,
    which is why Windows and iOS never noticed. The generic AArch64 ABI makes
    `long double` IEEE binary128, so the cast reads the low half of a
    quad-precision value - near enough always a denormal. An `altertimescale`
    of 0.25 arrived as -0 and tripped `timescale > 0` in Com_SetTimeScale;
    `*(long double *)&eval->opStack[...]` wrote sixteen bytes into an int
    array. Plain `double` is correct on every target the port builds for.

    Ports may still use `long double` deliberately; this only guards src/.
    """
    problems = []
    for root, _dirs, files in os.walk("src"):
        for name in sorted(files):
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(root, name)
            with open(path, encoding="utf-8", errors="replace") as handle:
                for number, line in enumerate(handle, 1):
                    if LONG_DOUBLE_RE.search(line):
                        problems.append(f"{path}:{number}: {line.strip()}")
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

    for label, check in (
        ("Block-scope extern declarations", check_block_scope_externs),
        ("Backend interface drift", check_backend_interfaces),
        ("Undefined port symbols", check_port_symbols),
        ("Apple-only definitions", check_apple_only_definitions),
        ("Apple-gated shared calls", check_apple_gated_shared_calls),
        ("Backup rules", check_backup_rules),
        ("Apple-only guard policy", check_apple_only_policy),
        ("Signed char", check_signed_char),
        ("long double in src/", check_long_double),
    ):
        problems = check()
        if problems:
            failed = True
            print(f"{label} ({len(problems)}):")
            for problem in problems:
                print(f"  {problem}")
        else:
            print(f"{label}: ok")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
