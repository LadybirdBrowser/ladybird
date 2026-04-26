/*
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/Bindings/SVGScriptElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGScriptElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGScriptElement);

SVGScriptElement::SVGScriptElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGScriptElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGScriptElement);
    Base::initialize(realm);
}

void SVGScriptElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_script);
}

void SVGScriptElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (name == SVG::AttributeNames::href || name == SVG::AttributeNames::type) {
        process_the_script_element();
    }
}

void SVGScriptElement::inserted()
{
    Base::inserted();
    if (m_parser_inserted)
        return;

    process_the_script_element();
}

void SVGScriptElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    if (m_parser_inserted)
        return;

    process_the_script_element();
}

// https://www.w3.org/TR/SVGMobile12/script.html#ScriptContentProcessing
void SVGScriptElement::process_the_script_element()
{
    // 1. If the 'script' element's "already processed" flag is true or if the element is not in the
    //    document tree, then no action is performed and these steps are ended.
    if (m_already_processed || !in_a_document_tree())
        return;

    // https://html.spec.whatwg.org/multipage/webappapis.html#enabling-and-disabling-scripting
    if (is_scripting_disabled())
        return;

    // https://svgwg.org/svg2-draft/interact.html#ScriptElement
    // Before attempting to execute the ‘script’ element the resolved media type value for ‘type’ must be inspected.
    // If the SVG user agent does not support the scripting language then the ‘script’ element must not be executed.
    // FIXME: Support type="module" scripts
    auto maybe_script_type = attribute(SVG::AttributeNames::type);
    if (maybe_script_type.has_value() && !maybe_script_type->is_empty()) {
        auto script_type = MUST(maybe_script_type->to_ascii_lowercase().trim_ascii_whitespace());
        if (!MimeSniff::is_javascript_mime_type_essence_match(script_type)) {
            dbgln("SVGScriptElement: Unsupported script type: {}", *maybe_script_type);
            return;
        }
    }

    // 2. If the 'script' element references external script content, then the external script content
    //    using the current value of the 'xlink:href' attribute is fetched. Further processing of the
    //    'script' element is dependent on the external script content, and will block here until the
    //    resource has been fetched or is determined to be an invalid IRI reference.
    if (has_attribute(SVG::AttributeNames::href) || has_attribute_ns(Namespace::XLink.to_string(), SVG::AttributeNames::href)) {
        auto href_value = href()->base_val();

        auto maybe_script_url = document().encoding_parse_url(href_value);
        if (!maybe_script_url.has_value()) {
            dbgln("Invalid script URL: {}", href_value);
            return;
        }
        auto script_url = maybe_script_url.release_value();

        auto& vm = realm().vm();
        auto request = Fetch::Infrastructure::Request::create(vm);
        request->set_url(script_url);
        request->set_destination(Fetch::Infrastructure::Request::Destination::Script);
        // FIXME: Use CORS state specified by the ‘crossorigin’ attribute.
        request->set_mode(Fetch::Infrastructure::Request::Mode::NoCORS);
        request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::SameOrigin);
        request->set_client(&document().relevant_settings_object());

        // 3. The 'script' element's "already processed" flag is set to true.
        // We set this before dispatching the fetch so that re-entrant calls (e.g. from attribute_changed
        // while the fetch is in flight) early-return at step 1, matching Chromium's `already_started_`
        // semantics. The in-flight fetch is allowed to finish.
        m_already_processed = true;

        m_document_load_event_delayer.emplace(*m_document);

        if (m_parser_inserted)
            m_document->set_pending_parsing_blocking_svg_script(this);

        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
        fetch_algorithms_input.process_response_consume_body
            = [self = GC::Ref { *this }, script_url](auto response, auto body_bytes) {
                  ByteBuffer body;
                  if (!response->is_network_error())
                      body_bytes.visit([&](ByteBuffer& bytes) { body = move(bytes); }, [](auto) {});

                  self->finish_external_script_fetch(script_url, body);
              };

        (void)Fetch::Fetching::fetch(realm(), request,
            Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));
        return;
    }

    auto script_content = child_text_content().to_utf8_but_should_be_ported_to_utf16();
    if (script_content.is_empty())
        return;

    // 3. The 'script' element's "already processed" flag is set to true.
    m_already_processed = true;
    m_script = HTML::ClassicScript::create(m_document->url().basename(), script_content,
        HTML::relevant_settings_object(*this), m_document->base_url(), m_source_line_number);
    execute_script();
}

void SVGScriptElement::finish_external_script_fetch(URL::URL const& script_url, ByteBuffer const& body)
{
    if (!body.is_empty() && in_a_document_tree() && !is_scripting_disabled()) {
        auto script_content = String::from_utf8(body);
        if (script_content.is_error())
            dbgln("Failed to decode SVG external script as UTF-8");
        else
            m_script = HTML::ClassicScript::create(script_url.basename(), script_content.release_value(),
                HTML::relevant_settings_object(*this), m_document->base_url(), m_source_line_number);
    }

    if (m_parser_inserted) {
        m_ready_to_be_parser_executed = true;
        m_document->schedule_html_parser_end_check();
        return;
    }

    ScopeGuard clear_delayer { [&] { m_document_load_event_delayer.clear(); } };
    execute_script();
}

void SVGScriptElement::execute_pending_parser_blocking_script(Badge<HTML::HTMLParser>)
{
    VERIFY(m_ready_to_be_parser_executed);
    m_ready_to_be_parser_executed = false;
    ScopeGuard clear_delayer { [&] { m_document_load_event_delayer.clear(); } };
    execute_script();
}

void SVGScriptElement::execute_script()
{
    // 4. If the script content is inline, or if it is external and was fetched successfully, then the
    //    script is executed. Note that at this point, these steps may be re-entrant if the execution
    //    of the script results in further 'script' elements being inserted into the document.

    // m_script is null when the external fetch failed; the parser-blocking slot still needs to be drained
    // so the parser can resume, but there's no script to run.
    if (!m_script)
        return;

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-html
    // Before any script execution occurs, the user agent must wait for scripts may run for the newly-created document to be true for document.
    if (!m_document->ready_to_run_scripts())
        return;

    // FIXME: Note that a load event is dispatched on a 'script' element once it has been processed,
    // unless it referenced external script content with an invalid IRI reference and 'externalResourcesRequired' was set to 'true'.

    (void)m_script->run();
}

}
