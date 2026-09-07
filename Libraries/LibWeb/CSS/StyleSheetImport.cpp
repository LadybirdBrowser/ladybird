/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/ScopeGuard.h>
#include <LibGC/Root.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

NonnullRefPtr<StyleSheetImport> StyleSheetImport::create(RustRule rule, GC::Ptr<DOM::Document> document)
{
    auto import = adopt_ref(*new StyleSheetImport(rule.identity(), RustImportRule { rule.payload().import_rule }, RustMediaList { Parser::ValueParserFFI::rust_media_list_retain(rule.payload().media) }, document));
    import->m_native_rule.emplace(move(rule));
    return import;
}

NonnullRefPtr<StyleSheetImport> StyleSheetImport::create(RustRuleView const& rule, GC::Ptr<DOM::Document> document)
{
    return adopt_ref(*new StyleSheetImport(rule.identity(), rule.import_rule(), rule.import_media(), document));
}

StyleSheetImport::StyleSheetImport(u64 identity, RustImportRule rule, RustMediaList media, GC::Ptr<DOM::Document> document)
    : m_identity(identity)
    , m_rule(move(rule))
    , m_document(document)
    , m_media_list(move(media))
{
}

RustRule const& StyleSheetImport::native_rule() const
{
    if (!m_native_rule.has_value()) {
        auto* rule = Parser::ValueParserFFI::rust_rule_materialize_import(m_identity, m_rule.handle(), m_media_list.handle());
        m_native_rule.emplace(rule);
        Parser::ValueParserFFI::rust_rule_release(rule);
    }
    return *m_native_rule;
}

void StyleSheetImport::set_loading_state(StyleSheetState::LoadingState loading_state)
{
    m_loading_state = loading_state;
    if (loading_state == StyleSheetState::LoadingState::Loaded || loading_state == StyleSheetState::LoadingState::Error) {
        if (auto style_sheet = m_parent_style_sheet.strong_ref())
            style_sheet->check_if_loading_completed();
    }
}

void StyleSheetImport::visit_edges(GC::Cell::Visitor& visitor)
{
    // NB: The parent is a weak back-reference, not an owning edge.
    visitor.ignore(m_parent_style_sheet);
    visitor.visit(m_document);
    visitor.visit(m_media.ptr());
    visitor.visit(m_style_sheet);
    visitor.visit(m_cssom_rule.ptr());
}

CSSImportRule& StyleSheetImport::cssom_rule() const
{
    if (!m_cssom_rule)
        m_cssom_rule = CSSImportRule::create(const_cast<StyleSheetImport&>(*this));
    return *m_cssom_rule;
}

void StyleSheetImport::set_cssom_rule(CSSImportRule& rule)
{
    VERIFY(!m_cssom_rule || &*m_cssom_rule == &rule);
    m_cssom_rule = &rule;
}

URL const& StyleSheetImport::url() const
{
    if (!m_cached_url.has_value()) {
        auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(m_rule.url_data())));
        m_cached_url = value->as_url().url();
    }
    return *m_cached_url;
}

void StyleSheetImport::set_parent_style_sheet(StyleSheetState* parent_style_sheet)
{
    if (m_parent_style_sheet) {
        m_parent_style_sheet->native_sheet().set_import(m_identity, nullptr);
        if (m_style_sheet && m_parent_style_sheet.ptr() != parent_style_sheet) {
            for (auto owner : m_parent_style_sheet->owning_documents_or_shadow_roots())
                m_style_sheet->remove_owning_document_or_shadow_root(*owner);
        }
        m_parent_style_sheet->remove_critical_subresource(*this);
    }

    m_parent_style_sheet = parent_style_sheet;
    if (m_style_sheet)
        m_style_sheet->set_parent_css_style_sheet(parent_style_sheet);

    if (m_parent_style_sheet)
        m_parent_style_sheet->add_critical_subresource(*this);

    if (m_style_sheet && parent_style_sheet) {
        parent_style_sheet->native_sheet().set_import(m_identity, &m_style_sheet->native_sheet());
        for (auto owning_document_or_shadow_root : parent_style_sheet->owning_documents_or_shadow_roots())
            m_style_sheet->add_owning_document_or_shadow_root(*owning_document_or_shadow_root);
    }

    if (loading_state() != StyleSheetState::LoadingState::Unloaded)
        return;

    // Only try to fetch if we now have a parent
    if (parent_style_sheet)
        fetch();
}

