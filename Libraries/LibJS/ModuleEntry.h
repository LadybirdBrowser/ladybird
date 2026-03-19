/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibJS/Runtime/ModuleRequest.h>

namespace JS {

struct ImportEntry {
    Optional<Utf16FlyString> import_name;     // [[ImportName]]: stored string if Optional is not empty, NAMESPACE-OBJECT otherwise
    Utf16FlyString local_name;                // [[LocalName]]
    Optional<ModuleRequest> m_module_request; // [[ModuleRequest]]

    ImportEntry(Optional<Utf16FlyString> import_name_, Utf16FlyString local_name_)
        : import_name(move(import_name_))
        , local_name(move(local_name_))
    {
    }

    bool is_namespace() const { return !import_name.has_value(); }

    ModuleRequest const& module_request() const
    {
        return m_module_request.value();
    }
};

// ExportEntry Record, https://tc39.es/ecma262/#table-exportentry-records
struct ExportEntry {
    enum class Kind {
        NamedExport,
        ModuleRequestAll,
        ModuleRequestAllButDefault,
        // EmptyNamedExport is a special type for export {} from "module",
        // which should import the module without getting any of the exports
        // however we don't want give it a fake export name which may get
        // duplicates
        EmptyNamedExport,
    } kind;

    Optional<Utf16FlyString> export_name;          // [[ExportName]]
    Optional<Utf16FlyString> local_or_import_name; // Either [[ImportName]] or [[LocalName]]

    ExportEntry(Kind export_kind, Optional<Utf16FlyString> export_name_, Optional<Utf16FlyString> local_or_import_name_)
        : kind(export_kind)
        , export_name(move(export_name_))
        , local_or_import_name(move(local_or_import_name_))
    {
    }

    Optional<ModuleRequest> m_module_request; // [[ModuleRequest]]

    bool is_module_request() const
    {
        return m_module_request.has_value();
    }

    static ExportEntry indirect_export_entry(ModuleRequest module_request, Optional<Utf16FlyString> export_name, Optional<Utf16FlyString> import_name)
    {
        ExportEntry entry { Kind::NamedExport, move(export_name), move(import_name) };
        entry.m_module_request = move(module_request);
        return entry;
    }

    ModuleRequest const& module_request() const
    {
        return m_module_request.value();
    }

    static ExportEntry named_export(Utf16FlyString export_name, Utf16FlyString local_name)
    {
        return ExportEntry { Kind::NamedExport, move(export_name), move(local_name) };
    }

    static ExportEntry all_but_default_entry()
    {
        return ExportEntry { Kind::ModuleRequestAllButDefault, {}, {} };
    }

    static ExportEntry all_module_request(Utf16FlyString export_name)
    {
        return ExportEntry { Kind::ModuleRequestAll, move(export_name), {} };
    }

    static ExportEntry empty_named_export()
    {
        return ExportEntry { Kind::EmptyNamedExport, {}, {} };
    }
};

}
