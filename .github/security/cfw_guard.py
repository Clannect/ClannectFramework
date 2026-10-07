#!/usr/bin/env python3
"""Source policy gates for .github/workflows/cfw-security-guard.yml.

    cfw_guard.py cxx           --log FILE   C/C++: process spawning, code injection, run-time loading
    cfw_guard.py supply-chain  --log FILE   CMake, Python, shell and committed executables
    cfw_guard.py flawfinder CSV --log FILE  Flawfinder hits of level 4 and 5 (from --csv)

Every finding is a (rule, file) pair with a count. A pair passes only if
allowlist.txt, next to this script, lists it with at least that count; anything
else fails the gate (exit 1). An error also fails it (exit 2): nothing here
passes by default. Add --print-allowlist to print the current findings in the
allowlist's format, to review and paste.

The scans read the files git tracks, as text: they catch a call that is written
down, not one that is assembled at run time. They are a tripwire for review,
not a proof of absence.
"""

import argparse
import ast
import csv
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ALLOWLIST = Path(__file__).with_name("allowlist.txt")

CXX_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".m", ".mm")

# Identifiers, matched as whole tokens outside comments and literals, so that
# taking the address of a function counts as much as calling it.
CXX_RULES = {
    "cxx-process-spawn": (
        "system _wsystem popen _popen _wpopen execl execlp execle execv execvp execve execvpe fexecve "
        "posix_spawn posix_spawnp CreateProcess CreateProcessA CreateProcessW CreateProcessAsUserA "
        "CreateProcessAsUserW CreateProcessWithLogonW CreateProcessWithTokenW ShellExecute ShellExecuteA "
        "ShellExecuteW ShellExecuteEx ShellExecuteExA ShellExecuteExW WinExec"
    ),
    "cxx-code-injection": (
        "WriteProcessMemory ReadProcessMemory CreateRemoteThread CreateRemoteThreadEx NtCreateThreadEx "
        "RtlCreateUserThread QueueUserAPC VirtualAllocEx VirtualProtect VirtualProtectEx mprotect "
        "memfd_create ptrace process_vm_writev SetWindowsHookEx SetWindowsHookExA SetWindowsHookExW"
    ),
    "cxx-dynamic-load": (
        "dlopen dlmopen dlsym dlvsym LoadLibrary LoadLibraryA LoadLibraryW LoadLibraryEx LoadLibraryExA "
        "LoadLibraryExW GetProcAddress"
    ),
}

CMAKE_RULES = {
    "cmake-download": r"\bfile\s*\(\s*(DOWNLOAD|UPLOAD)\b|\b(FetchContent|ExternalProject|CPM)\w*\s*\("
    r"|\binclude\s*\(\s*(FetchContent|ExternalProject|CPM)\b",
    "cmake-exec": r"\b(execute_process|exec_program|add_custom_command|add_custom_target)\s*\("
    r"|\binstall\s*\(\s*(CODE|SCRIPT)\b",
    "cmake-url": r"\b(https?|ftp|git|ssh)://|\bgit@",
    "cmake-net-tool": r"(?<![\w.-])(curl|wget|powershell|pwsh|Invoke-WebRequest|Invoke-Expression|certutil"
    r"|bitsadmin|mshta|nc|ncat|netcat|scp|ssh|base64)(?![\w.-])",
}

SHELL_RULES = {
    "sh-network": r"(?<![\w.-])(curl|wget|nc|ncat|netcat|telnet|ssh|scp|sftp|ftp|rsync)(?![\w.-])|/dev/(tcp|udp)/",
    "sh-dynamic-code": r"(?<![\w.-])eval(?![\w.-])|\bbase64\s+(-d|-D|--decode)\b|\|\s*(sudo\s+)?(ba|da|z|k)?sh\b"
    r"|\b(python3?|perl|ruby|node)\s+-[ce]\b|\bxxd\s+-r\b",
}

PY_NETWORK = {
    "socket", "socketserver", "ssl", "urllib", "urllib2", "urllib3", "http", "httplib", "httpx", "requests",
    "aiohttp", "ftplib", "smtplib", "poplib", "imaplib", "telnetlib", "xmlrpc", "paramiko", "websocket",
    "websockets", "pycurl", "asyncio", "webbrowser",
}
PY_PROCESS = {"subprocess", "pty", "multiprocessing", "pexpect", "sh"}
PY_OBFUSCATION = {"marshal", "pickle", "cPickle", "dill", "shelve", "base64", "binascii", "codecs", "ctypes", "cffi"}
PY_DYNAMIC_CALLS = {"eval", "exec", "compile", "__import__", "execfile"}
PY_OS_PROCESS = re.compile(r"^(system|popen|exec[lv]p?e?|spawn[lv]p?e?|posix_spawnp?|startfile|fork|forkpty)$")
URL = re.compile(r"\b(https?|ftp|git|ssh)://", re.I)

