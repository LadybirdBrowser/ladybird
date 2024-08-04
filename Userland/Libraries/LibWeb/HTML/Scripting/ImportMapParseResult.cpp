/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ModuleRequest.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/ImportMapParseResult.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ImportMapParseResult);

ImportMapParseResult::ImportMapParseResult() = default;

ImportMapParseResult::~ImportMapParseResult() = default;

// https://html.spec.whatwg.org/multipage/webappapis.html#create-an-import-map-parse-result
JS::NonnullGCPtr<ImportMapParseResult> ImportMapParseResult::create(JS::Realm& realm, ByteString const& input, URL::URL base_url)
{
    // 1. Let result be an import map parse result whose import map is null and whose error to rethrow is null.
    auto result = realm.heap().allocate<ImportMapParseResult>(realm);

    // 2. Parse an import map string given input and baseURL, catching any exceptions.
    auto import_map = parse_import_map_string(realm, input, base_url);

    // 2.1. If this threw an exception, then set result's error to rethrow to that exception.
    if (import_map.is_exception())
        result->set_error_to_rethrow(import_map.exception());

    // 2.2. Otherwise, set result's import map to the return value.
    else
        result->set_import_map(import_map.release_value());

    // 3. Return result.
    return result;
}

void ImportMapParseResult::visit_host_defined_self(Visitor& visitor)
{
    visitor.visit(*this);
}

void ImportMapParseResult::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_error_to_rethrow.has_value()) {
        m_error_to_rethrow.value().visit(
            [&](WebIDL::SimpleException const&) {
                // ignore
            },
            [&](JS::NonnullGCPtr<WebIDL::DOMException> exception) {
                visitor.visit(exception);
            },
            [&](JS::Completion const& completion) {
                if (completion.value().has_value())
                    visitor.visit(completion.value().value());
            });
    }
}

// https://html.spec.whatwg.org/multipage/webappapis.html#register-an-import-map
void ImportMapParseResult::register_import_map(Window& global)
{
    // 1. If result's error to rethrow is not null, then report the exception given by result's error to rethrow and return.
    if (m_error_to_rethrow.has_value()) {
        auto completion = Web::Bindings::dom_exception_to_throw_completion(global.vm(), m_error_to_rethrow.value());
        HTML::report_exception(completion, global.realm());
        return;
    }

    // 2. Assert: global's import map is an empty import map.
    VERIFY(global.import_map().imports().is_empty() && global.import_map().scopes().is_empty());

    // 3. Set global's import map to result's import map.
    VERIFY(m_import_map.has_value());
    global.set_import_map(m_import_map.value());
}

}