// https://drafts.csswg.org/css-cascade-4/#fetch-an-import
void StyleSheetImport::fetch()
{
    dbgln_if(CSS_LOADER_DEBUG, "StyleSheetImport: Loading import URL: {}", url());
    // To fetch an @import, given an @import rule rule:

    // 1. Let parentStylesheet be rule’s parent CSS style sheet. [CSSOM]
    VERIFY(parent_style_sheet());
    auto& parent_style_sheet = *this->parent_style_sheet();

    // 2. If rule has a <supports-condition>, and that condition is not true, return.
    if (auto supports = m_rule.supports(); supports.has_value() && !supports_condition_matches(*supports)) {
        set_loading_state(StyleSheetState::LoadingState::Loaded);
        return;
    }

    // AD-HOC: Track pending import rules to block rendering until they are done.
    m_document->add_pending_css_import_rule({}, *this);
    set_loading_state(StyleSheetState::LoadingState::Loading);

    // 3. Fetch a style resource from rule’s URL, with ruleOrDeclaration rule, destination "style", CORS mode "no-cors", and
    //    processResponse being the following steps given response response and byte stream, null or failure byteStream:
    RuleOrDeclaration rule_or_declaration {
        .environment_settings_object = HTML::relevant_settings_object(*m_document),
        .value = RuleOrDeclaration::Rule {
            .parent_style_sheet = &parent_style_sheet,
        },
        .style_resource_base_url = {},
        .parent_style_sheet_origin_clean = {},
    };
    (void)fetch_a_style_resource(URL { href() }, rule_or_declaration, Fetch::Infrastructure::Request::Destination::Style, CorsMode::NoCors,
        [strong_this = NonnullRefPtr { *this }, parent_style_sheet = NonnullRefPtr { parent_style_sheet }, document = m_document, load_event_delayer = DOM::DocumentLoadEventDelayer(*m_document, DOM::DocumentLoadEventDelayerReason::StyleSheetRequest)](auto response, auto maybe_byte_stream) mutable {
            // AD-HOC: Stop delaying the load event.
            auto finish_loading = [strong_this, document, load_event_delayer = move(load_event_delayer)] {
                auto imported_sheet = strong_this->m_style_sheet;
                document->remove_pending_css_import_rule({}, strong_this);
                if (!imported_sheet) {
                    strong_this->set_loading_state(StyleSheetState::LoadingState::Error);
                    return;
                }
                if (strong_this->loading_state() != StyleSheetState::LoadingState::Error) {
                    // If we have no critical subresources, or they're loaded already, we can report that immediately.
                    auto sheet_loading_state = imported_sheet->loading_state();
                    if (sheet_loading_state == StyleSheetState::LoadingState::Loaded || sheet_loading_state == StyleSheetState::LoadingState::Error) {
                        strong_this->set_loading_state(sheet_loading_state);
                    }
                }
            };
            ArmedScopeGuard guard = [&] { finish_loading(); };

            // 1. If byteStream is not a byte stream, return.
            auto byte_stream = maybe_byte_stream.template get_pointer<Core::ImmutableBytes>();
            if (!byte_stream) {
                // AD-HOC: This means the fetch failed, so we should report this as a load failure.
                strong_this->set_loading_state(StyleSheetState::LoadingState::Error);
                return;
            }

            // FIXME: 2. If parentStylesheet is in quirks mode and response is CORS-same-origin, let content type be "text/css".
            //           Otherwise, let content type be the Content Type metadata of response.
            auto content_type = "text/css"sv;

            // 3. If content type is not "text/css", return.
            if (content_type != "text/css"sv) {
                dbgln_if(CSS_LOADER_DEBUG, "StyleSheetImport: Rejecting loaded style sheet; content type isn't text/css; is: '{}'", content_type);
                return;
            }

            // 4. Let importedStylesheet be the result of parsing byteStream given parsedUrl.
            // FIXME: Tidy up our parsing API. For now, do the decoding here.
            // FIXME: Spec issue: parsedURL is not defined - we instead need to get that from the response.
            //        https://github.com/w3c/csswg-drafts/issues/12288
            auto url = response->unsafe_response()->url().value();

            Optional<StringView> mime_type_charset;
            auto extracted_mime_type = Fetch::Infrastructure::extract_mime_type(response->header_list());
            if (extracted_mime_type.has_value()) {
                if (auto charset = extracted_mime_type->parameters().get("charset"sv); charset.has_value())
                    mime_type_charset = charset->bytes_as_string_view();
            }
            // The environment encoding of an imported style sheet is the encoding of the style sheet that imported it. [css-syntax-3]
            // FIXME: Save encoding on Stylesheet to get it here
            Optional<StringView> environment_encoding;
            auto decoded_or_error = css_decode_bytes(environment_encoding, mime_type_charset, byte_stream->bytes());
            if (decoded_or_error.is_error()) {
                dbgln_if(CSS_LOADER_DEBUG, "StyleSheetImport: Failed to decode CSS file: {}", url);
                return;
            }
            Parser::Parser::parse_stylesheet_off_thread(
                Parser::ParsingParams { *document }, decoded_or_error.release_value(),
                [strong_this, parent_style_sheet, document = GC::make_root(document), url = move(url), is_cors_same_origin = response->is_cors_same_origin(), finish_loading = move(finish_loading)](Parser::RustStyleSheetParse parsed) mutable {
                    ScopeGuard guard = move(finish_loading);
                    Parser::Parser parser { Parser::ParsingParams { *document } };
                    auto imported_style_sheet = parser.create_css_stylesheet(parsed, move(url), strong_this->m_media_list.retain());

                    // 5. Set importedStylesheet’s origin-clean flag to parentStylesheet’s origin-clean flag.
                    imported_style_sheet->set_origin_clean(parent_style_sheet->is_origin_clean());

                    // 6. If response is not CORS-same-origin, unset importedStylesheet’s origin-clean flag.
                    if (!is_cors_same_origin)
                        imported_style_sheet->set_origin_clean(false);

                    // 7. Set rule’s styleSheet to importedStylesheet.
                    strong_this->set_style_sheet(imported_style_sheet);
                });
            guard.disarm();
        });
}

