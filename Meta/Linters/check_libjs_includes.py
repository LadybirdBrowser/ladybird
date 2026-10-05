#!/usr/bin/env python3
"""Keeps code outside LibJS on the LibJS headers that make up the engine's embedding API.

ALLOWED_HEADERS is the set of LibJS headers that code outside Libraries/LibJS includes, which is all of the engine that
its users depend on. The list is exact: a header that nothing outside LibJS includes any more has to leave it, and a new
one needs a reason to become part of the embedding API. Engine internals (bytecode and the interpreter, shapes and
property storage, heap internals, the parser) can never be part of it, so including one outside LibJS fails whatever
ALLOWED_HEADERS says.
"""

import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
THIS_SCRIPT = pathlib.Path(__file__).resolve().relative_to(REPO_ROOT)

ALLOWED_HEADERS = {
    "LibJS/Console.h",
    "LibJS/ConsoleLogLevel.h",
    "LibJS/Debugger.h",
    "LibJS/Forward.h",
    "LibJS/Heap/Cell.h",
    "LibJS/HostClassBuilder.h",
    "LibJS/HostObjectABI.h",
    "LibJS/Module.h",
    "LibJS/ParserError.h",
    "LibJS/Position.h",
    "LibJS/Print.h",
    "LibJS/Runtime/AbstractOperations.h",
    "LibJS/Runtime/Accessor.h",
    "LibJS/Runtime/Agent.h",
    "LibJS/Runtime/Array.h",
    "LibJS/Runtime/ArrayBuffer.h",
    "LibJS/Runtime/ArrayPrototype.h",
    "LibJS/Runtime/AsyncIteratorPrototype.h",
    "LibJS/Runtime/BigInt.h",
    "LibJS/Runtime/BigIntObject.h",
    "LibJS/Runtime/BooleanObject.h",
    "LibJS/Runtime/Completion.h",
    "LibJS/Runtime/ConsoleObject.h",
    "LibJS/Runtime/DataView.h",
    "LibJS/Runtime/DataViewConstructor.h",
    "LibJS/Runtime/Date.h",
    "LibJS/Runtime/DeclarativeEnvironment.h",
    "LibJS/Runtime/Environment.h",
    "LibJS/Runtime/Error.h",
    "LibJS/Runtime/ErrorConstructor.h",
    "LibJS/Runtime/ErrorData.h",
    "LibJS/Runtime/ErrorTypes.h",
    "LibJS/Runtime/ExternalMemory.h",
    "LibJS/Runtime/FinalizationRegistry.h",
    "LibJS/Runtime/FunctionEnvironment.h",
    "LibJS/Runtime/FunctionObject.h",
    "LibJS/Runtime/GlobalEnvironment.h",
    "LibJS/Runtime/GlobalObject.h",
    "LibJS/Runtime/HostArray.h",
    "LibJS/Runtime/HostFunction.h",
    "LibJS/Runtime/HostModule.h",
    "LibJS/Runtime/HostObject.h",
    "LibJS/Runtime/Intrinsics.h",
    "LibJS/Runtime/Iterator.h",
    "LibJS/Runtime/IteratorPrototype.h",
    "LibJS/Runtime/JobCallback.h",
    "LibJS/Runtime/JSONObject.h",
    "LibJS/Runtime/Map.h",
    "LibJS/Runtime/MapIterator.h",
    "LibJS/Runtime/ModuleEnvironment.h",
    "LibJS/Runtime/ModuleRequest.h",
    "LibJS/Runtime/NativeFunction.h",
    "LibJS/Runtime/NumberObject.h",
    "LibJS/Runtime/Object.h",
    "LibJS/Runtime/ObjectEnvironment.h",
    "LibJS/Runtime/PrimitiveString.h",
    "LibJS/Runtime/Promise.h",
    "LibJS/Runtime/PromiseCapability.h",
    "LibJS/Runtime/PromiseConstructor.h",
    "LibJS/Runtime/PropertyDescriptor.h",
    "LibJS/Runtime/PropertyKey.h",
    "LibJS/Runtime/Realm.h",
    "LibJS/Runtime/Reference.h",
    "LibJS/Runtime/RegExpObject.h",
    "LibJS/Runtime/Set.h",
    "LibJS/Runtime/SetIterator.h",
    "LibJS/Runtime/SharedArrayBufferConstructor.h",
    "LibJS/Runtime/StringObject.h",
    "LibJS/Runtime/Symbol.h",
    "LibJS/Runtime/TypedArray.h",
    "LibJS/Runtime/Value.h",
    "LibJS/Runtime/ValueInlines.h",
    "LibJS/Runtime/VM.h",
    "LibJS/Script.h",
    "LibJS/ScriptCompilation.h",
    "LibJS/SourceCode.h",
    "LibJS/SourceTextModule.h",
    "LibJS/SyntaxHighlighter.h",
    "LibJS/SyntheticModule.h",
    "LibJS/Token.h",
}

