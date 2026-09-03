/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/FFIHelpers.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/Parser/SpeculativeHTMLParser.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTMLTokenizerRustFFI.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SpeculativeHTMLParser);

GC::Ref<SpeculativeHTMLParser> SpeculativeHTMLParser::create(GC::Ref<DOM::Document> document, Utf16String pending_input, URL::URL base_url)
{
    return document->relevant_settings_object().realm().create<SpeculativeHTMLParser>(document, move(pending_input), move(base_url));
}

SpeculativeHTMLParser::SpeculativeHTMLParser(GC::Ref<DOM::Document> document, Utf16String pending_input, URL::URL base_url)
    : m_document(document)
    , m_input(move(pending_input))
    , m_base_url(move(base_url))
{
}

SpeculativeHTMLParser::~SpeculativeHTMLParser() = default;

void SpeculativeHTMLParser::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

void SpeculativeHTMLParser::stop()
{
    // https://html.spec.whatwg.org/multipage/parsing.html#stop-the-speculative-html-parser
    // 3. Throw away any pending content in speculativeParser's input stream, and discard any future content
    //    that would have been added to it.
    m_stopped = true;
}

void SpeculativeHTMLParser::run()
{
    if (m_stopped)
        return;

    auto scanner_callback = [](void* context, RustFfiPreloadScannerEntry const* entry) -> bool {
        auto& parser = *static_cast<SpeculativeHTMLParser*>(context);
        if (parser.m_stopped || entry == nullptr)
            return false;

        parser.process_preload_scanner_entry(*entry);
        return !parser.m_stopped;
    };

    auto input = m_input.utf16_view();
    if (input.has_ascii_storage()) {
        auto bytes = input.bytes();
        rust_html_preload_scanner_scan(bytes.data(), bytes.size(), this, scanner_callback);
    } else {
        auto code_units = input.utf16_span();
        rust_html_preload_scanner_scan_utf16(reinterpret_cast<u16 const*>(code_units.data()), code_units.size(), this, scanner_callback);
    }
}

namespace {

Optional<Fetch::Infrastructure::Request::Destination> destination_from_preload_scanner(RustFfiPreloadScannerDestination destination)
{
    switch (destination) {
    case RustFfiPreloadScannerDestination::None:
        return {};
    case RustFfiPreloadScannerDestination::Font:
        return Fetch::Infrastructure::Request::Destination::Font;
    case RustFfiPreloadScannerDestination::Image:
        return Fetch::Infrastructure::Request::Destination::Image;
    case RustFfiPreloadScannerDestination::Script:
        return Fetch::Infrastructure::Request::Destination::Script;
    case RustFfiPreloadScannerDestination::Style:
        return Fetch::Infrastructure::Request::Destination::Style;
    case RustFfiPreloadScannerDestination::Track:
        return Fetch::Infrastructure::Request::Destination::Track;
    case RustFfiPreloadScannerDestination::AudioWorklet:
        return Fetch::Infrastructure::Request::Destination::AudioWorklet;
    case RustFfiPreloadScannerDestination::JSON:
        return Fetch::Infrastructure::Request::Destination::JSON;
    case RustFfiPreloadScannerDestination::PaintWorklet:
        return Fetch::Infrastructure::Request::Destination::PaintWorklet;
    case RustFfiPreloadScannerDestination::ServiceWorker:
        return Fetch::Infrastructure::Request::Destination::ServiceWorker;
    case RustFfiPreloadScannerDestination::SharedWorker:
        return Fetch::Infrastructure::Request::Destination::SharedWorker;
    case RustFfiPreloadScannerDestination::Worker:
        return Fetch::Infrastructure::Request::Destination::Worker;
    case RustFfiPreloadScannerDestination::Text:
        return Fetch::Infrastructure::Request::Destination::Text;
    }
    VERIFY_NOT_REACHED();
}

CORSSettingAttribute cors_setting_from_preload_scanner(RustFfiPreloadScannerCorsSetting cors_setting)
{
    switch (cors_setting) {
    case RustFfiPreloadScannerCorsSetting::NoCors:
        return CORSSettingAttribute::NoCORS;
    case RustFfiPreloadScannerCorsSetting::Anonymous:
        return CORSSettingAttribute::Anonymous;
    case RustFfiPreloadScannerCorsSetting::UseCredentials:
        return CORSSettingAttribute::UseCredentials;
    }
    VERIFY_NOT_REACHED();
}

void issue_speculative_fetch(JS::Realm& realm, DOM::Document& document, URL::URL url, Optional<Fetch::Infrastructure::Request::Destination> destination, CORSSettingAttribute cors_setting)
{
    auto request = create_potential_CORS_request(url, destination, cors_setting);
    request->set_client(&document.relevant_settings_object());

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    auto algorithms = Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input));

    // The fetch stays alive via ResourceLoader's GC::Root callbacks for the duration of the
    // network request, so we don't need to retain the FetchController.
    (void)Fetch::Fetching::fetch(realm, request, algorithms);
}

