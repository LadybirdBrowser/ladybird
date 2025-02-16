/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibJS/Script.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-script
// https://whatpr.org/html/9893/webappapis.html#concept-script
class Script
    : public JS::Cell
    , public JS::Script::HostDefined {
    GC_CELL(Script, JS::Cell);
    GC_DECLARE_ALLOCATOR(Script);

public:
    virtual ~Script() override;

    Optional<URL::URL> const& base_url() const { return m_base_url; }
    ByteString const& filename() const { return m_filename; }

    JS::Realm& realm() { return m_realm; }
    EnvironmentSettingsObject& settings_object();

    [[nodiscard]] JS::Value error_to_rethrow() const { return m_error_to_rethrow; }
    void set_error_to_rethrow(JS::Value value) { m_error_to_rethrow = value; }

    [[nodiscard]] JS::Value parse_error() const { return m_parse_error; }
    void set_parse_error(JS::Value value) { m_parse_error = value; }

protected:
    Script(Optional<URL::URL> base_url, ByteString filename, JS::Realm&);

    virtual void visit_edges(Visitor&) override;

private:
    virtual void visit_host_defined_self(JS::Cell::Visitor&) override;

    Optional<URL::URL> m_base_url;
    ByteString m_filename;
    GC::Ref<JS::Realm> m_realm;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-parse-error
    JS::Value m_parse_error;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-script-error-to-rethrow
    JS::Value m_error_to_rethrow;
};

}
