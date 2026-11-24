"""Writes the header Tempo Tantrum's Pro Tools Scripting client is built with.

Run by CMakeLists.txt at configure time:

    python3 ptsl_generate.py <unpacked PTSL SDK folder> <output header>

It reads the SDK's own definitions (Source/PTSL.proto and Source/PTSL_Versions.h) and writes the
numbers the client needs: the path of the unary call, the field numbers of the few messages the
client writes and reads by hand, the command numbers of the commands it sends and the task
statuses. The output goes to the build folder only. Nothing it writes may be committed: the SDK
is Avid's and confidential, and this repository is public.

Any name that is not found stops the build with a message, so a changed SDK is noticed at once.
"""

import re
import sys
from pathlib import Path

# The commands the client may send, by name.
COMMANDS = [
    "RegisterConnection",
    "HostReadyCheck",
    "GetPTSLVersion",
    "GetSessionName",
    "GetSessionSampleRate",
    "GetSessionBitDepth",
    "GetSessionStartTime",
    "GetSessionLength",
    "GetTransportState",
    "GetMainCounterFormat",
    "GetEditSelection",
    "GetTimelineSelection",
    "GetEditModeOptions",
    "GetFileLocation",
    "GetTimeAsType",
    "GetTrackList",
    "GetClipList",
    "GetMediaFileInfo",
    "GetMemoryLocations",
    "ExportSessionInfoAsText",
    "TogglePlayState",
    "SetMainCounterFormat",
    "SetEditModeOptions",
    "SetTimelineSelection",
    "BounceTrack",
]

# (C++ name, message, field)
FIELDS = [
    ("requestHeader", "Request", "header"),
    ("requestBodyJson", "Request", "request_body_json"),
    ("requestHeaderCommand", "RequestHeader", "command"),
    ("requestHeaderVersion", "RequestHeader", "version"),
    ("requestHeaderSessionId", "RequestHeader", "session_id"),
    ("requestHeaderVersionMinor", "RequestHeader", "version_minor"),
    ("requestHeaderVersionRevision", "RequestHeader", "version_revision"),
    ("responseHeader", "Response", "header"),
    ("responseBodyJson", "Response", "response_body_json"),
    ("responseErrorJson", "Response", "response_error_json"),
    ("responseHeaderTaskId", "ResponseHeader", "task_id"),
    ("responseHeaderCommand", "ResponseHeader", "command"),
    ("responseHeaderStatus", "ResponseHeader", "status"),
]

STATUSES = ["Queued", "Pending", "InProgress", "Completed", "Failed", "WaitingForUserInput"]


def fail(message):
    sys.stderr.write("ptsl_generate.py: " + message + "\n")
    sys.exit(1)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def block(text, kind, name):
    """The body of `message Name { ... }` or `enum Name { ... }`, braces matched."""
    match = re.search(r"\b" + kind + r"\s+" + re.escape(name) + r"\s*\{", text)
    if not match:
        fail(kind + " " + name + " not found")
    depth, at = 1, match.end()
    while depth > 0 and at < len(text):
        depth += {"{": 1, "}": -1}.get(text[at], 0)
        at += 1
    return text[match.end():at - 1]


def field_number(text, message, field):
    body = block(text, "message", message)
    match = re.search(r"[\w.]+\s+" + re.escape(field) + r"\s*=\s*(\d+)\s*;", body)
    if not match:
        fail("field " + message + "." + field + " not found")
    return int(match.group(1))


def enum_value(text, enum, names):
    body = block(text, "enum", enum)
    for name in names:
        match = re.search(r"(?<![\w])" + re.escape(name) + r"\s*=\s*(\d+)\s*;", body)
        if match:
            return int(match.group(1))
    fail("none of " + ", ".join(names) + " found in enum " + enum)


def version_constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(\d+)\s*;", text)
    if not match:
        fail(name + " not found in PTSL_Versions.h")
    return int(match.group(1))


def main():
    if len(sys.argv) != 3:
        fail("usage: ptsl_generate.py <PTSL SDK folder> <output header>")

    sdk = Path(sys.argv[1])
    proto_path = sdk / "Source" / "PTSL.proto"
    versions_path = sdk / "Source" / "PTSL_Versions.h"

    if not proto_path.is_file():
        fail(str(proto_path) + " not found")
    if not versions_path.is_file():
        fail(str(versions_path) + " not found")

    proto = strip_comments(proto_path.read_text(encoding="utf-8", errors="replace"))
    versions = strip_comments(versions_path.read_text(encoding="utf-8", errors="replace"))

    package = re.search(r"\bpackage\s+([\w.]+)\s*;", proto)
    if not package:
        fail("package not found")

    service = re.search(r"\bservice\s+(\w+)\s*\{", proto)
    if not service:
        fail("service not found")

    service_body = block(proto, "service", service.group(1))
    unary = None
    for rpc in re.finditer(r"\brpc\s+(\w+)\s*\(\s*(stream\s+)?(\w+)\s*\)\s*returns\s*\(\s*(stream\s+)?(\w+)\s*\)", service_body):
        if not rpc.group(2) and not rpc.group(4) and rpc.group(3) == "Request" and rpc.group(5) == "Response":
            unary = rpc.group(1)
            break
    if not unary:
        fail("no unary rpc taking Request and returning Response")

    path = "/" + package.group(1) + "." + service.group(1) + "/" + unary

    major = version_constant(versions, "PTSL_VERSION")
    minor = version_constant(versions, "PTSL_VERSION_MINOR")
    revision = version_constant(versions, "PTSL_VERSION_REVISION")

    lines = [
        "// Generated at configure time by tempotantrum/Tools/ptsl_generate.py from the Pro Tools",
        "// Scripting SDK. It lives in the build folder only and must never be committed.",
        "#pragma once",
        "",
        "namespace tt::ptsl::generated",
        "{",
        '    inline constexpr const char* unaryPath = "' + path + '";',
        "    inline constexpr int apiVersion = " + str(major) + ";",
        "    inline constexpr int apiVersionMinor = " + str(minor) + ";",
        "    inline constexpr int apiVersionRevision = " + str(revision) + ";",
        "",
    ]

    for cpp, message, field in FIELDS:
        lines.append("    inline constexpr int " + cpp + "Field = " + str(field_number(proto, message, field)) + ";")

    lines.append("")

    for status in STATUSES:
        value = enum_value(proto, "TaskStatus", ["TStatus_" + status, status])
        lines.append("    inline constexpr int status" + status + " = " + str(value) + ";")

    lines += ["", "    struct CommandNumber { const char* name; int number; };", "",
              "    inline constexpr CommandNumber commands[] =", "    {"]

    for name in COMMANDS:
        value = enum_value(proto, "CommandId", ["CId_" + name, name])
        lines.append('        { "' + name + '", ' + str(value) + " },")

    lines += ["    };", "}", ""]

    out = Path(sys.argv[2])
    out.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines)

    if not out.is_file() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
