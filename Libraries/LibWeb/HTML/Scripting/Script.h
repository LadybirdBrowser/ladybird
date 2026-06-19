/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Script.h>
#include <LibJS/SourceCode.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/ScriptRegistry.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-script
class WEB_API Script
    : public JS::Cell
    , public JS::Script::HostDefined {
    GC_CELL(Script, JS::Cell);
    GC_DECLARE_ALLOCATOR(Script);

public:
    virtual ~Script() override;

    Optional<URL::URL> const& base_url() const { return m_base_url; }
    ByteString const& filename() const { return m_filename; }
    Utf16String const& display_filename() const { return m_display_filename; }

    EnvironmentSettingsObject& settings_object();

    [[nodiscard]] JS::Value error_to_rethrow() const { return m_error_to_rethrow; }
    void set_error_to_rethrow(JS::Value value) { m_error_to_rethrow = value; }

    [[nodiscard]] JS::Value parse_error() const { return m_parse_error; }
    void set_parse_error(JS::Value value) { m_parse_error = value; }

protected:
    Script(Optional<URL::URL> base_url, ByteString filename, EnvironmentSettingsObject&);

    virtual void visit_edges(Visitor&) override;

private:
    virtual bool is_script() const final { return true; }
    virtual void visit_host_defined_self(JS::Cell::Visitor&) override;

    Optional<URL::URL> m_base_url;
    ByteString m_filename;
    Utf16String m_display_filename;

    // https://html.spec.whatwg.org/multipage/webappapis.html#settings-object
    // An environment settings object, containing various settings that are shared with other scripts in the same context.
    GC::Ref<EnvironmentSettingsObject> m_settings;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-parse-error
    JS::Value m_parse_error;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-error-to-rethrow
    JS::Value m_error_to_rethrow;
};

void register_javascript_source(Script&, NonnullRefPtr<JS::SourceCode const>, ScriptRegistry::IsInlineSource, size_t source_line_number);

}

template<>
inline bool JS::Script::HostDefined::fast_is<Web::HTML::Script>() const { return is_script(); }
