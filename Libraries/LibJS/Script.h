/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/ParserError.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

// 16.1.4 Script Records, https://tc39.es/ecma262/#sec-script-records
class Script final : public Cell {
    JS_CELL(Script, Cell);
    JS_DECLARE_ALLOCATOR(Script);

public:
    struct HostDefined {
        virtual ~HostDefined() = default;

        virtual void visit_host_defined_self(Cell::Visitor&) = 0;
    };

    virtual ~Script() override;
    static Result<NonnullGCPtr<Script>, Vector<ParserError>> parse(StringView source_text, Realm&, StringView filename = {}, HostDefined* = nullptr, size_t line_number_offset = 1);

    Realm& realm() { return *m_realm; }
    Program const& parse_node() const { return *m_parse_node; }
    Vector<ModuleWithSpecifier>& loaded_modules() { return m_loaded_modules; }
    Vector<ModuleWithSpecifier> const& loaded_modules() const { return m_loaded_modules; }

    HostDefined* host_defined() const { return m_host_defined; }
    StringView filename() const { return m_filename; }

private:
    Script(Realm&, StringView filename, NonnullRefPtr<Program>, HostDefined* = nullptr);

    virtual void visit_edges(Cell::Visitor&) override;

    GCPtr<Realm> m_realm;                         // [[Realm]]
    NonnullRefPtr<Program> m_parse_node;          // [[ECMAScriptCode]]
    Vector<ModuleWithSpecifier> m_loaded_modules; // [[LoadedModules]]

    // Needed for potential lookups of modules.
    ByteString m_filename;
    HostDefined* m_host_defined { nullptr }; // [[HostDefined]]
};

}
