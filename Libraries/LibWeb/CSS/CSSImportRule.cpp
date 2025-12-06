/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/ScopeGuard.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/CSSImportRulePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSImportRule);

GC::Ref<CSSImportRule> CSSImportRule::create(JS::Realm& realm, URL url, GC::Ptr<DOM::Document> document, Optional<FlyString> layer, RefPtr<Supports> supports, GC::Ref<MediaList> media)
{
    return realm.create<CSSImportRule>(realm, move(url), document, move(layer), move(supports), move(media));
}

CSSImportRule::CSSImportRule(JS::Realm& realm, URL url, GC::Ptr<DOM::Document> document, Optional<FlyString> layer, RefPtr<Supports> supports, GC::Ref<MediaList> media)
    : CSSRule(realm, Type::Import)
    , m_url(move(url))
    , m_document(document)
    , m_layer(move(layer))
    , m_supports(move(supports))
    , m_media(move(media))
{
    if (m_layer.has_value() && m_layer->is_empty()) {
        m_layer_internal = CSSLayerBlockRule::next_unique_anonymous_layer_name();
    } else {
        m_layer_internal = m_layer;
    }
}

CSSImportRule::~CSSImportRule() = default;

void CSSImportRule::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSImportRule);
    Base::initialize(realm);
}

void CSSImportRule::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_media);
    visitor.visit(m_style_sheet);
}