Utf16String utf16_string_from_preload_scanner(u8 const* pointer, size_t length)
{
    if (length == 0)
        return {};
    return Utf16String::from_utf8(ffi_string_view(pointer, length));
}

bool media_attribute_matches_environment(DOM::Document const& document, RustFfiPreloadScannerEntry const& entry)
{
    auto media = utf16_string_from_preload_scanner(entry.media_ptr, entry.media_len);
    if (media.is_empty())
        return true;

    auto media_queries = parse_media_query_list(CSS::Parser::ParsingParams(document), media);
    for (auto const& media_query : media_queries) {
        if (media_query->evaluate(document))
            return true;
    }
    return false;
}

// Speculatively fetches url the way "fetch a single module script" would for a module script element or a modulepreload
// link (a CORS-mode request carrying the script fetch options the element's attributes give) — so, the response is the
// one the real fetch later finds in the HTTP cache. A potential-CORS request won't work here; a module script is always
// fetched in CORS mode, and a server that only sends Access-Control-Allow-Origin in reply to an Origin header answers
// a no-cors request without it. So the real fetch, served that response from the cache, fails its CORS check.
// https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script
void issue_speculative_module_fetch(DOM::Document& document, URL::URL const& url, RustFfiPreloadScannerEntry const& entry, Fetch::Infrastructure::Request::Destination destination, Fetch::Infrastructure::Request::ParserMetadata parser_metadata)
{
    auto& settings_object = document.relevant_settings_object();
    auto integrity_metadata = entry.integrity_present
        ? utf16_string_from_preload_scanner(entry.integrity_ptr, entry.integrity_len)
        : resolve_a_module_integrity_metadata(url, settings_object);
    ScriptFetchOptions options {
        .cryptographic_nonce = utf16_string_from_preload_scanner(entry.nonce_ptr, entry.nonce_len),
        .integrity_metadata = move(integrity_metadata),
        .parser_metadata = parser_metadata,
        .credentials_mode = cors_settings_attribute_credentials_mode(cors_setting_from_preload_scanner(entry.cors_setting)),
        .referrer_policy = ReferrerPolicy::from_string(utf16_string_from_preload_scanner(entry.referrer_policy_ptr, entry.referrer_policy_len)).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString),
        .fetch_priority = Fetch::Infrastructure::request_priority_from_string(utf16_string_from_preload_scanner(entry.fetch_priority_ptr, entry.fetch_priority_len)).value_or(Fetch::Infrastructure::Request::Priority::Auto),
    };

    // NB: A speculative fetch must not populate the module map. A CSP meta element can appear between the parser's
    //     current position and this link, and the real modulepreload fetch must apply that policy before it can reuse
    //     the response. This request may warm the HTTP cache, while the real link remains authoritative.
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(url);
    request->set_mode(Fetch::Infrastructure::Request::Mode::CORS);
    request->set_referrer(Fetch::Infrastructure::Request::Referrer::Client);
    request->set_client(&settings_object);
    request->set_destination(destination);
    if (first_is_one_of(destination,
            Fetch::Infrastructure::Request::Destination::Worker,
            Fetch::Infrastructure::Request::Destination::SharedWorker,
            Fetch::Infrastructure::Request::Destination::ServiceWorker))
        request->set_mode(Fetch::Infrastructure::Request::Mode::SameOrigin);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Script);
    set_up_module_script_request(request, options);

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    auto algorithms = Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input));
    (void)Fetch::Fetching::fetch(settings_object.realm(), request, algorithms);
}

