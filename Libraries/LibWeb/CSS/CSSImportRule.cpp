/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/ScopeGuard.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/CSSImportRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSImportRule);

GC::Ref<CSSImportRule> CSSImportRule::create(URL::URL url, DOM::Document& document, RefPtr<Supports> supports, Vector<NonnullRefPtr<MediaQuery>> media_query_list)
{
    auto& realm = document.realm();
    return realm.create<CSSImportRule>(move(url), document, supports, move(media_query_list));
}

CSSImportRule::CSSImportRule(URL::URL url, DOM::Document& document, RefPtr<Supports> supports, Vector<NonnullRefPtr<MediaQuery>> media_query_list)
    : CSSRule(document.realm(), Type::Import)
    , m_url(move(url))
    , m_document(document)
    , m_supports(supports)
    , m_media_query_list(move(media_query_list))
{
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

void CSSImportRule::set_parent_style_sheet(CSSStyleSheet* parent_style_sheet)
{
    Base::set_parent_style_sheet(parent_style_sheet);
    // Crude detection of whether we're already fetching.
    if (m_style_sheet || m_document_load_event_delayer.has_value())
        return;
    fetch();
}

// https://www.w3.org/TR/cssom/#serialize-a-css-rule
String CSSImportRule::serialized() const
{
    StringBuilder builder;
    // The result of concatenating the following:

    // 1. The string "@import" followed by a single SPACE (U+0020).
    builder.append("@import "sv);

    // 2. The result of performing serialize a URL on the rule’s location.
    serialize_a_url(builder, m_url.to_string());

    // AD-HOC: Serialize the rule's supports condition if it exists.
    //         This isn't currently specified, but major browsers include this in their serialization of import rules
    if (m_supports)
        builder.appendff(" supports({})", m_supports->to_string());

    // 3. If the rule’s associated media list is not empty, a single SPACE (U+0020) followed by the result of performing serialize a media query list on the media list.
    if (!m_media_query_list.is_empty())
        builder.appendff(" {}", serialize_a_media_query_list(m_media_query_list));

    // 4. The string ";", i.e., SEMICOLON (U+003B).
    builder.append(';');

    return MUST(builder.to_string());
}

// https://drafts.csswg.org/css-cascade-4/#fetch-an-import
void CSSImportRule::fetch()
{
    dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Loading import URL: {}", m_url);
    // To fetch an @import, given an @import rule rule:

    // 1. Let parentStylesheet be rule’s parent CSS style sheet. [CSSOM]
    VERIFY(parent_style_sheet());
    auto& parent_style_sheet = *this->parent_style_sheet();

    // 2. If rule has a <supports-condition>, and that condition is not true, return.
    if (m_supports && !m_supports->matches()) {
        return;
    }

    // 3. Let parsedUrl be the result of the URL parser steps with rule’s URL and parentStylesheet’s location.
    //    If the algorithm returns an error, return. [CSSOM]
    // FIXME: Stop producing a URL::URL when parsing the @import
    auto parsed_url = url().to_string();

    // FIXME: Figure out the "correct" way to delay the load event.
    m_document_load_event_delayer.emplace(*m_document);

    // 4. Fetch a style resource from parsedUrl, with stylesheet parentStylesheet, destination "style", CORS mode "no-cors", and processResponse being the following steps given response response and byte stream, null or failure byteStream:
    fetch_a_style_resource(parsed_url, parent_style_sheet, Fetch::Infrastructure::Request::Destination::Style, CorsMode::NoCors,
        [strong_this = GC::Ref { *this }, parent_style_sheet = GC::Ref { parent_style_sheet }](auto response, auto maybe_byte_stream) {
            // AD-HOC: Stop delaying the load event.
            ScopeGuard guard = [strong_this] {
                strong_this->m_document_load_event_delayer.clear();
            };

            // 1. If byteStream is not a byte stream, return.
            auto byte_stream = maybe_byte_stream.template get_pointer<ByteBuffer>();
            if (!byte_stream)
                return;

            // FIXME: 2. If parentStylesheet is in quirks mode and response is CORS-same-origin, let content type be "text/css".
            //           Otherwise, let content type be the Content Type metadata of response.
            auto content_type = "text/css"sv;

            // 3. If content type is not "text/css", return.
            if (content_type != "text/css"sv) {
                dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Rejecting loaded style sheet; content type isn't text/css; is: '{}'", content_type);
                return;
            }

            // 4. Let importedStylesheet be the result of parsing byteStream given parsedUrl.
            // FIXME: Tidy up our parsing API. For now, do the decoding here.
            // FIXME: Get the encoding from the response somehow.
            auto encoding = "utf-8"sv;
            auto maybe_decoder = TextCodec::decoder_for(encoding);
            if (!maybe_decoder.has_value()) {
                dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Failed to decode CSS file: {} Unsupported encoding: {}", strong_this->url(), encoding);
                return;
            }
            auto& decoder = maybe_decoder.release_value();

            auto decoded_or_error = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(decoder, *byte_stream);
            if (decoded_or_error.is_error()) {
                dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Failed to decode CSS file: {} Encoding was: {}", strong_this->url(), encoding);
                return;
            }
            auto decoded = decoded_or_error.release_value();

            auto* imported_style_sheet = parse_css_stylesheet(Parser::ParsingParams(*strong_this->m_document, strong_this->url()), decoded, strong_this->url(), strong_this->m_media_query_list);

            // 5. Set importedStylesheet’s origin-clean flag to parentStylesheet’s origin-clean flag.
            imported_style_sheet->set_origin_clean(parent_style_sheet->is_origin_clean());

            // 6. If response is not CORS-same-origin, unset importedStylesheet’s origin-clean flag.
            if (!response->is_cors_cross_origin())
                imported_style_sheet->set_origin_clean(false);

            // 7. Set rule’s styleSheet to importedStylesheet.
            strong_this->set_style_sheet(*imported_style_sheet);
        });
}

void CSSImportRule::set_style_sheet(GC::Ref<CSSStyleSheet> style_sheet)
{
    m_style_sheet = style_sheet;
    m_style_sheet->set_owner_css_rule(this);
    m_document->style_computer().invalidate_rule_cache();
    m_document->style_computer().load_fonts_from_sheet(*m_style_sheet);
    m_document->invalidate_style(DOM::StyleInvalidationReason::CSSImportRule);
}

// https://drafts.csswg.org/cssom/#dom-cssimportrule-media
GC::Ptr<MediaList> CSSImportRule::media() const
{
    // The media attribute must return the value of the media attribute of the associated CSS style sheet.
    if (!m_style_sheet)
        return nullptr;
    return m_style_sheet->media();
}

Optional<String> CSSImportRule::supports_text() const
{
    if (!m_supports)
        return {};
    return m_supports->to_string();
}

}
