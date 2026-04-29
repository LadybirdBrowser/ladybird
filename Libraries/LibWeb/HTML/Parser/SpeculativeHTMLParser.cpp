/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Parser/SpeculativeHTMLParser.h>
#include <LibWeb/HTML/Parser/SpeculativeMockElement.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/PreloadEntry.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SpeculativeHTMLParser);

GC::Ref<SpeculativeHTMLParser> SpeculativeHTMLParser::create(JS::Realm& realm, GC::Ref<DOM::Document> document, String pending_input, URL::URL base_url)
{
    return realm.create<SpeculativeHTMLParser>(document, move(pending_input), move(base_url));
}

SpeculativeHTMLParser::SpeculativeHTMLParser(GC::Ref<DOM::Document> document, String pending_input, URL::URL base_url)
    : m_document(document)
    , m_input(move(pending_input))
    , m_tokenizer(m_input.bytes_as_string_view(), "UTF-8"sv)
    , m_base_url(move(base_url))
{
}

SpeculativeHTMLParser::~SpeculativeHTMLParser() = default;

void SpeculativeHTMLParser::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    m_tokenizer.visit_edges(visitor);
}

void SpeculativeHTMLParser::stop()
{
    // https://html.spec.whatwg.org/multipage/parsing.html#stop-the-speculative-html-parser
    // 3. Throw away any pending content in speculativeParser's input stream, and discard any future content
    //    that would have been added to it.
    m_tokenizer.abort();
}

void SpeculativeHTMLParser::run()
{
    while (true) {
        auto token = m_tokenizer.next_token();
        if (!token.has_value())
            break;

        if (token->is_start_tag()) {
            process_start_tag(*token);
        } else if (token->is_end_tag()) {
            process_end_tag(*token);
        } else if (token->is_end_of_file()) {
            break;
        }
    }
}

namespace {

Vector<HTMLToken::Attribute> attributes_from_token(HTMLToken const& token)
{
    Vector<HTMLToken::Attribute> attributes;
    token.for_each_attribute([&](HTMLToken::Attribute const& attribute) {
        attributes.append(attribute);
        return IterationDecision::Continue;
    });
    return attributes;
}

// https://html.spec.whatwg.org/multipage/parsing.html#speculative-fetch
// Step 4 says "fetch url as if the element was processed normally". For the regular parser to
// dedup against the in-flight speculative fetch, we follow the preload algorithm's shape: create
// a preload entry, register it under create_a_preload_key(request), and supply a
// processResponseConsumeBody callback that populates the entry. Then when the regular parser later
// processes the element, fetch()'s consume_a_preloaded_resource() check joins the entry rather than
// issuing a duplicate request.
void issue_speculative_fetch(JS::Realm& realm, DOM::Document& document, URL::URL url, Optional<Fetch::Infrastructure::Request::Destination> destination, CORSSettingAttribute cors_setting)
{
    auto& vm = realm.vm();
    auto request = create_potential_CORS_request(vm, url, destination, cors_setting);
    request->set_client(&document.relevant_settings_object());

    // Mirrors the preload algorithm step 6 ("Let entry be a new preload entry...") and step 7
    // ("Let key be the result of creating a preload key given request"):
    // https://html.spec.whatwg.org/multipage/links.html#preload
    auto entry = realm.create<PreloadEntry>();
    auto key = create_a_preload_key(*request);

    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response_consume_body = [&realm, entry](GC::Ref<Fetch::Infrastructure::Response> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes body_bytes) {
        // 1. If bodyBytes is a byte sequence, then set response's body to bodyBytes as a body.
        // 2. Otherwise, set response to a network error.
        // 5. If entry's on response available is null, then set entry's response to response;
        //    otherwise call entry's on response available given response.
        // (No processResponse, no reportTiming — those steps only apply to <link rel=preload>.)
        (void)deliver_preload_response(realm, *entry, response, body_bytes.get_pointer<ByteBuffer>());
    };
    auto algorithms = Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input));

    // The fetch stays alive via ResourceLoader's GC::Root callbacks for the duration of the
    // network request, so we don't need to retain the FetchController.
    (void)Fetch::Fetching::fetch(realm, request, algorithms);

    // Mirrors the preload algorithm step 12.2 ("Set document's map of preloaded resources[key] to
    // entry"). Note: the insert happens *after* fetch() starts because fetch() runs
    // consume_a_preloaded_resource() synchronously — registering the entry beforehand would let
    // the speculative fetch short-circuit itself.
    document.map_of_preloaded_resources().set(key, entry);
}

