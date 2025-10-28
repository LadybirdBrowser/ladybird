/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Export.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

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
    Program const& parse_node() const { return *m_parse_node; }
    Vector<LoadedModuleRequest>& loaded_modules() { return m_loaded_modules; }
    Vector<LoadedModuleRequest> const& loaded_modules() const { return m_loaded_modules; }

    HostDefined* host_defined() const { return m_host_defined; }
    StringView filename() const LIFETIME_BOUND { return m_filename; }

private:
    Script(Realm&, StringView filename, NonnullRefPtr<Program>, HostDefined*);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<Realm> m_realm;                       // [[Realm]]
    NonnullRefPtr<Program> m_parse_node;          // [[ECMAScriptCode]]
    Vector<LoadedModuleRequest> m_loaded_modules; // [[LoadedModules]]

    // Needed for potential lookups of modules.
    ByteString m_filename;
    HostDefined* m_host_defined { nullptr }; // [[HostDefined]]
};

}