ENGINE_INTERNAL_HEADER_DIRECTORIES = (
    "LibJS/Bytecode/",
    "LibJS/Contrib/",
    "LibJS/Heap/",
    "LibJS/Interpreter/",
)

ENGINE_INTERNAL_HEADERS = {
    "LibJS/AST.h",
    "LibJS/Lexer.h",
    "LibJS/Parser.h",
    "LibJS/Runtime/EnvironmentShape.h",
    "LibJS/Runtime/IndexedProperties.h",
    "LibJS/Runtime/InterpreterStack.h",
    "LibJS/Runtime/Shape.h",
    "LibJS/RustIntegration.h",
}

# Heap/Cell.h declares JS::Cell, the base of the cells that code outside LibJS defines.
PUBLIC_HEADERS_IN_ENGINE_INTERNAL_DIRECTORIES = {
    "LibJS/Heap/Cell.h",
}

# LibJS and its own tests, the clang plugin tests, which model LibJS's own classes, and the code that still embeds the
# engine the old way and moves to the embedding API's headers when it is ported.
EXEMPT_PATHS = (
    "Libraries/LibJS/",
    "Meta/Fuzzers/FuzzJs.cpp",
    "Tests/ClangPlugins/",
    "Tests/LibJS/",
    "Utilities/js.cpp",
    "Utilities/test262-runner.cpp",
)

CPP_SUFFIXES = {".c", ".cpp", ".h", ".ipc", ".mm"}

CPP_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*[<"](LibJS/[^>"]+)[>"]', re.MULTILINE)

# The bindings generators name the headers that generated code includes as strings.
GENERATOR_ROOT = "Meta/Generators/"
GENERATOR_HEADER_PATTERN = re.compile(r"\bLibJS/[A-Za-z0-9_/]+\.h\b")


def tracked_paths():
    output = subprocess.check_output(["git", "ls-files"], cwd=REPO_ROOT, text=True)
    for line in output.splitlines():
        if not line.startswith(EXEMPT_PATHS):
            yield pathlib.PurePosixPath(line)


def included_libjs_headers(path):
    if path.suffix in CPP_SUFFIXES:
        pattern = CPP_INCLUDE_PATTERN
    elif path.suffix == ".py" and str(path).startswith(GENERATOR_ROOT):
        pattern = GENERATOR_HEADER_PATTERN
    else:
        return []
    text = (REPO_ROOT / path).read_text(encoding="utf-8", errors="ignore")
    if "LibJS/" not in text:
        return []
    return [match.group(1) if match.groups() else match.group(0) for match in pattern.finditer(text)]


def is_engine_internal(header):
    if header in PUBLIC_HEADERS_IN_ENGINE_INTERNAL_DIRECTORIES:
        return False
    return header in ENGINE_INTERNAL_HEADERS or header.startswith(ENGINE_INTERNAL_HEADER_DIRECTORIES)


def main():
    users_of_header = {}
    for path in tracked_paths():
        for header in included_libjs_headers(path):
            users_of_header.setdefault(header, []).append(str(path))

    allowed_engine_internals = sorted(header for header in ALLOWED_HEADERS if is_engine_internal(header))
    included_engine_internals = sorted(header for header in users_of_header if is_engine_internal(header))
    disallowed = sorted(set(users_of_header) - ALLOWED_HEADERS - set(included_engine_internals))
    unused = sorted(ALLOWED_HEADERS - set(users_of_header))

    for header in allowed_engine_internals:
        print(
            f"{header} is an engine internal and can never be part of LibJS's embedding API; remove it from "
            f"ALLOWED_HEADERS in {THIS_SCRIPT}."
        )
    for header in included_engine_internals:
        print(f"{header} is an engine internal, but code outside LibJS includes it:")
        for user in sorted(users_of_header[header]):
            print(f"  {user}")
    for header in disallowed:
        print(f"{header} is not part of LibJS's embedding API, but code outside LibJS includes it:")
        for user in sorted(users_of_header[header]):
            print(f"  {user}")
    for header in unused:
        print(f"{header} is no longer included outside LibJS; remove it from ALLOWED_HEADERS in {THIS_SCRIPT}.")

    if included_engine_internals:
        print()
        print(
            "Code outside LibJS has to use LibJS's embedding API. If that API lacks something an engine internal has, "
            "add it to one of the headers in ALLOWED_HEADERS instead of including the internal."
        )
    if disallowed:
        print()
        print(
            "Use LibJS's embedding API instead. A header that is not an engine internal can join ALLOWED_HEADERS "
            "only if it belongs in that API."
        )

    if allowed_engine_internals or included_engine_internals or disallowed or unused:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