bool rel_contains_keyword(StringView rel, StringView keyword)
{
    for (auto token : rel.split_view_if(Infra::is_ascii_whitespace)) {
        if (token.equals_ignoring_ascii_case(keyword))
            return true;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/parsing.html#speculative-fetch
void speculative_fetch(SpeculativeMockElement& element, DOM::Document& document, URL::URL& base_url)
{
    auto& realm = document.realm();

    // 1. If the speculative HTML parser encounters one of the following elements, then act as if that
    //    element is processed for the purpose of its effect on subsequent speculative fetches.
    //    - A base element.
    if (element.local_name == HTML::TagNames::base) {
        if (auto href = element.attribute(HTML::AttributeNames::href); href.has_value() && !href->is_empty()) {
            if (auto parsed = document.encoding_parse_url(*href); parsed.has_value())
                base_url = parsed.release_value();
        }
        return;
    }
    // FIXME: A meta element whose http-equiv attribute is in the Content security policy state.
    // FIXME: A meta element whose name attribute is referrer.
    // FIXME: A meta element whose name attribute is viewport.

    // 2. Let url be the URL that element would fetch if it was processed normally. If there is no such
    //    URL or if it is the empty string, then do nothing.
    // We resolve URLs against the speculative parser's tracked base_url (which may have been updated
    // by an earlier speculative <base href>); this is why we use complete_url here rather than
    // document.encoding_parse_url, which would resolve against the document's base instead.
    Optional<URL::URL> url;
    Optional<Fetch::Infrastructure::Request::Destination> destination;
    auto cors_setting = cors_setting_attribute_from_keyword(element.attribute(HTML::AttributeNames::crossorigin));

    if (element.local_name == HTML::TagNames::script) {
        auto src = element.attribute(HTML::AttributeNames::src);
        if (!src.has_value() || src->is_empty())
            return;
        url = base_url.complete_url(*src);
        destination = Fetch::Infrastructure::Request::Destination::Script;
    } else if (element.local_name == HTML::TagNames::link) {
        auto rel = element.attribute(HTML::AttributeNames::rel);
        auto href = element.attribute(HTML::AttributeNames::href);
        if (!href.has_value() || href->is_empty() || !rel.has_value())
            return;
        auto rel_view = rel->bytes_as_string_view();
        if (rel_contains_keyword(rel_view, "stylesheet"sv)) {
            url = base_url.complete_url(*href);
            destination = Fetch::Infrastructure::Request::Destination::Style;
        } else if (rel_contains_keyword(rel_view, "preload"sv)) {
            auto translated = translate_a_preload_destination(element.attribute(HTML::AttributeNames::as));
            if (translated.has<Empty>())
                return;
            destination = translated.get<Optional<Fetch::Infrastructure::Request::Destination>>();
            url = base_url.complete_url(*href);
        } else {
            return;
        }
    } else if (element.local_name == HTML::TagNames::img) {
        auto src = element.attribute(HTML::AttributeNames::src);
        if (!src.has_value() || src->is_empty())
            return;
        url = base_url.complete_url(*src);
        destination = Fetch::Infrastructure::Request::Destination::Image;
    } else {
        return;
    }

    if (!url.has_value())
        return;

    // 3. Otherwise, if url is already in the list of speculative fetch URLs, then do nothing.
    if (document.has_speculative_fetch_url(*url))
        return;

    // 4. Otherwise, fetch url as if the element was processed normally, and add url to the list of
    //    speculative fetch URLs.
    document.add_speculative_fetch_url(*url);
    issue_speculative_fetch(realm, document, *url, destination, cors_setting);
}

}

void SpeculativeHTMLParser::process_start_tag(HTMLToken const& token)
{
    auto const& tag_name = token.tag_name();

    if (tag_name == HTML::TagNames::template_) {
        ++m_template_depth;
        return;
    }

    if (tag_name == HTML::TagNames::svg || tag_name == HTML::TagNames::math) {
        ++m_foreign_depth;
        return;
    }

    if (m_template_depth > 0 || m_foreign_depth > 0)
        return;

    if (!tag_name.is_one_of(HTML::TagNames::script, HTML::TagNames::link, HTML::TagNames::img, HTML::TagNames::base))
        return;

    auto element = create_a_speculative_mock_element(tag_name, attributes_from_token(token));

    // 6. Optionally, perform a speculative fetch for element.
    speculative_fetch(element, *m_document, m_base_url);
}

void SpeculativeHTMLParser::process_end_tag(HTMLToken const& token)
{
    auto const& tag_name = token.tag_name();
    if (tag_name == HTML::TagNames::template_ && m_template_depth > 0) {
        --m_template_depth;
    } else if ((tag_name == HTML::TagNames::svg || tag_name == HTML::TagNames::math) && m_foreign_depth > 0) {
        --m_foreign_depth;
    }
}

}