void CSSImportRule::set_parent_style_sheet(CSSStyleSheet* parent_style_sheet)
{
    Base::set_parent_style_sheet(parent_style_sheet);

    if (m_style_sheet && parent_style_sheet) {
        for (auto owning_document_or_shadow_root : parent_style_sheet->owning_documents_or_shadow_roots())
            m_style_sheet->add_owning_document_or_shadow_root(*owning_document_or_shadow_root);
    }

    // Crude detection of whether we're already fetching.
    if (m_style_sheet || m_document_load_event_delayer.has_value())
        return;

    // Only try to fetch if we now have a parent
    if (parent_style_sheet)
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
    builder.append(m_url.to_string());

    // AD-HOC: Serialize the rule's layer if it exists.
    if (m_layer.has_value()) {
        if (m_layer->is_empty()) {
            builder.append(" layer"sv);
        } else {
            builder.appendff(" layer({})", m_layer);
        }
    }

    // AD-HOC: Serialize the rule's supports condition if it exists.
    //         This isn't currently specified, but major browsers include this in their serialization of import rules
    if (m_supports)
        builder.appendff(" supports({})", m_supports->to_string());

    // 3. If the rule’s associated media list is not empty, a single SPACE (U+0020) followed by the result of performing serialize a media query list on the media list.
    if (m_media->length() != 0)
        builder.appendff(" {}", m_media->media_text());

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
    auto parsed_url = DOMURL::parse(href(), parent_style_sheet.location());
    if (!parsed_url.has_value()) {
        dbgln("Unable to parse @import url `{}` parent location `{}` as a URL.", href(), parent_style_sheet.location());
        return;
    }

    // FIXME: Figure out the "correct" way to delay the load event.
    m_document_load_event_delayer.emplace(*m_document);

    // AD-HOC: Track pending import rules to block rendering until they are done.
    m_document->add_pending_css_import_rule({}, *this);

    // 4. Fetch a style resource from parsedUrl, with stylesheet parentStylesheet, destination "style", CORS mode "no-cors", and processResponse being the following steps given response response and byte stream, null or failure byteStream:
    (void)fetch_a_style_resource(parsed_url.value(), { parent_style_sheet }, Fetch::Infrastructure::Request::Destination::Style, CorsMode::NoCors,
        [strong_this = GC::Ref { *this }, parent_style_sheet = GC::Ref { parent_style_sheet }, parsed_url = parsed_url.value(), document = m_document](auto response, auto maybe_byte_stream) {
            // AD-HOC: Stop delaying the load event.
            ScopeGuard guard = [strong_this, document] {
                document->remove_pending_css_import_rule({}, strong_this);
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
            Optional<String> mime_type_charset;
            if (auto extracted_mime_type = Fetch::Infrastructure::extract_mime_type(response->header_list()); extracted_mime_type.has_value()) {
                if (auto charset = extracted_mime_type->parameters().get("charset"sv); charset.has_value())
                    mime_type_charset = charset.value();
            }
            // The environment encoding of an imported style sheet is the encoding of the style sheet that imported it. [css-syntax-3]
            // FIXME: Save encoding on Stylesheet to get it here
            Optional<StringView> environment_encoding;
            auto decoded_or_error = css_decode_bytes(environment_encoding, mime_type_charset, *byte_stream);
            if (decoded_or_error.is_error()) {
                dbgln_if(CSS_LOADER_DEBUG, "CSSImportRule: Failed to decode CSS file: {}", parsed_url);
                return;
            }
            auto decoded = decoded_or_error.release_value();

            auto imported_style_sheet = parse_css_stylesheet(Parser::ParsingParams(*strong_this->m_document), decoded, parsed_url, strong_this->m_media);

            // 5. Set importedStylesheet’s origin-clean flag to parentStylesheet’s origin-clean flag.
            imported_style_sheet->set_origin_clean(parent_style_sheet->is_origin_clean());

            // 6. If response is not CORS-same-origin, unset importedStylesheet’s origin-clean flag.
            if (!response->is_cors_cross_origin())
                imported_style_sheet->set_origin_clean(false);

            // 7. Set rule’s styleSheet to importedStylesheet.
            strong_this->set_style_sheet(imported_style_sheet);
        });
}

void CSSImportRule::set_style_sheet(GC::Ref<CSSStyleSheet> style_sheet)
{
    m_style_sheet = style_sheet;
    m_style_sheet->set_owner_css_rule(this);

    if (m_parent_style_sheet) {
        for (auto owning_document_or_shadow_root : m_parent_style_sheet->owning_documents_or_shadow_roots())
            m_style_sheet->add_owning_document_or_shadow_root(*owning_document_or_shadow_root);
    }

    m_style_sheet->invalidate_owners(DOM::StyleInvalidationReason::CSSImportRule);
}

// https://drafts.csswg.org/cssom/#dom-cssimportrule-media
GC::Ref<MediaList> CSSImportRule::media() const
{
    // The media attribute must return the value of the media attribute of the associated CSS style sheet.
    // AD-HOC: Return our own MediaList.
    //         https://github.com/w3c/csswg-drafts/issues/12063
    return m_media;
}

// https://drafts.csswg.org/cssom/#dom-cssimportrule-layername
Optional<FlyString> CSSImportRule::layer_name() const
{
    // The layerName attribute must return the layer name declared in the at-rule itself, or an empty string if the
    // layer is anonymous, or null if the at-rule does not declare a layer.
    if (!m_layer.has_value())
        return {};
    return m_layer;
}

// https://drafts.csswg.org/cssom/#dom-cssimportrule-supportstext
Optional<String> CSSImportRule::supports_text() const
{
    // The supportsText attribute must return the <supports-condition> declared in the at-rule itself, or null if the
    // at-rule does not declare a supports condition.
    if (!m_supports)
        return {};
    return m_supports->to_string();
}

Optional<FlyString> CSSImportRule::internal_qualified_layer_name(Badge<StyleScope>) const
{
    if (!m_layer.has_value())
        return {};

    auto const& parent_name = parent_layer_internal_qualified_name();
    if (parent_name.is_empty())
        return m_layer_internal.value();
    return MUST(String::formatted("{}.{}", parent_name, m_layer_internal.value()));
}

bool CSSImportRule::matches() const
{
    if (m_supports && !m_supports->matches())
        return false;
    return m_media->matches();
}

void CSSImportRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Document URL: {}\n", url().to_string());

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Has document load delayer: {}\n", m_document_load_event_delayer.has_value());

    if (m_layer.has_value()) {
        dump_indent(builder, indent_levels + 1);
        builder.appendff("Layer: `{}` (internal: `{}`)\n", *m_layer, *m_layer_internal);
    }

    if (m_media->length() != 0)
        m_media->dump(builder, indent_levels + 1);

    if (m_supports)
        m_supports->dump(builder, indent_levels + 1);

    if (m_style_sheet) {
        dump_sheet(builder, *m_style_sheet, indent_levels + 1);
    } else {
        dump_indent(builder, indent_levels + 1);
        builder.append("Style sheet not loaded\n"sv);
    }
}

}
