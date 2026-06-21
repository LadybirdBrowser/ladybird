/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/ExecutableBacking.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>

namespace JS {

JS_API extern bool g_dump_ast;
JS_API extern bool g_dump_ast_use_color;

namespace FFI {

struct ParsedProgram;
struct CompiledProgram;
struct DecodedBytecodeCacheBlob;

}

namespace RustIntegration {

class DecodedBytecodeCache;
struct ScriptResult;

}

// 16.1.4 Script Records, https://tc39.es/ecma262/#sec-script-records
class JS_API Script final : public Cell {
    GC_CELL(Script, Cell);
    GC_DECLARE_ALLOCATOR(Script);

public:
    struct HostDefined {
        virtual ~HostDefined() = default;

        virtual void visit_host_defined_self(Cell::Visitor&) = 0;

        template<typename T>
        bool fast_is() const = delete;

        virtual bool is_script() const { return false; }
        virtual bool is_classic_script() const { return false; }
        virtual bool is_module_script() const { return false; }
    };

    virtual ~Script() override;
    static Result<GC::Ref<Script>, Vector<ParserError>> parse(Utf16View source_text, Realm&, StringView filename = {}, Utf16View display_filename = {}, HostDefined* = nullptr, size_t line_number_offset = 1);
    static Result<GC::Ref<Script>, Vector<ParserError>> create_from_parsed(FFI::ParsedProgram* parsed, NonnullRefPtr<SourceCode const> source_code, Realm&, StringView filename, HostDefined* = nullptr);
    static Result<GC::Ref<Script>, Vector<ParserError>> create_from_compiled(FFI::CompiledProgram* compiled, NonnullRefPtr<SourceCode const> source_code, Realm&, StringView filename, HostDefined* = nullptr);
    static Result<GC::Ref<Script>, Vector<ParserError>> create_from_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache>, NonnullRefPtr<SourceCode const> source_code, Realm&, StringView filename, HostDefined* = nullptr);

    Realm& realm() { return *m_realm; }
    Vector<LoadedModuleRequest>& loaded_modules() { return m_loaded_modules; }
    Vector<LoadedModuleRequest> const& loaded_modules() const { return m_loaded_modules; }

    HostDefined* host_defined() const { return m_host_defined; }
    StringView filename() const LIFETIME_BOUND { return m_filename; }

    Bytecode::Executable* cached_executable() const { return m_executable; }
    ExecutableBacking const& executable_backing() const { return m_executable_backing; }
    [[nodiscard]] bool can_generate_bytecode_cache() const;
    [[nodiscard]] bool can_install_generated_bytecode_cache() const;
    void begin_bytecode_cache_generation();
    void finish_bytecode_cache_generation_without_install();
    bool try_install_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache>, NonnullRefPtr<SourceCode const> source_code);
    void install_generated_bytecode_cache(NonnullRefPtr<RustIntegration::DecodedBytecodeCache>, NonnullRefPtr<SourceCode const> source_code);

    ThrowCompletionOr<void> global_declaration_instantiation(VM&, GlobalEnvironment&);

    // Pre-computed global declaration instantiation data.
    // These are extracted from the AST at parse time so that GDI can run
    // without needing to walk the AST.
    struct FunctionToInitialize {
        GC::Ref<SharedFunctionInstanceData> shared_data;
        Utf16FlyString name;
    };
    struct LexicalBinding {
        Utf16FlyString name;
        bool is_constant { false };
    };

private:
    Script(Realm&, StringView filename, RustIntegration::ScriptResult&&, ExecutableBacking, HostDefined*);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;
    Vector<SharedFunctionInstanceData*> collect_shared_function_data();
    void complete_bytecode_cache_install(GC::Ref<Bytecode::Executable>, NonnullRefPtr<RustIntegration::DecodedBytecodeCache>);
    void verify_executable_backing_invariants();

    GC::Ptr<Realm> m_realm;                       // [[Realm]]
    Vector<LoadedModuleRequest> m_loaded_modules; // [[LoadedModules]]

    mutable GC::Ptr<Bytecode::Executable> m_executable;
    SharedFunctionInstanceDataList m_shared_function_data;
    ExecutableBacking m_executable_backing;

    Vector<Utf16FlyString> m_lexical_names;
    Vector<Utf16FlyString> m_var_names;
    Vector<FunctionToInitialize> m_functions_to_initialize;
    HashTable<Utf16FlyString> m_declared_function_names;
    Vector<Utf16FlyString> m_var_scoped_names;
    Vector<Utf16FlyString> m_annex_b_candidate_names;
    Vector<LexicalBinding> m_lexical_bindings;
    bool m_is_strict_mode { false };

    // Needed for potential lookups of modules.
    ByteString m_filename;
    HostDefined* m_host_defined { nullptr }; // [[HostDefined]]
};

}
