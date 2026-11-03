/*
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Pavel Shliak <shlyakpavel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Utf16String.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGScriptElement);

SVGScriptElement::SVGScriptElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGScriptElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_preparation_time_document);
    visitor.visit(m_script);
}

void SVGScriptElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);

    if (m_document_load_event_delayer.has_value())
        m_document_load_event_delayer.emplace(document());
}

void SVGScriptElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
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
        auto script_type = maybe_script_type->to_ascii_lowercase().trim_ascii_whitespace();
        if (!MimeSniff::is_javascript_mime_type_essence_match(script_type)) {
            dbgln("SVGScriptElement: Unsupported script type: {}", *maybe_script_type);
            return;
        }
    }

    // https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element
    // 16. Set el's preparation-time document to its node document.
    m_preparation_time_document = document();

    // 2. If the 'script' element references external script content, then the external script content
    //    using the current value of the 'xlink:href' attribute is fetched. Further processing of the
    //    'script' element is dependent on the external script content, and will block here until the
    //    resource has been fetched or is determined to be an invalid IRI reference.
    if (has_attribute(SVG::AttributeNames::href) || has_attribute_ns(Namespace::XLink, SVG::AttributeNames::href)) {
        auto href_value = href()->base_val();

        auto maybe_script_url = document().encoding_parse_url(href_value);
        if (!maybe_script_url.has_value()) {
            dbgln("Invalid script URL: {}", href_value);
            return;
        }
        auto script_url = maybe_script_url.release_value();

        // https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element
        // 23. If el has a charset attribute, then let encoding be the result of getting an encoding from the value of
        //     the charset attribute. If el does not have a charset attribute, or if getting an encoding failed, then
        //     let encoding be el's node document's encoding.
        // NB: An SVG script element has no charset attribute.
        auto encoding = document().encoding_or_default();

        // 24. Let classic script CORS setting be the current state of el's crossorigin content attribute.
        auto crossorigin = get_attribute(HTML::AttributeNames::crossorigin);
        auto cors_setting = HTML::cors_setting_attribute_from_keyword(crossorigin.map([](auto const& value) { return value.utf16_view(); }));

        // 25-31. Let options be a script fetch options for el.
        // NB: An SVG script element has no integrity or fetchpriority attribute.
        auto referrer_policy_attribute = get_attribute(HTML::AttributeNames::referrerpolicy).value_or({});
        auto referrer_policy = ReferrerPolicy::from_string(referrer_policy_attribute).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);
        auto options = HTML::script_element_fetch_options({
            .crossorigin = cors_setting,
            .cryptographic_nonce = nonce(),
            .referrer_policy = referrer_policy,
            .parser_inserted = is_parser_inserted(),
        });

        // 32. Let settings object be el's node document's relevant settings object.
        auto& settings_object = document().relevant_settings_object();

        // 3. The 'script' element's "already processed" flag is set to true.
        // We set this before dispatching the fetch so that re-entrant calls (e.g. from attribute_changed
        // while the fetch is in flight) early-return at step 1, matching Chromium's `already_started_`
        // semantics. The in-flight fetch is allowed to finish.
        m_already_processed = true;

        // https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element
        // 33.8. Set el's delaying the load event to true.
        m_document_load_event_delayer.emplace(*m_preparation_time_document);

        // 33.11. Switch on el's type: "classic": Fetch a classic script given url, settings object, options, classic
        //        script CORS setting, encoding, and onComplete.
        auto on_complete = HTML::create_on_fetch_script_complete(heap(), [self = GC::Ref { *this }](GC::Ptr<HTML::Script> script) {
            self->finish_external_script_fetch(as<HTML::ClassicScript>(script.ptr()));
        });
        HTML::fetch_classic_script(HTML::relevant_realm(*this), script_url, settings_object, move(options), cors_setting, move(encoding), on_complete);

        // 35.5.1. Set el's parser document's pending parsing-blocking script to el.
        if (m_parser_inserted)
            m_document->set_pending_parsing_blocking_svg_script(this);
        return;
    }

    auto script_content = child_text_content();
    if (script_content.is_empty()) {
        // AD-HOC: The parser is done with an empty script once it gets here, so a later change to the script's
        //         children should process it like for a script that the parser did not insert.
        m_parser_inserted = false;
        return;
    }

    // 3. The 'script' element's "already processed" flag is set to true.
    m_already_processed = true;
    m_script = HTML::ClassicScript::create(m_document->url().basename(), script_content,
        HTML::relevant_settings_object(*this), m_document->base_url(), m_source_line_number, HTML::ClassicScript::MutedErrors::No, HTML::ScriptRegistry::IsInlineSource::Yes);
    execute_script();
}

void SVGScriptElement::finish_external_script_fetch(GC::Ptr<HTML::ClassicScript> script)
{
    if (in_a_document_tree() && !is_scripting_disabled())
        m_script = script;

    if (m_parser_inserted) {
        // https://html.spec.whatwg.org/multipage/scripting.html#mark-as-ready
        // 4. Set el's delaying the load event to false.
        m_document_load_event_delayer.clear();

        // NB: The parser that waits for a parser-inserted script is its preparation-time document's.
        m_ready_to_be_parser_executed = true;
        m_preparation_time_document->schedule_html_parser_end_check();
        return;
    }

    ScopeGuard clear_delayer { [&] { m_document_load_event_delayer.clear(); } };
    execute_script();
}

void SVGScriptElement::execute_pending_parser_blocking_script(Badge<HTML::HTMLParser>)
{
    VERIFY(m_ready_to_be_parser_executed);
    m_ready_to_be_parser_executed = false;
    execute_script();
}

void SVGScriptElement::execute_script()
{
    // 4. If the script content is inline, or if it is external and was fetched successfully, then the
    //    script is executed. Note that at this point, these steps may be re-entrant if the execution
    //    of the script results in further 'script' elements being inserted into the document.

    // https://html.spec.whatwg.org/multipage/scripting.html#execute-the-script-element
    // 1. Let document be el's node document.
    // 2. If el's preparation-time document is not equal to document, then return.
    if (m_preparation_time_document.ptr() != &document())
        return;

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