void StyleSheetImport::set_style_sheet(NonnullRefPtr<StyleSheetState> style_sheet)
{
    m_style_sheet = style_sheet;
    m_style_sheet->set_parent_css_style_sheet(m_parent_style_sheet.ptr());
    if (m_media)
        m_media->set_associated_style_sheet(style_sheet);
    m_style_sheet->set_owner_import(*this);
    if (m_parent_style_sheet)
        m_style_sheet->set_owner_node(m_parent_style_sheet->owner_node());

    if (m_parent_style_sheet) {
        m_parent_style_sheet->native_sheet().set_import(m_identity, &m_style_sheet->native_sheet());
        for (auto owning_document_or_shadow_root : m_parent_style_sheet->owning_documents_or_shadow_roots())
            m_style_sheet->add_owning_document_or_shadow_root(*owning_document_or_shadow_root);
    }

    auto document = m_style_sheet->owning_document();
    if (!document && m_parent_style_sheet)
        document = m_parent_style_sheet->owning_document();
    if (document)
        m_style_sheet->load_pending_image_resources(*document);

    // The rules the import brings in arrive now, at the position the import already holds.
    if (m_parent_style_sheet)
        record_imported_style_sheet_loaded(m_identity, *m_parent_style_sheet);
    m_style_sheet->invalidate_owners();
}

// https://drafts.csswg.org/cssom/#dom-cssimportrule-media
GC::Ref<MediaList> StyleSheetImport::media() const
{
    // The media attribute must return the value of the media attribute of the associated CSS style sheet.
    // AD-HOC: Return our own MediaList.
    //         https://github.com/w3c/csswg-drafts/issues/12063
    if (!m_media) {
        m_media = MediaList::create(m_media_list.retain());
        if (m_style_sheet)
            m_media->set_associated_style_sheet(*m_style_sheet);
        else
            m_media->set_associated_rule(cssom_rule());
    }
    return *m_media;
}

}
