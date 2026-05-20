/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/Parser/SpeculativeHTMLParser.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTMLTokenizerRustFFI.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SpeculativeHTMLParser);

GC::Ref<SpeculativeHTMLParser> SpeculativeHTMLParser::create(JS::Realm& realm, GC::Ref<DOM::Document> document, String pending_input, URL::URL base_url)
{
    return realm.create<SpeculativeHTMLParser>(document, move(pending_input), move(base_url));
}

SpeculativeHTMLParser::SpeculativeHTMLParser(GC::Ref<DOM::Document> document, String pending_input, URL::URL base_url)
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

    auto input = m_input.bytes_as_string_view();
    auto* bytes = reinterpret_cast<u8 const*>(input.characters_without_null_termination());
    if (bytes == nullptr)
        bytes = reinterpret_cast<u8 const*>("");

    rust_html_preload_scanner_scan(bytes, input.length(), this, [](void* context, RustFfiPreloadScannerEntry const* entry) -> bool {
        auto& parser = *static_cast<SpeculativeHTMLParser*>(context);
        if (parser.m_stopped || entry == nullptr)
            return false;

        parser.process_preload_scanner_entry(*entry);
        return !parser.m_stopped;
    });
}

namespace {

StringView ffi_string_view(u8 const* ptr, size_t len)
{
    if (ptr == nullptr || len == 0)
        return {};
    return { ptr, len };
}

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
    auto& vm = realm.vm();
    auto request = create_potential_CORS_request(vm, url, destination, cors_setting);
    request->set_client(&document.relevant_settings_object());

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    auto algorithms = Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input));

    // The fetch stays alive via ResourceLoader's GC::Root callbacks for the duration of the
    // network request, so we don't need to retain the FetchController.
    (void)Fetch::Fetching::fetch(realm, request, algorithms);
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
        if (auto parsed = m_document->encoding_parse_url(url_string); parsed.has_value())
            m_base_url = parsed.release_value();
        return;

    case RustFfiPreloadScannerAction::Fetch:
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
    issue_speculative_fetch(m_document->realm(), *m_document, *url, destination_from_preload_scanner(entry.destination), cors_setting_from_preload_scanner(entry.cors_setting));
}

}
