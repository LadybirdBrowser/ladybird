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
#include <LibCore/Forward.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/DecodedBytecodeCache.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
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

using ProgramType = RustIntegration::ProgramType;
using DecodedBytecodeCache = RustIntegration::DecodedBytecodeCache;

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
    explicit CompiledDynamicFunction(GC::Ref<SharedFunctionInstanceData>);

    GC::Root<SharedFunctionInstanceData> m_function_data;
};

// Every position in the source where a breakpoint can be set, including positions inside functions that have not been
// compiled yet. This compiles a private copy of the source without touching the VM or the GC heap.
JS_API Vector<Position> breakpoint_positions_for_source(SourceCode const&, ProgramType, size_t line_number_offset);

class ParsedProgram;
class CompiledProgram;

// Turning a parsed or compiled program into a record must happen on the main thread. A parsed program with syntax
// errors yields those errors.
JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);

// ParsedProgram and CompiledProgram hold a program outside of the VM and the GC heap. Parsing, compiling, serializing
// and destroying them is thread-safe, so a host can do that work on a worker thread.
class JS_API ParsedProgram {
    AK_MAKE_NONCOPYABLE(ParsedProgram);

public:
    static ParsedProgram parse(ReadonlySpan<u16> source_text, ProgramType, size_t line_number_offset);

    ParsedProgram() = default;
    ParsedProgram(ParsedProgram&&);
    ParsedProgram& operator=(ParsedProgram&&);
    ~ParsedProgram();

    explicit operator bool() const { return m_program; }
    bool has_errors() const;

    // Only for a program without errors. The copy compiles independently of the original.
    ParsedProgram clone() const;

private:
    friend class CompiledProgram;
    friend JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
    friend JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(ParsedProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);

    ParsedProgram(FFI::ParsedProgram*, size_t source_length_in_code_units);

    FFI::ParsedProgram* m_program { nullptr };
    size_t m_source_length_in_code_units { 0 };
};

class JS_API CompiledProgram {
    AK_MAKE_NONCOPYABLE(CompiledProgram);

public:
    // Both consume a program without errors. compile() generates bytecode for the top level and for the functions it
    // invokes immediately; every other function is compiled on its first call, or by
    // compile_remaining_functions_off_thread(). compile_all_functions() compiles everything, as a bytecode cache needs.
    static CompiledProgram compile(ParsedProgram);
    static CompiledProgram compile_all_functions(ParsedProgram);

    CompiledProgram() = default;
    CompiledProgram(CompiledProgram&&);
    CompiledProgram& operator=(CompiledProgram&&);
    ~CompiledProgram();

    explicit operator bool() const { return m_program; }

    // A versioned bytecode cache blob for the program, or an empty buffer if the program cannot be cached.
    ByteBuffer serialize_for_bytecode_cache(ProgramType, ReadonlyBytes source_hash) const;

private:
    friend JS_API Result<GC::Ref<Script>, Vector<ParserError>> create_script(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);
    friend JS_API Result<GC::Ref<SourceTextModule>, Vector<ParserError>> create_module(CompiledProgram, NonnullRefPtr<SourceCode const>, Realm&, StringView filename, GC::Ptr<GC::Cell>);

    explicit CompiledProgram(FFI::CompiledProgram*);

    FFI::CompiledProgram* m_program { nullptr };
};

// Thread-safe. Decodes a bytecode cache blob and validates it against the source it claims to be compiled from, or
// returns null if the blob does not match that source or is corrupt. The blob's bytes are released on the main thread
// event loop.
JS_API RefPtr<DecodedBytecodeCache> decode_and_validate_bytecode_cache(Core::ImmutableBytes, ProgramType, ReadonlyBytes source_hash, size_t source_length_in_code_units, Core::EventLoop& main_thread_event_loop);

// How the host runs background compilation work. submit_work is called on the main thread and must run the work on a
// worker thread. post_to_main_thread is called on that worker thread and must run the task on the main thread. Both
// callbacks may be destroyed on either thread.
struct OffThreadCompilationCallbacks {
    Function<void(Function<void()>)> submit_work;
    Function<void(Function<void()>)> post_to_main_thread;
};

// On a worker thread, compiles the functions that the record's top-level code creates and that have not been compiled
// yet, so that their first calls do not have to. Functions that start running in the meantime are compiled on the main
// thread as usual, and the functions they create are then compiled off thread as well. Must be called on the main
// thread.
JS_API void compile_remaining_functions_off_thread(Script&, NonnullRefPtr<SourceCode const>, OffThreadCompilationCallbacks);
JS_API void compile_remaining_functions_off_thread(SourceTextModule&, NonnullRefPtr<SourceCode const>, OffThreadCompilationCallbacks);

// The source that the record's top-level code was compiled from. Null for a module with top-level await, whose body is
// compiled as an async function instead.
JS_API RefPtr<SourceCode const> top_level_source_code(Script const&);
JS_API RefPtr<SourceCode const> top_level_source_code(SourceTextModule const&);

}
