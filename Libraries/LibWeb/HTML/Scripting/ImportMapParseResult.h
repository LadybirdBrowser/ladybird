/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibJS/Script.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/ImportMap.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#import-map-parse-result
class ImportMapParseResult
    : public JS::Cell
    , public JS::Script::HostDefined {
    JS_CELL(ImportMapParseResult, JS::Cell);
    JS_DECLARE_ALLOCATOR(ImportMapParseResult);

public:
    virtual ~ImportMapParseResult() override;

    static JS::NonnullGCPtr<ImportMapParseResult> create(JS::Realm& realm, ByteString const& input, URL::URL base_url);

    [[nodiscard]] Optional<ImportMap> const& import_map() const { return m_import_map; }
    void set_import_map(ImportMap const& value) { m_import_map = value; }

    [[nodiscard]] Optional<WebIDL::Exception> const& error_to_rethrow() const { return m_error_to_rethrow; }
    void set_error_to_rethrow(WebIDL::Exception const& value) { m_error_to_rethrow = value; }

    void register_import_map(Window& global);

protected:
    ImportMapParseResult();

    virtual void visit_edges(Visitor&) override;

private:
    virtual void visit_host_defined_self(Visitor&) override;

    // https://html.spec.whatwg.org/multipage/webappapis.html#impr-import-map
    Optional<ImportMap> m_import_map;

    // https://html.spec.whatwg.org/multipage/webappapis.html#impr-error-to-rethrow
    Optional<WebIDL::Exception> m_error_to_rethrow;
};

}
