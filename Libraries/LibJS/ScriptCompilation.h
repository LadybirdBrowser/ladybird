/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Result.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/ParserError.h>
#include <LibJS/Position.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Script.h>

namespace JS::FFI {

struct ParsedProgram;
struct CompiledProgram;

}

namespace JS {

enum class ProgramType : u8 {
    Script = 0,
    Module = 1,
};

// A function compiled from standalone source text the way CreateDynamicFunction compiles one, for hosts that create
// functions outside of any script, such as event handler content attributes.
class JS_API CompiledDynamicFunction {
public:
    // Fails with a syntax error message if the parameters or the body do not parse.
    static Result<CompiledDynamicFunction, Utf16String> compile(VM&, Utf16View source_text, Utf16View parameters_string, Utf16View body_parse_string, FunctionKind);

    // OrdinaryFunctionCreate over the compiled function, closing over the given environments, with the default
    // prototype for the function's kind and the given [[ScriptOrModule]].
    GC::Ref<FunctionObject> instantiate(Realm&, Environment& scope, GC::Ptr<PrivateEnvironment>, ScriptOrModule) const;

private:
    // The runtime's compiled code of a function, its SharedFunctionInstanceData.
    class FunctionData final : public EngineCell {
    };

    explicit CompiledDynamicFunction(GC::Ref<FunctionData>);

    GC::Root<FunctionData> m_function_data;
};

// Every position in the source where a breakpoint can be set, including positions inside functions that have not been
// compiled yet. This compiles a private copy of the source without touching the VM or the GC heap.
JS_API Vector<Position> breakpoint_positions_for_source(SourceCode const&, ProgramType, size_t line_number_offset);

// Whether two functions run the same compiled code, which compiles them if they did not run yet. Only for tests.
JS_API bool functions_share_code(VM&, FunctionObject const&, FunctionObject const&);

class ParsedProgram;
class CompiledProgram;

// Turning a parsed or compiled program into a record must happen on the main thread. A parsed program with syntax
// errors yields those errors.
JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);

// ParsedProgram and CompiledProgram hold a program outside of the VM and the GC heap. Parsing, compiling and destroying
// them is thread-safe, so a host can do that work on a worker thread.
class JS_API ParsedProgram {
    AK_MAKE_NONCOPYABLE(ParsedProgram);

public:
    static ParsedProgram parse(Utf16String source_text, ProgramType, size_t line_number_offset);

    ParsedProgram() = default;
    ParsedProgram(ParsedProgram&&);
    ParsedProgram& operator=(ParsedProgram&&);
    ~ParsedProgram();

    explicit operator bool() const { return m_program; }
    bool has_errors() const;

private:
    friend class CompiledProgram;
    friend JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
    friend JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);

    explicit ParsedProgram(FFI::ParsedProgram*);

    FFI::ParsedProgram* m_program { nullptr };
};

class JS_API CompiledProgram {
    AK_MAKE_NONCOPYABLE(CompiledProgram);

public:
    // Consumes a program without errors, and generates bytecode for the top level and for the functions it invokes
    // immediately. Every other function is compiled on its first call.
    static CompiledProgram compile(ParsedProgram);

    CompiledProgram() = default;
    CompiledProgram(CompiledProgram&&);
    CompiledProgram& operator=(CompiledProgram&&);
    ~CompiledProgram();

    explicit operator bool() const { return m_program; }

    // Another reference to the same program, which can be turned into a record of its own, in any realm. They all share
    // the program's code, and each record gets the caches and the functions that depend on its realm.
    CompiledProgram clone() const;

    // Whether the code that the program's functions compile to stays with the program from now on, so that the next
    // record finds them compiled. Otherwise, it goes away with the function that compiled it.
    void set_retains_compiled_functions(bool) const;

    // The bytes that the program holds on to right now, which grows as functions compile while it retains them.
    size_t memory_size() const;

private:
    friend JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
    friend JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);

    explicit CompiledProgram(FFI::CompiledProgram*);

    FFI::CompiledProgram* m_program { nullptr };
};

// The source that the record's top-level code was compiled from. Null for a module with top-level await, whose body is
// compiled as an async function instead.
JS_API RefPtr<SourceCode const> top_level_source_code(Script const&);
JS_API RefPtr<SourceCode const> top_level_source_code(SourceTextModule const&);

}
