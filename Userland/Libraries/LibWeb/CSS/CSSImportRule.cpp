/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/CSSImportRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(CSSImportRule);

JS::NonnullGCPtr<CSSImportRule> CSSImportRule::create(URL::URL url, DOM::Document& document)
{
    auto& realm = document.realm();
    return realm.heap().allocate<CSSImportRule>(realm, move(url), document);
}

CSSImportRule::CSSImportRule(URL::URL url, DOM::Document& document)
    : CSSRule(document.realm())
    , m_url(move(url))
    , m_document(document)
{
    dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Loading import URL: {}", m_url);
    auto request = LoadRequest::create_for_url_on_page(m_url, &document.page());

    // NOTE: Mark this rule as delaying the document load event *before* calling set_resource()
    //       as it may trigger a synchronous resource_did_load() callback.
    m_document_load_event_delayer.emplace(document);

    set_resource(ResourceLoader::the().load_resource(Resource::Type::Generic, request));
}

void CSSImportRule::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSImportRule);
}

void CSSImportRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_style_sheet);
}

// https://www.w3.org/TR/cssom/#serialize-a-css-rule
String CSSImportRule::serialized() const
{
    StringBuilder builder;
    // The result of concatenating the following:

    // 1. The string "@import" followed by a single SPACE (U+0020).
    builder.append("@import "sv);

    // 2. The result of performing serialize a URL on the rule’s location.
    serialize_a_url(builder, MUST(m_url.to_string()));

    // FIXME: 3. If the rule’s associated media list is not empty, a single SPACE (U+0020) followed by the result of performing serialize a media query list on the media list.

    // 4. The string ";", i.e., SEMICOLON (U+003B).
    builder.append(';');

    return MUST(builder.to_string());
}

void CSSImportRule::resource_did_fail()
{
    dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Resource did fail. URL: {}", resource()->url());

    m_document_load_event_delayer.clear();
}

void CSSImportRule::resource_did_load()
{
    VERIFY(resource());

    if (!m_document)
        return;

    m_document_load_event_delayer.clear();

    if (!resource()->has_encoded_data()) {
        dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Resource did load, no encoded data. URL: {}", resource()->url());
    } else {
        dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Resource did load, has encoded data. URL: {}", resource()->url());
    }

    auto* sheet = parse_css_stylesheet(CSS::Parser::ParsingContext(*m_document, resource()->url()), resource()->encoded_data(), resource()->url());
    if (!sheet) {
        dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Failed to parse stylesheet: {}", resource()->url());
        return;
    }

    m_style_sheet = sheet;
    m_style_sheet->set_owner_css_rule(this);

    m_document->style_computer().invalidate_rule_cache();
    m_document->style_computer().load_fonts_from_sheet(*m_style_sheet);
    m_document->invalidate_style(DOM::StyleInvalidationReason::CSSImportRule);
}

}