// https://html.spec.whatwg.org/multipage/links.html#link-type-modulepreload
void issue_speculative_modulepreload_fetch(DOM::Document& document, URL::URL const& url, RustFfiPreloadScannerEntry const& entry)
{
    auto destination = destination_from_preload_scanner(entry.destination);
    VERIFY(destination.has_value());

    // The link's script fetch options have parser metadata "not-parser-inserted".
    issue_speculative_module_fetch(document, url, entry, *destination, Fetch::Infrastructure::Request::ParserMetadata::NotParserInserted);
}

// https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element
// A module script element's src is fetched as an external module script graph, whose root request "fetch a single
// module script" builds with destination "script" — and with parser metadata "parser-inserted", since the parser
// inserts the element the speculative parser found.
void issue_speculative_module_script_fetch(DOM::Document& document, URL::URL const& url, RustFfiPreloadScannerEntry const& entry)
{
    issue_speculative_module_fetch(document, url, entry, Fetch::Infrastructure::Request::Destination::Script, Fetch::Infrastructure::Request::ParserMetadata::ParserInserted);
}

}

void SpeculativeHTMLParser::process_preload_scanner_entry(RustFfiPreloadScannerEntry const& entry)
{
    auto url_string = ffi_string_view(entry.url_ptr, entry.url_len);
    if (url_string.is_empty())
        return;

    // https://html.spec.whatwg.org/multipage/parsing.html#speculative-fetch
    switch (entry.action) {
    case RustFfiPreloadScannerAction::Base:
        // 1. If the speculative HTML parser encounters one of the following elements, then act as if that
        //    element is processed for the purpose of its effect on subsequent speculative fetches.
        //    - A base element.
        if (auto parsed = m_document->encoding_parse_url(Utf16String::from_utf8(url_string)); parsed.has_value())
            m_base_url = parsed.release_value();
        return;

    case RustFfiPreloadScannerAction::Fetch:
    case RustFfiPreloadScannerAction::ModuleScript:
        break;
    case RustFfiPreloadScannerAction::ModulePreload:
        if (!media_attribute_matches_environment(*m_document, entry))
            return;
        break;
    }

    // FIXME: A meta element whose http-equiv attribute is in the Content security policy state.
    // FIXME: A meta element whose name attribute is referrer.
    // FIXME: A meta element whose name attribute is viewport.

    // 2. Let url be the URL that element would fetch if it was processed normally. If there is no such
    //    URL or if it is the empty string, then do nothing.
    // We resolve URLs against the speculative parser's tracked base_url (which may have been updated
    // by an earlier speculative <base href>); this is why we use complete_url here rather than
    // document.encoding_parse_url, which would resolve against the document's base instead.
    auto url = m_base_url.complete_url(url_string);
    if (!url.has_value())
        return;

    // 3. Otherwise, if url is already in the list of speculative fetch URLs, then do nothing.
    if (m_document->has_speculative_fetch_url(*url))
        return;

    // 4. Otherwise, fetch url as if the element was processed normally, and add url to the list of
    //    speculative fetch URLs.
    m_document->add_speculative_fetch_url(*url);
    if (entry.action == RustFfiPreloadScannerAction::ModulePreload)
        issue_speculative_modulepreload_fetch(*m_document, *url, entry);
    else if (entry.action == RustFfiPreloadScannerAction::ModuleScript)
        issue_speculative_module_script_fetch(*m_document, *url, entry);
    else
        issue_speculative_fetch(m_document->relevant_settings_object().realm(), *m_document, *url, destination_from_preload_scanner(entry.destination), cors_setting_from_preload_scanner(entry.cors_setting));
}

}