EXECUTABLE_SUFFIXES = (
    ".pyc", ".pyo", ".class", ".jar", ".exe", ".dll", ".sys", ".scr", ".com", ".msi", ".so", ".dylib", ".o",
    ".obj", ".a", ".lib", ".ko", ".bat", ".cmd", ".ps1", ".vbs", ".hta", ".lnk",
)
MACH_O = {b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"}


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    files = [name for name in out.decode("utf-8").split("\0") if name]
    if not files:
        raise SystemExit("error: git lists no tracked files; refusing to report a clean tree")
    return files


def read_text(path):
    return Path(path).read_bytes().decode("utf-8", errors="replace")


def blank(match):
    # Keep the line breaks, so that line numbers still point at the source.
    return re.sub(r"[^\n]", " ", match.group(0))


CXX_NOT_CODE = re.compile(
    r"""//(?:\\\n|[^\n])*                       # line comment, with continuations
      | /\*.*?\*/                               # block comment
      | R"([^()\\\s]{0,16})\(.*?\)\1"           # raw string literal
      | "(?:\\.|[^"\\\n])*"                     # string literal
      | (?<![0-9A-Za-z_])'(?:\\.|[^'\\\n])+'    # character literal (not a digit separator)
    """,
    re.S | re.X,
)


def scan_cxx(files):
    rules = [(rule, re.compile(r"(?<![A-Za-z0-9_])(%s)(?![A-Za-z0-9_])" % "|".join(names.split())))
             for rule, names in CXX_RULES.items()]
    sources = [name for name in files if name.lower().endswith(CXX_SUFFIXES)]
    if not sources:
        raise SystemExit("error: no C/C++ sources found; refusing to report a clean tree")
    findings = []
    for name in sources:
        original = read_text(name).splitlines()
        code = CXX_NOT_CODE.sub(blank, read_text(name)).splitlines()
        for number, line in enumerate(code, 1):
            for rule, pattern in rules:
                if pattern.search(line):
                    findings.append((rule, name, number, original[number - 1].strip()))
    return findings, len(sources)


def strip_hash_comments(text):
    """Blanks # comments that start outside double quotes (CMake and shell)."""
    text = re.sub(r"#\[(=*)\[.*?\]\1\]", blank, text, flags=re.S)
    lines = []
    for line in text.split("\n"):
        quoted = False
        for index, char in enumerate(line):
            if char == '"' and (index == 0 or line[index - 1] != "\\"):
                quoted = not quoted
            elif char == "#" and not quoted and (index == 0 or line[index - 1] not in "$\\"):
                line = line[:index]
                break
        lines.append(line)
    return lines


def scan_patterns(name, lines, rules, flags=0):
    findings = []
    for number, line in enumerate(lines, 1):
        for rule, pattern in rules.items():
            if re.search(pattern, line, flags):
                findings.append((rule, name, number, line.strip()))
    return findings


def scan_python(name):
    source = read_text(name)
    try:
        tree = ast.parse(source, filename=name)
    except (SyntaxError, ValueError) as error:
        return [("py-unparsable", name, getattr(error, "lineno", 0) or 0, str(error))]
    lines = source.splitlines()
    findings = []

    def add(rule, node):
        text = lines[node.lineno - 1].strip() if 0 < node.lineno <= len(lines) else ""
        findings.append((rule, name, node.lineno, text))

    def module_rule(module, node):
        top = module.split(".")[0]
        for rule, modules in (("py-network", PY_NETWORK), ("py-process", PY_PROCESS),
                              ("py-obfuscation", PY_OBFUSCATION)):
            if top in modules:
                add(rule, node)
        if top in ("importlib", "imp", "runpy", "code", "codeop"):
            add("py-dynamic-code", node)

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                module_rule(alias.name, node)
        elif isinstance(node, ast.ImportFrom):
            module_rule(node.module or "", node)
        elif isinstance(node, ast.Name) and node.id in PY_DYNAMIC_CALLS:
            add("py-dynamic-code", node)
        elif isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            if node.value.id == "os" and PY_OS_PROCESS.match(node.attr):
                add("py-process", node)
        elif isinstance(node, ast.Constant) and isinstance(node.value, (str, bytes)):
            value = node.value if isinstance(node.value, str) else node.value.decode("latin-1")
            if URL.search(value):
                add("py-url", node)
    return findings


def scan_binary(name):
    path = Path(name)
    if path.is_symlink() or not path.is_file():
        return []
    with path.open("rb") as stream:
        head = stream.read(4096)
    kind = None
    if head[:4] == b"\x7fELF":
        kind = "ELF"
    elif head[:4] in MACH_O:
        kind = "Mach-O or Java class"
    elif head[:2] == b"MZ" and len(head) >= 64:
        offset = int.from_bytes(head[60:64], "little")
        if offset + 4 <= len(head) and head[offset:offset + 4] == b"PE\0\0":
            kind = "PE"
    if kind is None and name.lower().endswith(EXECUTABLE_SUFFIXES):
        kind = "%s file" % path.suffix.lower()
    return [("binary-executable", name, 0, "committed executable (%s)" % kind)] if kind else []


def scan_supply_chain(files):
    findings = []
    counts = Counter()
    for name in files:
        lower = name.lower()
        base = lower.rsplit("/", 1)[-1]
        findings += scan_binary(name)
        if base == "cmakelists.txt" or lower.endswith((".cmake", ".cmake.in")) or base.startswith("cmake") \
                and base.endswith(".json"):
            lines = read_text(name).split("\n") if base.endswith(".json") else strip_hash_comments(read_text(name))
            findings += scan_patterns(name, lines, CMAKE_RULES, re.I)
            counts["CMake"] += 1
        elif lower.endswith((".py", ".pyw")):
            findings += scan_python(name)
            counts["Python"] += 1
        elif lower.endswith((".sh", ".bash", ".zsh", ".ksh")):
            findings += scan_patterns(name, strip_hash_comments(read_text(name)), SHELL_RULES)
            counts["shell"] += 1
    if not counts["CMake"]:
        raise SystemExit("error: no CMake files found; refusing to report a clean tree")
    return findings, ", ".join("%d %s" % (count, kind) for kind, count in counts.items())


def scan_flawfinder(report):
    findings = []
    with open(report, newline="", encoding="utf-8", errors="replace") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or not {"File", "Line", "Level", "Name", "Warning"} <= set(reader.fieldnames):
            raise SystemExit("error: %s is not a Flawfinder CSV report" % report)
        for row in reader:
            if row["Level"] == "Level":  # xargs split the files over two runs: a second header
                continue
            if int(row["Level"]) >= 4:
                name = row["File"].replace("\\", "/")
                name = name[2:] if name.startswith("./") else name
                text = "[level %s, %s] %s" % (row["Level"], row.get("CWEs", ""), row.get("Context", "").strip())
                findings.append(("flawfinder-" + row["Name"], name, int(row["Line"]), text))
    return findings


def load_allowlist():
    allowed = {}
    for number, line in enumerate(ALLOWLIST.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split("#", 1)[0].split()
        if not fields:
            continue
        if len(fields) != 3 or not fields[2].isdigit():
            raise SystemExit("error: %s:%d: expected '<rule> <file> <count>'" % (ALLOWLIST.name, number))
        allowed[(fields[0], fields[1])] = int(fields[2])
    return allowed


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("gate", choices=("cxx", "supply-chain", "flawfinder"))
    parser.add_argument("report", nargs="?", help="Flawfinder CSV report (flawfinder gate)")
    parser.add_argument("--log", help="also write the report to this file")
    parser.add_argument("--print-allowlist", action="store_true")
    args = parser.parse_args()

    if args.gate == "flawfinder":
        if not args.report:
            parser.error("the flawfinder gate needs the CSV report")
        findings, scanned, prefixes = scan_flawfinder(args.report), "Flawfinder report", ("flawfinder-",)
    elif args.gate == "cxx":
        findings, count = scan_cxx(tracked_files())
        scanned, prefixes = "%d C/C++ files" % count, ("cxx-",)
    else:
        findings, scanned = scan_supply_chain(tracked_files())
        prefixes = ("cmake-", "py-", "sh-", "binary-")

    counts = Counter((rule, name) for rule, name, _, _ in findings)
    if args.print_allowlist:
        for (rule, name), count in sorted(counts.items()):
            print("%-28s %-56s %d" % (rule, name, count))
        return 0

    allowed = load_allowlist()
    blocked = {key: count for key, count in counts.items() if count > allowed.get(key, 0)}
    out = ["gate: %s (%s)" % (args.gate, scanned), ""]
    for status, keys in (("BLOCKED", blocked), ("allowlisted", counts.keys() - blocked.keys())):
        for rule, name in sorted(keys):
            limit = allowed.get((rule, name), 0)
            out.append("%s  %s  %s  (found %d, allowlist %d)" % (status, rule, name, counts[(rule, name)], limit))
            for found_rule, found_name, line, text in findings:
                if (found_rule, found_name) == (rule, name):
                    out.append("    %s:%d: %s" % (name, line, text[:200]))
    stale = sorted(key for key, limit in allowed.items()
                   if key[0].startswith(prefixes) and counts.get(key, 0) < limit)
    for rule, name in stale:
        out.append("note: allowlist has %s %s %d, found %d: lower the entry"
                   % (rule, name, allowed[(rule, name)], counts.get((rule, name), 0)))
    out += ["", "result: %d finding(s) in %d (rule, file) pair(s); %d pair(s) blocked"
            % (len(findings), len(counts), len(blocked))]
    report = "\n".join(out) + "\n"
    sys.stdout.write(report)
    if args.log:
        Path(args.log).write_text(report, encoding="utf-8")
    return 1 if blocked else 0


if __name__ == "__main__":
    sys.exit(main())
