/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

JS_API extern bool g_dump_ast;
JS_API extern bool g_dump_ast_use_color;

class FunctionDeclaration;

namespace RustIntegration {

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
        virtual bool is_javascript_module_script() const { return false; }
    };

    virtual ~Script() override;
    static Result<GC::Ref<Script>, Vector<ParserError>> parse(StringView source_text, Realm&, StringView filename = {}, HostDefined* = nullptr, size_t line_number_offset = 1);

    Realm& realm() { return *m_realm; }
    Program const* parse_node() const { return m_parse_node; }
    Vector<LoadedModuleRequest>& loaded_modules() { return m_loaded_modules; }
    Vector<LoadedModuleRequest> const& loaded_modules() const { return m_loaded_modules; }

    HostDefined* host_defined() const { return m_host_defined; }
    StringView filename() const LIFETIME_BOUND { return m_filename; }

    Bytecode::Executable* cached_executable() const { return m_executable; }
    void cache_executable(Bytecode::Executable& executable) const { m_executable = &executable; }

    ThrowCompletionOr<void> global_declaration_instantiation(VM&, GlobalEnvironment&);

    void drop_ast();

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
    Script(Realm&, StringView filename, RefPtr<Program>, HostDefined*);
    Script(Realm&, StringView filename, RustIntegration::ScriptResult&&, HostDefined*);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<Realm> m_realm;                       // [[Realm]]
    RefPtr<Program> m_parse_node;                 // [[ECMAScriptCode]]
    Vector<LoadedModuleRequest> m_loaded_modules; // [[LoadedModules]]

    mutable GC::Ptr<Bytecode::Executable> m_executable;

    Vector<Utf16FlyString> m_lexical_names;
    Vector<Utf16FlyString> m_var_names;
    Vector<FunctionToInitialize> m_functions_to_initialize;
    HashTable<Utf16FlyString> m_declared_function_names;
    Vector<Utf16FlyString> m_var_scoped_names;
    Vector<Utf16FlyString> m_annex_b_candidate_names;
    Vector<NonnullRefPtr<FunctionDeclaration>> m_annex_b_function_declarations;
    Vector<LexicalBinding> m_lexical_bindings;
    bool m_is_strict_mode { false };

    // Needed for potential lookups of modules.
    ByteString m_filename;
    HostDefined* m_host_defined { nullptr }; // [[HostDefined]]
};

}
