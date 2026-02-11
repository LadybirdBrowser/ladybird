/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/CDATASection.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/Entities.h>
#include <LibWeb/HTML/Parser/NamedCharacterReferences.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

namespace Web {

Optional<String> resolve_named_html_entity(StringView entity_name)
{
    HTML::NamedCharacterReferenceMatcher matcher;
    for (auto c : entity_name) {
        if (!matcher.try_consume_ascii_char(c))
            return {};
    }
    if (!matcher.try_consume_ascii_char(';'))
        return {};

    auto codepoints = matcher.code_points();
    if (!codepoints.has_value())
        return {};

    StringBuilder builder;
    builder.append_code_point(codepoints.value().first);
    auto second_codepoint = HTML::named_character_reference_second_codepoint_value(codepoints.value().second);
    if (second_codepoint.has_value())
        builder.append_code_point(second_codepoint.value());

    return builder.to_string_without_validation();
}

XMLDocumentBuilder::XMLDocumentBuilder(DOM::Document& document, XMLScriptingSupport scripting_support)
    : m_document(document)
    , m_template_node_stack(document.realm().heap())
    , m_current_node(m_document)
    , m_scripting_support(scripting_support)
{
    m_namespace_stack.append({ {}, 1 });
}

ErrorOr<void> XMLDocumentBuilder::set_source(ByteString source)
{
    m_document->set_source(TRY(String::from_byte_string(source)));
    return {};
}

void XMLDocumentBuilder::set_doctype(XML::Doctype doctype)
{
    if (m_document->doctype()) {
        return;
    }

    auto document_type = DOM::DocumentType::create(m_document);
    auto name = MUST(AK::String::from_byte_string(doctype.type));
    document_type->set_name(name);

    if (doctype.external_id.has_value()) {
        auto external_id = doctype.external_id.release_value();

        auto system_id = MUST(AK::String::from_byte_string(external_id.system_id.system_literal));
        document_type->set_system_id(system_id);

        if (external_id.public_id.has_value()) {
            auto public_id = MUST(AK::String::from_byte_string(external_id.public_id.release_value().public_literal));
            document_type->set_public_id(public_id);
        }
    }

    m_document->insert_before(document_type, m_document->first_child(), false);
}

void XMLDocumentBuilder::element_start(XML::Name const& name, OrderedHashMap<XML::Name, ByteString> const& attributes)
{
    if (m_has_error)
        return;

    Vector<NamespaceAndPrefix, 2> namespaces;
    for (auto const& [name, value] : attributes) {
        if (name == "xmlns"sv || name.starts_with("xmlns:"sv)) {
            auto parts = name.split_limit(':', 2);
            Optional<ByteString> prefix;
            auto namespace_ = value;
            if (parts.size() == 2) {
                namespace_ = value;
                prefix = parts[1];
            }

            if (namespaces.find_if([&](auto const& namespace_and_prefix) { return namespace_and_prefix.prefix == prefix; }) != namespaces.end())
                continue;

            namespaces.append({ FlyString(MUST(String::from_byte_string(namespace_))), prefix });
        }
    }

    if (!namespaces.is_empty()) {
        m_namespace_stack.append({ move(namespaces), 1 });
    } else {
        m_namespace_stack.last().depth += 1;
    }

    auto namespace_ = namespace_for_name(name);

    auto qualified_name_or_error = DOM::validate_and_extract(m_document->realm(), namespace_, FlyString(MUST(String::from_byte_string(name))), DOM::ValidationContext::Element);

    if (qualified_name_or_error.is_error()) {
        m_has_error = true;
        return;
    }

    auto qualified_name = qualified_name_or_error.value();

    auto node_or_error = DOM::create_element(m_document, qualified_name.local_name(), qualified_name.namespace_(), qualified_name.prefix());

    if (node_or_error.is_error()) {
        m_has_error = true;
        return;
    }

    auto node = node_or_error.value();

    // When an XML parser with XML scripting support enabled creates a script element,
    // it must have its parser document set and its "force async" flag must be unset.
    // FIXME: If the parser was created as part of the XML fragment parsing algorithm, then the element must be marked as "already started" also.
    if (m_scripting_support == XMLScriptingSupport::Enabled && node->is_html_script_element()) {
        auto& script_element = static_cast<HTML::HTMLScriptElement&>(*node);
        script_element.set_parser_document(Badge<XMLDocumentBuilder> {}, m_document);
        script_element.set_force_async(Badge<XMLDocumentBuilder> {}, false);
    }
    if (m_current_node->is_html_template_element()) {
        // When an XML parser would append a node to a template element, it must instead append it to the template element's template contents (a DocumentFragment node).
        m_template_node_stack.append(*m_current_node);
        MUST(static_cast<HTML::HTMLTemplateElement&>(*m_current_node).content()->append_child(node));
    } else {
        MUST(m_current_node->append_child(node));
    }

    for (auto const& attribute : attributes) {
        if (attribute.key == "xmlns" || attribute.key.starts_with("xmlns:"sv)) {
            // The prefix xmlns is used only to declare namespace bindings and is by definition bound to the namespace name http://www.w3.org/2000/xmlns/.
            if (!attribute.key.is_one_of("xmlns:"sv, "xmlns:xmlns"sv)) {
                auto maybe_extracted_qualified_name = validate_and_extract(node->realm(), Namespace::XMLNS, MUST(String::from_byte_string(attribute.key)), DOM::ValidationContext::Element);
                if (!maybe_extracted_qualified_name.is_error()) {
                    auto extracted_qualified_name = maybe_extracted_qualified_name.release_value();
                    node->set_attribute_value(extracted_qualified_name.local_name(), MUST(String::from_byte_string(attribute.value)), extracted_qualified_name.prefix(), extracted_qualified_name.namespace_());
                    continue;
                }
            }

            m_has_error = true;
        } else if (attribute.key.contains(':')) {
            if (auto namespace_for_key = namespace_for_name(attribute.key); namespace_for_key.has_value()) {
                auto maybe_extracted_qualified_name = validate_and_extract(node->realm(), namespace_for_key, MUST(String::from_byte_string(attribute.key)), DOM::ValidationContext::Element);
                if (!maybe_extracted_qualified_name.is_error()) {
                    auto extracted_qualified_name = maybe_extracted_qualified_name.release_value();
                    node->set_attribute_value(extracted_qualified_name.local_name(), MUST(String::from_byte_string(attribute.value)), extracted_qualified_name.prefix(), extracted_qualified_name.namespace_());
                    continue;
                }
            }

            if (attribute.key.starts_with("xml:"sv)) {
                auto maybe_extracted_qualified_name = validate_and_extract(node->realm(), Namespace::XML, MUST(String::from_byte_string(attribute.key)), DOM::ValidationContext::Element);
                if (!maybe_extracted_qualified_name.is_error()) {
                    auto extracted_qualified_name = maybe_extracted_qualified_name.release_value();
                    node->set_attribute_value(extracted_qualified_name.local_name(), MUST(String::from_byte_string(attribute.value)), extracted_qualified_name.prefix(), extracted_qualified_name.namespace_());
                    continue;
                }
            }

            m_has_error = true;
        } else {
            node->set_attribute_value(MUST(String::from_byte_string(attribute.key)), MUST(String::from_byte_string(attribute.value)));
        }
    }

    m_current_node = node.ptr();
}

void XMLDocumentBuilder::element_end(XML::Name const& name)
{
    if (m_has_error)
        return;

    if (--m_namespace_stack.last().depth == 0) {
        m_namespace_stack.take_last();
    }

    VERIFY(m_current_node->node_name().equals_ignoring_ascii_case(name));
    // When an XML parser with XML scripting support enabled creates a script element, [...]
    // When the element's end tag is subsequently parsed,
    if (m_scripting_support == XMLScriptingSupport::Enabled && m_current_node->is_html_script_element()) {
        // the user agent must perform a microtask checkpoint,
        HTML::perform_a_microtask_checkpoint();
        // and then prepare the script element.
        auto& script_element = static_cast<HTML::HTMLScriptElement&>(*m_current_node);
        script_element.prepare_script(Badge<XMLDocumentBuilder> {});

        // If this causes there to be a pending parsing-blocking script, then the user agent must run the following steps:
        if (auto pending_parsing_blocking_script = m_document->pending_parsing_blocking_script()) {
            // 1. Block this instance of the XML parser, such that the event loop will not run tasks that invoke it.
            // NOTE: Noop.

            // 2. Spin the event loop until the parser's Document has no style sheet that is blocking scripts and the pending parsing-blocking script's "ready to be parser-executed" flag is set.
            if (m_document->has_a_style_sheet_that_is_blocking_scripts() || !pending_parsing_blocking_script->is_ready_to_be_parser_executed()) {
                HTML::main_thread_event_loop().spin_until(GC::create_function(script_element.heap(), [&] {
                    return !m_document->has_a_style_sheet_that_is_blocking_scripts() && pending_parsing_blocking_script->is_ready_to_be_parser_executed();
                }));
            }

            // 3. Unblock this instance of the XML parser, such that tasks that invoke it can again be run.
            // NOTE: Noop.

            // 4. Execute the script element given by the pending parsing-blocking script.
            pending_parsing_blocking_script->execute_script();

            // 5. Set the pending parsing-blocking script to null.
            m_document->set_pending_parsing_blocking_script(nullptr);
        }
    } else if (m_scripting_support == XMLScriptingSupport::Enabled && m_current_node->is_svg_script_element()) {
        // https://www.w3.org/TR/SVGMobile12/struct.html#ProgressiveRendering
        // When an end element event occurs for a 'script' element, that element is processed according to the
        // Script processing section of the Scripting chapter. Further parsing of the document will be blocked
        // until processing of the 'script' is complete.
        auto& script_element = static_cast<SVG::SVGScriptElement&>(*m_current_node);
        script_element.process_the_script_element();
    };

    auto* parent = m_current_node->parent_node();
    if (parent && parent->is_document_fragment()) {
        auto template_parent_node = m_template_node_stack.take_last();
        parent = template_parent_node.ptr();
    }
    m_current_node = parent;
}

void XMLDocumentBuilder::text(StringView data)
{
    if (m_has_error)
        return;

    if (auto* last = m_current_node->last_child(); last && last->is_text()) {
        auto& text_node = static_cast<DOM::Text&>(*last);
        m_text_builder.append(text_node.data());
        m_text_builder.append(data);
        text_node.set_data(m_text_builder.to_utf16_string());
        m_text_builder.clear();
    } else if (!data.is_empty()) {
        auto node = m_document->create_text_node(Utf16String::from_utf8(data));
        MUST(m_current_node->append_child(node));
    }
}

void XMLDocumentBuilder::comment(StringView data)
{
    if (m_has_error || !m_current_node)
        return;

    MUST(m_current_node->append_child(m_document->create_comment(Utf16String::from_utf8(data))));
}

void XMLDocumentBuilder::cdata_section(StringView data)
{
    if (m_has_error || !m_current_node)
        return;

    auto section = MUST(m_document->create_cdata_section(Utf16String::from_utf8(data)));
    MUST(m_current_node->append_child(section));
}

void XMLDocumentBuilder::processing_instruction(StringView target, StringView data)
{
    if (m_has_error || !m_current_node)
        return;

    auto processing_instruction = MUST(m_document->create_processing_instruction(MUST(String::from_utf8(target)), Utf16String::from_utf8(data)));
    MUST(m_current_node->append_child(processing_instruction));
}

void XMLDocumentBuilder::document_end()
{
    auto& heap = m_document->heap();

    // When an XML parser reaches the end of its input, it must stop parsing.
    // If the active speculative HTML parser is not null, then stop the speculative HTML parser and return.
    // NOTE: Noop.

    // Set the insertion point to undefined.
    m_template_node_stack.clear();
    m_current_node = nullptr;

    // Update the current document readiness to "interactive".
    m_document->update_readiness(HTML::DocumentReadyState::Interactive);

    // Pop all the nodes off the stack of open elements.
    // NOTE: Noop.

    if (!m_document->browsing_context() || m_document->is_decoded_svg()) {
        // No need to spin the event loop waiting for scripts or load events
        // when parsed via DOMParser or as a decoded SVG image.
        m_document->update_readiness(HTML::DocumentReadyState::Complete);
        return;
    }

    // While the list of scripts that will execute when the document has finished parsing is not empty:
    while (!m_document->scripts_to_execute_when_parsing_has_finished().is_empty()) {
        // Spin the event loop until the first script in the list of scripts that will execute when the document has finished parsing has its "ready to be parser-executed" flag set
        // and the parser's Document has no style sheet that is blocking scripts.
        HTML::main_thread_event_loop().spin_until(GC::create_function(heap, [&] {
            return m_document->scripts_to_execute_when_parsing_has_finished().first()->is_ready_to_be_parser_executed()
                && !m_document->has_a_style_sheet_that_is_blocking_scripts();
        }));

        // Execute the first script in the list of scripts that will execute when the document has finished parsing.
        m_document->scripts_to_execute_when_parsing_has_finished().first()->execute_script();

        // Remove the first script element from the list of scripts that will execute when the document has finished parsing (i.e. shift out the first entry in the list).
        (void)m_document->scripts_to_execute_when_parsing_has_finished().take_first();
    }
    // Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following substeps:
    queue_global_task(HTML::Task::Source::DOMManipulation, m_document, GC::create_function(m_document->heap(), [document = m_document] {
        // Set the Document's load timing info's DOM content loaded event start time to the current high resolution time given the Document's relevant global object.
        document->load_timing_info().dom_content_loaded_event_start_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(*document));

        // Fire an event named DOMContentLoaded at the Document object, with its bubbles attribute initialized to true.
        auto content_loaded_event = DOM::Event::create(document->realm(), HTML::EventNames::DOMContentLoaded);
        content_loaded_event->set_bubbles(true);
        document->dispatch_event(content_loaded_event);

        // Set the Document's load timing info's DOM content loaded event end time to the current high resolution time given the Document's relevant global object.
        document->load_timing_info().dom_content_loaded_event_end_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(*document));

        // FIXME: Enable the client message queue of the ServiceWorkerContainer object whose associated service worker client is the Document object's relevant settings object.

        // FIXME: Invoke WebDriver BiDi DOM content loaded with the Document's browsing context, and a new WebDriver BiDi navigation status whose id is the Document object's navigation id, status is "pending", and url is the Document object's URL.
    }));

    // Spin the event loop until the set of scripts that will execute as soon as possible and the list of scripts that will execute in order as soon as possible are empty.
    HTML::main_thread_event_loop().spin_until(GC::create_function(heap, [&] {
        return m_document->scripts_to_execute_as_soon_as_possible().is_empty();
    }));

    // Spin the event loop until there is nothing that delays the load event in the Document.
    HTML::main_thread_event_loop().spin_until(GC::create_function(heap, [&] {
        return !m_document->anything_is_delaying_the_load_event();
    }));

    // Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following steps:
    queue_global_task(HTML::Task::Source::DOMManipulation, m_document, GC::create_function(m_document->heap(), [document = m_document] {
        // Update the current document readiness to "complete".
        document->update_readiness(HTML::DocumentReadyState::Complete);

        // If the Document object's browsing context is null, then abort these steps.
        if (!document->browsing_context())
            return;

        // Let window be the Document's relevant global object.
        GC::Ref<HTML::Window> window = as<HTML::Window>(relevant_global_object(*document));

        // Set the Document's load timing info's load event start time to the current high resolution time given window.
        document->load_timing_info().load_event_start_time = HighResolutionTime::current_high_resolution_time(window);

        // Fire an event named load at window, with legacy target override flag set.
        // FIXME: The legacy target override flag is currently set by a virtual override of dispatch_event()
        // We should reorganize this so that the flag appears explicitly here instead.
        window->dispatch_event(DOM::Event::create(document->realm(), HTML::EventNames::load));

        // FIXME: Invoke WebDriver BiDi load complete with the Document's browsing context, and a new WebDriver BiDi navigation status whose id is the Document object's navigation id, status is "complete", and url is the Document object's URL.

        // FIXME: Set the Document object's navigation id to null.

        // Set the Document's load timing info's load event end time to the current high resolution time given window.
        document->load_timing_info().dom_content_loaded_event_end_time = HighResolutionTime::current_high_resolution_time(window);

        // Assert: Document's page showing is false.
        VERIFY(!document->page_showing());

        // Set the Document's page showing flag to true.
        document->set_page_showing(true);

        // Fire a page transition event named pageshow at window with false.
        window->fire_a_page_transition_event(HTML::EventNames::pageshow, false);

        // Completely finish loading the Document.
        document->completely_finish_loading();

        // FIXME: Queue the navigation timing entry for the Document.
    }));

    // FIXME: If the Document's print when loaded flag is set, then run the printing steps.

    // The Document is now ready for post-load tasks.
    m_document->set_ready_for_post_load_tasks(true);
}

Optional<FlyString> XMLDocumentBuilder::namespace_for_name(XML::Name const& name)
{
    Optional<StringView> prefix;

    auto parts = name.split_limit(':', 3);
    if (parts.size() > 2)
        return {};

    if (parts.size() == 2) {
        if (parts[0].is_empty() || parts[1].is_empty())
            return {};
        prefix = parts[0];
    }

    for (auto const& stack_entry : m_namespace_stack.in_reverse()) {
        for (auto const& namespace_and_prefix : stack_entry.namespaces) {
            if (namespace_and_prefix.prefix == prefix) {
                return namespace_and_prefix.ns;
            }
        }
    }

    return {};
}

}
