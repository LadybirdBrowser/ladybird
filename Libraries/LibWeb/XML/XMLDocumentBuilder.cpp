/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

inline namespace {
extern StringView s_xhtml_unified_dtd;
}

namespace Web {

ErrorOr<Variant<ByteString, Vector<XML::MarkupDeclaration>>> resolve_xml_resource(XML::SystemID const&, Optional<XML::PublicID> const& public_id)
{
    static Optional<Vector<XML::MarkupDeclaration>> s_parsed_xhtml_unified_dtd;
    if (!public_id.has_value())
        return Error::from_string_literal("Refusing to load disallowed external entity");

    auto public_literal = public_id->public_literal;
    if (!public_literal.is_one_of(
            "-//W3C//DTD XHTML 1.0 Transitional//EN",
            "-//W3C//DTD XHTML 1.1//EN",
            "-//W3C//DTD XHTML 1.0 Strict//EN",
            "-//W3C//DTD XHTML 1.0 Frameset//EN",
            "-//W3C//DTD XHTML Basic 1.0//EN",
            "-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN",
            "-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN",
            "-//W3C//DTD MathML 2.0//EN",
            "-//WAPFORUM//DTD XHTML Mobile 1.0//EN"))
        return Error::from_string_literal("Refusing to load disallowed external entity");

    if (!s_parsed_xhtml_unified_dtd.has_value()) {
        auto parser = XML::Parser(s_xhtml_unified_dtd, XML::Parser::Options {});
        auto result = parser.parse_external_subset();
        if (result.is_error()) // We can't really recover from this, so just return the source and let libxml handle it.
            return ByteString { s_xhtml_unified_dtd };
        s_parsed_xhtml_unified_dtd = result.release_value();
    }

    return s_parsed_xhtml_unified_dtd.value();
}

XMLDocumentBuilder::XMLDocumentBuilder(DOM::Document& document, XMLScriptingSupport scripting_support)
    : m_document(document)
    , m_current_node(m_document)
    , m_scripting_support(scripting_support)
{
    m_namespace_stack.append({ {}, 1 });
}

void XMLDocumentBuilder::set_source(ByteString source)
{
    m_document->set_source(MUST(String::from_byte_string(source)));
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

void XMLDocumentBuilder::element_start(const XML::Name& name, HashMap<XML::Name, ByteString> const& attributes)
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

    auto namespace_ = namespace_for_element(name);

    auto qualified_name_or_error = DOM::validate_and_extract(m_document->realm(), namespace_, FlyString(MUST(String::from_byte_string(name))));

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
        MUST(static_cast<HTML::HTMLTemplateElement&>(*m_current_node).content()->append_child(node));
    } else {
        MUST(m_current_node->append_child(node));
    }

    for (auto const& attribute : attributes) {
        // https://www.w3.org/TR/2006/REC-xml-names11-20060816/#ns-decl
        if (attribute.key == "xmlns" || attribute.key.starts_with("xmlns:"sv)) {
            auto name = attribute.key;
            if (!name.is_one_of("xmlns:"sv, "xmlns:xmlns"sv)) {
                // The prefix xmlns is used only to declare namespace bindings and is by definition bound to the namespace name http://www.w3.org/2000/xmlns/.
                MUST(node->set_attribute_ns(Namespace::XMLNS, MUST(String::from_byte_string(name)), MUST(String::from_byte_string(attribute.value))));
            } else {
                m_has_error = true;
            }
        } else if (attribute.key.contains(":"sv)) {
            if (!attribute.key.starts_with("xml:"sv)) {
                m_has_error = true;
            }
        }
        MUST(node->set_attribute(MUST(String::from_byte_string(attribute.key)), MUST(String::from_byte_string(attribute.value))));
    }

    m_current_node = node.ptr();
}

void XMLDocumentBuilder::element_end(const XML::Name& name)
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

    m_current_node = m_current_node->parent_node();
}

void XMLDocumentBuilder::text(StringView data)
{
    if (m_has_error)
        return;
    auto last = m_current_node->last_child();
    if (last && last->is_text()) {
        auto& text_node = static_cast<DOM::Text&>(*last);
        text_builder.append(text_node.data());
        text_builder.append(data);
        text_node.set_data(MUST(text_builder.to_string()));
        text_builder.clear();
    } else {
        if (!data.is_empty()) {
            auto node = m_document->create_text_node(MUST(String::from_utf8(data)));
            MUST(m_current_node->append_child(node));
        }
    }
}

void XMLDocumentBuilder::comment(StringView data)
{
    if (m_has_error)
        return;
    MUST(m_document->append_child(m_document->create_comment(MUST(String::from_utf8(data)))));
}

void XMLDocumentBuilder::document_end()
{
    auto& heap = m_document->heap();

    // When an XML parser reaches the end of its input, it must stop parsing.
    // If the active speculative HTML parser is not null, then stop the speculative HTML parser and return.
    // NOTE: Noop.

    // Set the insertion point to undefined.
    m_current_node = nullptr;

    // Update the current document readiness to "interactive".
    m_document->update_readiness(HTML::DocumentReadyState::Interactive);

    // Pop all the nodes off the stack of open elements.
    // NOTE: Noop.

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

Optional<FlyString> XMLDocumentBuilder::namespace_for_element(XML::Name const& element_name)
{
    Optional<ByteString> prefix;
    if (auto parts = element_name.split_limit(':', 2); parts.size() == 2) {
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

inline namespace {
StringView s_xhtml_unified_dtd = R"xmlxmlxml(
<!ENTITY Tab "&#x9;"><!ENTITY NewLine "&#xA;"><!ENTITY excl "&#x21;"><!ENTITY quot "&#x22;"><!ENTITY QUOT "&#x22;"><!ENTITY num "&#x23;"><!ENTITY dollar "&#x24;"><!ENTITY percnt "&#x25;"><!ENTITY amp "&#x26;#x26;"><!ENTITY AMP "&#x26;#x26;"><!ENTITY apos "&#x27;"><!ENTITY lpar "&#x28;"><!ENTITY rpar "&#x29;"><!ENTITY ast "&#x2A;"><!ENTITY midast "&#x2A;"><!ENTITY plus "&#x2B;"><!ENTITY comma "&#x2C;"><!ENTITY period "&#x2E;"><!ENTITY sol "&#x2F;"><!ENTITY colon "&#x3A;"><!ENTITY semi "&#x3B;"><!ENTITY lt "&#x26;#x3C;"><!ENTITY LT "&#x26;#x3C;"><!ENTITY nvlt "&#x26;#x3C;&#x20D2;"><!ENTITY equals "&#x3D;"><!ENTITY bne "&#x3D;&#x20E5;"><!ENTITY gt "&#x3E;"><!ENTITY GT "&#x3E;"><!ENTITY nvgt "&#x3E;&#x20D2;"><!ENTITY quest "&#x3F;"><!ENTITY commat "&#x40;"><!ENTITY lsqb "&#x5B;"><!ENTITY lbrack "&#x5B;"><!ENTITY bsol "&#x5C;"><!ENTITY rsqb "&#x5D;"><!ENTITY rbrack "&#x5D;"><!ENTITY Hat "&#x5E;"><!ENTITY lowbar "&#x5F;"><!ENTITY UnderBar "&#x5F;"><!ENTITY grave "&#x60;"><!ENTITY DiacriticalGrave "&#x60;"><!ENTITY fjlig "&#x66;&#x6A;"><!ENTITY lcub "&#x7B;"><!ENTITY lbrace "&#x7B;"><!ENTITY verbar "&#x7C;"><!ENTITY vert "&#x7C;"><!ENTITY VerticalLine "&#x7C;"><!ENTITY rcub "&#x7D;"><!ENTITY rbrace "&#x7D;"><!ENTITY nbsp "&#xA0;"><!ENTITY NonBreakingSpace "&#xA0;"><!ENTITY iexcl "&#xA1;"><!ENTITY cent "&#xA2;"><!ENTITY pound "&#xA3;"><!ENTITY curren "&#xA4;"><!ENTITY yen "&#xA5;"><!ENTITY brvbar "&#xA6;"><!ENTITY sect "&#xA7;"><!ENTITY Dot "&#xA8;"><!ENTITY die "&#xA8;"><!ENTITY DoubleDot "&#xA8;"><!ENTITY uml "&#xA8;"><!ENTITY copy "&#xA9;"><!ENTITY COPY "&#xA9;"><!ENTITY ordf "&#xAA;"><!ENTITY laquo "&#xAB;"><!ENTITY not "&#xAC;"><!ENTITY shy "&#xAD;"><!ENTITY reg "&#xAE;"><!ENTITY circledR "&#xAE;"><!ENTITY REG "&#xAE;"><!ENTITY macr "&#xAF;"><!ENTITY strns "&#xAF;"><!ENTITY deg "&#xB0;"><!ENTITY plusmn "&#xB1;"><!ENTITY pm "&#xB1;"><!ENTITY PlusMinus "&#xB1;"><!ENTITY sup2 "&#xB2;"><!ENTITY sup3 "&#xB3;"><!ENTITY acute "&#xB4;"><!ENTITY DiacriticalAcute "&#xB4;"><!ENTITY micro "&#xB5;"><!ENTITY para "&#xB6;"><!ENTITY middot "&#xB7;"><!ENTITY centerdot "&#xB7;"><!ENTITY CenterDot "&#xB7;"><!ENTITY cedil "&#xB8;"><!ENTITY Cedilla "&#xB8;"><!ENTITY sup1 "&#xB9;"><!ENTITY ordm "&#xBA;"><!ENTITY raquo "&#xBB;"><!ENTITY frac14 "&#xBC;"><!ENTITY frac12 "&#xBD;"><!ENTITY half "&#xBD;"><!ENTITY frac34 "&#xBE;"><!ENTITY iquest "&#xBF;"><!ENTITY Agrave "&#xC0;"><!ENTITY Aacute "&#xC1;"><!ENTITY Acirc "&#xC2;"><!ENTITY Atilde "&#xC3;"><!ENTITY Auml "&#xC4;"><!ENTITY Aring "&#xC5;"><!ENTITY angst "&#xC5;"><!ENTITY AElig "&#xC6;"><!ENTITY Ccedil "&#xC7;"><!ENTITY Egrave "&#xC8;"><!ENTITY Eacute "&#xC9;"><!ENTITY Ecirc "&#xCA;"><!ENTITY Euml "&#xCB;"><!ENTITY Igrave "&#xCC;"><!ENTITY Iacute "&#xCD;"><!ENTITY Icirc "&#xCE;"><!ENTITY Iuml "&#xCF;"><!ENTITY ETH "&#xD0;"><!ENTITY Ntilde "&#xD1;"><!ENTITY Ograve "&#xD2;"><!ENTITY Oacute "&#xD3;"><!ENTITY Ocirc "&#xD4;"><!ENTITY Otilde "&#xD5;"><!ENTITY Ouml "&#xD6;"><!ENTITY times "&#xD7;"><!ENTITY Oslash "&#xD8;"><!ENTITY Ugrave "&#xD9;"><!ENTITY Uacute "&#xDA;"><!ENTITY Ucirc "&#xDB;"><!ENTITY Uuml "&#xDC;"><!ENTITY Yacute "&#xDD;"><!ENTITY THORN "&#xDE;"><!ENTITY szlig "&#xDF;"><!ENTITY agrave "&#xE0;"><!ENTITY aacute "&#xE1;"><!ENTITY acirc "&#xE2;"><!ENTITY atilde "&#xE3;"><!ENTITY auml "&#xE4;"><!ENTITY aring "&#xE5;"><!ENTITY aelig "&#xE6;"><!ENTITY ccedil "&#xE7;"><!ENTITY egrave "&#xE8;"><!ENTITY eacute "&#xE9;"><!ENTITY ecirc "&#xEA;"><!ENTITY euml "&#xEB;"><!ENTITY igrave "&#xEC;"><!ENTITY iacute "&#xED;"><!ENTITY icirc "&#xEE;"><!ENTITY iuml "&#xEF;"><!ENTITY eth "&#xF0;"><!ENTITY ntilde "&#xF1;"><!ENTITY ograve "&#xF2;"><!ENTITY oacute "&#xF3;"><!ENTITY ocirc "&#xF4;"><!ENTITY otilde "&#xF5;"><!ENTITY ouml "&#xF6;"><!ENTITY divide "&#xF7;"><!ENTITY div "&#xF7;"><!ENTITY oslash "&#xF8;"><!ENTITY ugrave "&#xF9;"><!ENTITY uacute "&#xFA;"><!ENTITY ucirc "&#xFB;"><!ENTITY uuml "&#xFC;"><!ENTITY yacute "&#xFD;"><!ENTITY thorn "&#xFE;"><!ENTITY yuml "&#xFF;"><!ENTITY Amacr "&#x100;"><!ENTITY amacr "&#x101;"><!ENTITY Abreve "&#x102;"><!ENTITY abreve "&#x103;"><!ENTITY Aogon "&#x104;"><!ENTITY aogon "&#x105;"><!ENTITY Cacute "&#x106;"><!ENTITY cacute "&#x107;"><!ENTITY Ccirc "&#x108;"><!ENTITY ccirc "&#x109;"><!ENTITY Cdot "&#x10A;"><!ENTITY cdot "&#x10B;"><!ENTITY Ccaron "&#x10C;"><!ENTITY ccaron "&#x10D;"><!ENTITY Dcaron "&#x10E;"><!ENTITY dcaron "&#x10F;"><!ENTITY Dstrok "&#x110;"><!ENTITY dstrok "&#x111;"><!ENTITY Emacr "&#x112;"><!ENTITY emacr "&#x113;"><!ENTITY Edot "&#x116;"><!ENTITY edot "&#x117;"><!ENTITY Eogon "&#x118;"><!ENTITY eogon "&#x119;"><!ENTITY Ecaron "&#x11A;"><!ENTITY ecaron "&#x11B;"><!ENTITY Gcirc "&#x11C;"><!ENTITY gcirc "&#x11D;"><!ENTITY Gbreve "&#x11E;"><!ENTITY gbreve "&#x11F;"><!ENTITY Gdot "&#x120;"><!ENTITY gdot "&#x121;"><!ENTITY Gcedil "&#x122;"><!ENTITY Hcirc "&#x124;"><!ENTITY hcirc "&#x125;"><!ENTITY Hstrok "&#x126;"><!ENTITY hstrok "&#x127;"><!ENTITY Itilde "&#x128;"><!ENTITY itilde "&#x129;"><!ENTITY Imacr "&#x12A;"><!ENTITY imacr "&#x12B;"><!ENTITY Iogon "&#x12E;"><!ENTITY iogon "&#x12F;"><!ENTITY Idot "&#x130;"><!ENTITY imath "&#x131;"><!ENTITY inodot "&#x131;"><!ENTITY IJlig "&#x132;"><!ENTITY ijlig "&#x133;"><!ENTITY Jcirc "&#x134;"><!ENTITY jcirc "&#x135;"><!ENTITY Kcedil "&#x136;"><!ENTITY kcedil "&#x137;"><!ENTITY kgreen "&#x138;"><!ENTITY Lacute "&#x139;"><!ENTITY lacute "&#x13A;"><!ENTITY Lcedil "&#x13B;"><!ENTITY lcedil "&#x13C;"><!ENTITY Lcaron "&#x13D;"><!ENTITY lcaron "&#x13E;"><!ENTITY Lmidot "&#x13F;"><!ENTITY lmidot "&#x140;"><!ENTITY Lstrok "&#x141;"><!ENTITY lstrok "&#x142;"><!ENTITY Nacute "&#x143;"><!ENTITY nacute "&#x144;"><!ENTITY Ncedil "&#x145;"><!ENTITY ncedil "&#x146;"><!ENTITY Ncaron "&#x147;"><!ENTITY ncaron "&#x148;"><!ENTITY napos "&#x149;"><!ENTITY ENG "&#x14A;"><!ENTITY eng "&#x14B;"><!ENTITY Omacr "&#x14C;"><!ENTITY omacr "&#x14D;"><!ENTITY Odblac "&#x150;"><!ENTITY odblac "&#x151;"><!ENTITY OElig "&#x152;"><!ENTITY oelig "&#x153;"><!ENTITY Racute "&#x154;"><!ENTITY racute "&#x155;"><!ENTITY Rcedil "&#x156;"><!ENTITY rcedil "&#x157;"><!ENTITY Rcaron "&#x158;"><!ENTITY rcaron "&#x159;"><!ENTITY Sacute "&#x15A;"><!ENTITY sacute "&#x15B;"><!ENTITY Scirc "&#x15C;"><!ENTITY scirc "&#x15D;"><!ENTITY Scedil "&#x15E;"><!ENTITY scedil "&#x15F;"><!ENTITY Scaron "&#x160;"><!ENTITY scaron "&#x161;"><!ENTITY Tcedil "&#x162;"><!ENTITY tcedil "&#x163;"><!ENTITY Tcaron "&#x164;"><!ENTITY tcaron "&#x165;"><!ENTITY Tstrok "&#x166;"><!ENTITY tstrok "&#x167;"><!ENTITY Utilde "&#x168;"><!ENTITY utilde "&#x169;"><!ENTITY Umacr "&#x16A;"><!ENTITY umacr "&#x16B;"><!ENTITY Ubreve "&#x16C;"><!ENTITY ubreve "&#x16D;"><!ENTITY Uring "&#x16E;"><!ENTITY uring "&#x16F;"><!ENTITY Udblac "&#x170;"><!ENTITY udblac "&#x171;"><!ENTITY Uogon "&#x172;"><!ENTITY uogon "&#x173;"><!ENTITY Wcirc "&#x174;"><!ENTITY wcirc "&#x175;"><!ENTITY Ycirc "&#x176;"><!ENTITY ycirc "&#x177;"><!ENTITY Yuml "&#x178;"><!ENTITY Zacute "&#x179;"><!ENTITY zacute "&#x17A;"><!ENTITY Zdot "&#x17B;"><!ENTITY zdot "&#x17C;"><!ENTITY Zcaron "&#x17D;"><!ENTITY zcaron "&#x17E;"><!ENTITY fnof "&#x192;"><!ENTITY imped "&#x1B5;"><!ENTITY gacute "&#x1F5;"><!ENTITY jmath "&#x237;"><!ENTITY circ "&#x2C6;"><!ENTITY caron "&#x2C7;"><!ENTITY Hacek "&#x2C7;"><!ENTITY breve "&#x2D8;"><!ENTITY Breve "&#x2D8;"><!ENTITY dot "&#x2D9;"><!ENTITY DiacriticalDot "&#x2D9;"><!ENTITY ring "&#x2DA;"><!ENTITY ogon "&#x2DB;"><!ENTITY tilde "&#x2DC;"><!ENTITY DiacriticalTilde "&#x2DC;"><!ENTITY dblac "&#x2DD;"><!ENTITY DiacriticalDoubleAcute "&#x2DD;"><!ENTITY DownBreve "&#x311;"><!ENTITY Alpha "&#x391;"><!ENTITY Beta "&#x392;"><!ENTITY Gamma "&#x393;"><!ENTITY Delta "&#x394;"><!ENTITY Epsilon "&#x395;"><!ENTITY Zeta "&#x396;"><!ENTITY Eta "&#x397;"><!ENTITY Theta "&#x398;"><!ENTITY Iota "&#x399;"><!ENTITY Kappa "&#x39A;"><!ENTITY Lambda "&#x39B;"><!ENTITY Mu "&#x39C;"><!ENTITY Nu "&#x39D;"><!ENTITY Xi "&#x39E;"><!ENTITY Omicron "&#x39F;"><!ENTITY Pi "&#x3A0;"><!ENTITY Rho "&#x3A1;"><!ENTITY Sigma "&#x3A3;"><!ENTITY Tau "&#x3A4;"><!ENTITY Upsilon "&#x3A5;"><!ENTITY Phi "&#x3A6;"><!ENTITY Chi "&#x3A7;"><!ENTITY Psi "&#x3A8;"><!ENTITY Omega "&#x3A9;"><!ENTITY ohm "&#x3A9;"><!ENTITY alpha "&#x3B1;"><!ENTITY beta "&#x3B2;"><!ENTITY gamma "&#x3B3;"><!ENTITY delta "&#x3B4;"><!ENTITY epsi "&#x3B5;"><!ENTITY epsilon "&#x3B5;"><!ENTITY zeta "&#x3B6;"><!ENTITY eta "&#x3B7;"><!ENTITY theta "&#x3B8;"><!ENTITY iota "&#x3B9;"><!ENTITY kappa "&#x3BA;"><!ENTITY lambda "&#x3BB;"><!ENTITY mu "&#x3BC;"><!ENTITY nu "&#x3BD;"><!ENTITY xi "&#x3BE;"><!ENTITY omicron "&#x3BF;"><!ENTITY pi "&#x3C0;"><!ENTITY rho "&#x3C1;"><!ENTITY sigmav "&#x3C2;"><!ENTITY varsigma "&#x3C2;"><!ENTITY sigmaf "&#x3C2;"><!ENTITY sigma "&#x3C3;"><!ENTITY tau "&#x3C4;"><!ENTITY upsi "&#x3C5;"><!ENTITY upsilon "&#x3C5;"><!ENTITY phi "&#x3C6;"><!ENTITY chi "&#x3C7;"><!ENTITY psi "&#x3C8;"><!ENTITY omega "&#x3C9;"><!ENTITY thetav "&#x3D1;"><!ENTITY vartheta "&#x3D1;"><!ENTITY thetasym "&#x3D1;"><!ENTITY Upsi "&#x3D2;"><!ENTITY upsih "&#x3D2;"><!ENTITY straightphi "&#x3D5;"><!ENTITY phiv "&#x3D5;"><!ENTITY varphi "&#x3D5;"><!ENTITY piv "&#x3D6;"><!ENTITY varpi "&#x3D6;"><!ENTITY Gammad "&#x3DC;"><!ENTITY gammad "&#x3DD;"><!ENTITY digamma "&#x3DD;"><!ENTITY kappav "&#x3F0;"><!ENTITY varkappa "&#x3F0;"><!ENTITY rhov "&#x3F1;"><!ENTITY varrho "&#x3F1;"><!ENTITY epsiv "&#x3F5;"><!ENTITY straightepsilon "&#x3F5;"><!ENTITY varepsilon "&#x3F5;"><!ENTITY bepsi "&#x3F6;"><!ENTITY backepsilon "&#x3F6;"><!ENTITY IOcy "&#x401;"><!ENTITY DJcy "&#x402;"><!ENTITY GJcy "&#x403;"><!ENTITY Jukcy "&#x404;"><!ENTITY DScy "&#x405;"><!ENTITY Iukcy "&#x406;"><!ENTITY YIcy "&#x407;"><!ENTITY Jsercy "&#x408;"><!ENTITY LJcy "&#x409;"><!ENTITY NJcy "&#x40A;"><!ENTITY TSHcy "&#x40B;"><!ENTITY KJcy "&#x40C;"><!ENTITY Ubrcy "&#x40E;"><!ENTITY DZcy "&#x40F;"><!ENTITY Acy "&#x410;"><!ENTITY Bcy "&#x411;"><!ENTITY Vcy "&#x412;"><!ENTITY Gcy "&#x413;"><!ENTITY Dcy "&#x414;"><!ENTITY IEcy "&#x415;"><!ENTITY ZHcy "&#x416;"><!ENTITY Zcy "&#x417;"><!ENTITY Icy "&#x418;"><!ENTITY Jcy "&#x419;"><!ENTITY Kcy "&#x41A;"><!ENTITY Lcy "&#x41B;"><!ENTITY Mcy "&#x41C;"><!ENTITY Ncy "&#x41D;"><!ENTITY Ocy "&#x41E;"><!ENTITY Pcy "&#x41F;"><!ENTITY Rcy "&#x420;"><!ENTITY Scy "&#x421;"><!ENTITY Tcy "&#x422;"><!ENTITY Ucy "&#x423;"><!ENTITY Fcy "&#x424;"><!ENTITY KHcy "&#x425;"><!ENTITY TScy "&#x426;"><!ENTITY CHcy "&#x427;"><!ENTITY SHcy "&#x428;"><!ENTITY SHCHcy "&#x429;"><!ENTITY HARDcy "&#x42A;"><!ENTITY Ycy "&#x42B;"><!ENTITY SOFTcy "&#x42C;"><!ENTITY Ecy "&#x42D;"><!ENTITY YUcy "&#x42E;"><!ENTITY YAcy "&#x42F;"><!ENTITY acy "&#x430;"><!ENTITY bcy "&#x431;"><!ENTITY vcy "&#x432;"><!ENTITY gcy "&#x433;"><!ENTITY dcy "&#x434;"><!ENTITY iecy "&#x435;"><!ENTITY zhcy "&#x436;"><!ENTITY zcy "&#x437;"><!ENTITY icy "&#x438;"><!ENTITY jcy "&#x439;"><!ENTITY kcy "&#x43A;"><!ENTITY lcy "&#x43B;"><!ENTITY mcy "&#x43C;"><!ENTITY ncy "&#x43D;"><!ENTITY ocy "&#x43E;"><!ENTITY pcy "&#x43F;"><!ENTITY rcy "&#x440;"><!ENTITY scy "&#x441;"><!ENTITY tcy "&#x442;"><!ENTITY ucy "&#x443;"><!ENTITY fcy "&#x444;"><!ENTITY khcy "&#x445;"><!ENTITY tscy "&#x446;"><!ENTITY chcy "&#x447;"><!ENTITY shcy "&#x448;"><!ENTITY shchcy "&#x449;"><!ENTITY hardcy "&#x44A;"><!ENTITY ycy "&#x44B;"><!ENTITY softcy "&#x44C;"><!ENTITY ecy "&#x44D;"><!ENTITY yucy "&#x44E;"><!ENTITY yacy "&#x44F;"><!ENTITY iocy "&#x451;"><!ENTITY djcy "&#x452;"><!ENTITY gjcy "&#x453;"><!ENTITY jukcy "&#x454;"><!ENTITY dscy "&#x455;"><!ENTITY iukcy "&#x456;"><!ENTITY yicy "&#x457;"><!ENTITY jsercy "&#x458;"><!ENTITY ljcy "&#x459;"><!ENTITY njcy "&#x45A;"><!ENTITY tshcy "&#x45B;"><!ENTITY kjcy "&#x45C;"><!ENTITY ubrcy "&#x45E;"><!ENTITY dzcy "&#x45F;"><!ENTITY ensp "&#x2002;"><!ENTITY emsp "&#x2003;"><!ENTITY emsp13 "&#x2004;"><!ENTITY emsp14 "&#x2005;"><!ENTITY numsp "&#x2007;"><!ENTITY puncsp "&#x2008;"><!ENTITY thinsp "&#x2009;"><!ENTITY ThinSpace "&#x2009;"><!ENTITY hairsp "&#x200A;"><!ENTITY VeryThinSpace "&#x200A;"><!ENTITY ZeroWidthSpace "&#x200B;"><!ENTITY NegativeVeryThinSpace "&#x200B;"><!ENTITY NegativeThinSpace "&#x200B;"><!ENTITY NegativeMediumSpace "&#x200B;"><!ENTITY NegativeThickSpace "&#x200B;"><!ENTITY zwnj "&#x200C;"><!ENTITY zwj "&#x200D;"><!ENTITY lrm "&#x200E;"><!ENTITY rlm "&#x200F;"><!ENTITY hyphen "&#x2010;"><!ENTITY dash "&#x2010;"><!ENTITY ndash "&#x2013;"><!ENTITY mdash "&#x2014;"><!ENTITY horbar "&#x2015;"><!ENTITY Verbar "&#x2016;"><!ENTITY Vert "&#x2016;"><!ENTITY lsquo "&#x2018;"><!ENTITY OpenCurlyQuote "&#x2018;"><!ENTITY rsquo "&#x2019;"><!ENTITY rsquor "&#x2019;"><!ENTITY CloseCurlyQuote "&#x2019;"><!ENTITY lsquor "&#x201A;"><!ENTITY sbquo "&#x201A;"><!ENTITY ldquo "&#x201C;"><!ENTITY OpenCurlyDoubleQuote "&#x201C;"><!ENTITY rdquo "&#x201D;"><!ENTITY rdquor "&#x201D;"><!ENTITY CloseCurlyDoubleQuote "&#x201D;"><!ENTITY ldquor "&#x201E;"><!ENTITY bdquo "&#x201E;"><!ENTITY dagger "&#x2020;"><!ENTITY Dagger "&#x2021;"><!ENTITY ddagger "&#x2021;"><!ENTITY bull "&#x2022;"><!ENTITY bullet "&#x2022;"><!ENTITY nldr "&#x2025;"><!ENTITY hellip "&#x2026;"><!ENTITY mldr "&#x2026;"><!ENTITY permil "&#x2030;"><!ENTITY pertenk "&#x2031;"><!ENTITY prime "&#x2032;"><!ENTITY Prime "&#x2033;"><!ENTITY tprime "&#x2034;"><!ENTITY bprime "&#x2035;"><!ENTITY backprime "&#x2035;"><!ENTITY lsaquo "&#x2039;"><!ENTITY rsaquo "&#x203A;"><!ENTITY oline "&#x203E;"><!ENTITY OverBar "&#x203E;"><!ENTITY caret "&#x2041;"><!ENTITY hybull "&#x2043;"><!ENTITY frasl "&#x2044;"><!ENTITY bsemi "&#x204F;"><!ENTITY qprime "&#x2057;"><!ENTITY MediumSpace "&#x205F;"><!ENTITY ThickSpace "&#x205F;&#x200A;"><!ENTITY NoBreak "&#x2060;"><!ENTITY ApplyFunction "&#x2061;"><!ENTITY af "&#x2061;"><!ENTITY InvisibleTimes "&#x2062;"><!ENTITY it "&#x2062;"><!ENTITY InvisibleComma "&#x2063;"><!ENTITY ic "&#x2063;"><!ENTITY euro "&#x20AC;"><!ENTITY tdot "&#x20DB;"><!ENTITY TripleDot "&#x20DB;"><!ENTITY DotDot "&#x20DC;"><!ENTITY Copf "&#x2102;"><!ENTITY complexes "&#x2102;"><!ENTITY incare "&#x2105;"><!ENTITY gscr "&#x210A;"><!ENTITY hamilt "&#x210B;"><!ENTITY HilbertSpace "&#x210B;"><!ENTITY Hscr "&#x210B;"><!ENTITY Hfr "&#x210C;"><!ENTITY Poincareplane "&#x210C;"><!ENTITY quaternions "&#x210D;"><!ENTITY Hopf "&#x210D;"><!ENTITY planckh "&#x210E;"><!ENTITY planck "&#x210F;"><!ENTITY hbar "&#x210F;"><!ENTITY plankv "&#x210F;"><!ENTITY hslash "&#x210F;"><!ENTITY Iscr "&#x2110;"><!ENTITY imagline "&#x2110;"><!ENTITY image "&#x2111;"><!ENTITY Im "&#x2111;"><!ENTITY imagpart "&#x2111;"><!ENTITY Ifr "&#x2111;"><!ENTITY Lscr "&#x2112;"><!ENTITY lagran "&#x2112;"><!ENTITY Laplacetrf "&#x2112;"><!ENTITY ell "&#x2113;"><!ENTITY Nopf "&#x2115;"><!ENTITY naturals "&#x2115;"><!ENTITY numero "&#x2116;"><!ENTITY copysr "&#x2117;"><!ENTITY weierp "&#x2118;"><!ENTITY wp "&#x2118;"><!ENTITY Popf "&#x2119;"><!ENTITY primes "&#x2119;"><!ENTITY rationals "&#x211A;"><!ENTITY Qopf "&#x211A;"><!ENTITY Rscr "&#x211B;"><!ENTITY realine "&#x211B;"><!ENTITY real "&#x211C;"><!ENTITY Re "&#x211C;"><!ENTITY realpart "&#x211C;"><!ENTITY Rfr "&#x211C;"><!ENTITY reals "&#x211D;"><!ENTITY Ropf "&#x211D;"><!ENTITY rx "&#x211E;"><!ENTITY trade "&#x2122;"><!ENTITY TRADE "&#x2122;"><!ENTITY integers "&#x2124;"><!ENTITY Zopf "&#x2124;"><!ENTITY mho "&#x2127;"><!ENTITY Zfr "&#x2128;"><!ENTITY zeetrf "&#x2128;"><!ENTITY iiota "&#x2129;"><!ENTITY bernou "&#x212C;"><!ENTITY Bernoullis "&#x212C;"><!ENTITY Bscr "&#x212C;"><!ENTITY Cfr "&#x212D;"><!ENTITY Cayleys "&#x212D;"><!ENTITY escr "&#x212F;"><!ENTITY Escr "&#x2130;"><!ENTITY expectation "&#x2130;"><!ENTITY Fscr "&#x2131;"><!ENTITY Fouriertrf "&#x2131;"><!ENTITY phmmat "&#x2133;"><!ENTITY Mellintrf "&#x2133;"><!ENTITY Mscr "&#x2133;"><!ENTITY order "&#x2134;"><!ENTITY orderof "&#x2134;"><!ENTITY oscr "&#x2134;"><!ENTITY alefsym "&#x2135;"><!ENTITY aleph "&#x2135;"><!ENTITY beth "&#x2136;"><!ENTITY gimel "&#x2137;"><!ENTITY daleth "&#x2138;"><!ENTITY CapitalDifferentialD "&#x2145;"><!ENTITY DD "&#x2145;"><!ENTITY DifferentialD "&#x2146;"><!ENTITY dd "&#x2146;"><!ENTITY ExponentialE "&#x2147;"><!ENTITY exponentiale "&#x2147;"><!ENTITY ee "&#x2147;"><!ENTITY ImaginaryI "&#x2148;"><!ENTITY ii "&#x2148;"><!ENTITY frac13 "&#x2153;"><!ENTITY frac23 "&#x2154;"><!ENTITY frac15 "&#x2155;"><!ENTITY frac25 "&#x2156;"><!ENTITY frac35 "&#x2157;"><!ENTITY frac45 "&#x2158;"><!ENTITY frac16 "&#x2159;"><!ENTITY frac56 "&#x215A;"><!ENTITY frac18 "&#x215B;"><!ENTITY frac38 "&#x215C;"><!ENTITY frac58 "&#x215D;"><!ENTITY frac78 "&#x215E;"><!ENTITY larr "&#x2190;"><!ENTITY leftarrow "&#x2190;"><!ENTITY LeftArrow "&#x2190;"><!ENTITY slarr "&#x2190;"><!ENTITY ShortLeftArrow "&#x2190;"><!ENTITY uarr "&#x2191;"><!ENTITY uparrow "&#x2191;"><!ENTITY UpArrow "&#x2191;"><!ENTITY ShortUpArrow "&#x2191;"><!ENTITY rarr "&#x2192;"><!ENTITY rightarrow "&#x2192;"><!ENTITY RightArrow "&#x2192;"><!ENTITY srarr "&#x2192;"><!ENTITY ShortRightArrow "&#x2192;"><!ENTITY darr "&#x2193;"><!ENTITY downarrow "&#x2193;"><!ENTITY DownArrow "&#x2193;"><!ENTITY ShortDownArrow "&#x2193;"><!ENTITY harr "&#x2194;"><!ENTITY leftrightarrow "&#x2194;"><!ENTITY LeftRightArrow "&#x2194;"><!ENTITY varr "&#x2195;"><!ENTITY updownarrow "&#x2195;"><!ENTITY UpDownArrow "&#x2195;"><!ENTITY nwarr "&#x2196;"><!ENTITY UpperLeftArrow "&#x2196;"><!ENTITY nwarrow "&#x2196;"><!ENTITY nearr "&#x2197;"><!ENTITY UpperRightArrow "&#x2197;"><!ENTITY nearrow "&#x2197;"><!ENTITY searr "&#x2198;"><!ENTITY searrow "&#x2198;"><!ENTITY LowerRightArrow "&#x2198;"><!ENTITY swarr "&#x2199;"><!ENTITY swarrow "&#x2199;"><!ENTITY LowerLeftArrow "&#x2199;"><!ENTITY nlarr "&#x219A;"><!ENTITY nleftarrow "&#x219A;"><!ENTITY nrarr "&#x219B;"><!ENTITY nrightarrow "&#x219B;"><!ENTITY rarrw "&#x219D;"><!ENTITY rightsquigarrow "&#x219D;"><!ENTITY nrarrw "&#x219D;&#x338;"><!ENTITY Larr "&#x219E;"><!ENTITY twoheadleftarrow "&#x219E;"><!ENTITY Uarr "&#x219F;"><!ENTITY Rarr "&#x21A0;"><!ENTITY twoheadrightarrow "&#x21A0;"><!ENTITY Darr "&#x21A1;"><!ENTITY larrtl "&#x21A2;"><!ENTITY leftarrowtail "&#x21A2;"><!ENTITY rarrtl "&#x21A3;"><!ENTITY rightarrowtail "&#x21A3;"><!ENTITY LeftTeeArrow "&#x21A4;"><!ENTITY mapstoleft "&#x21A4;"><!ENTITY UpTeeArrow "&#x21A5;"><!ENTITY mapstoup "&#x21A5;"><!ENTITY map "&#x21A6;"><!ENTITY RightTeeArrow "&#x21A6;"><!ENTITY mapsto "&#x21A6;"><!ENTITY DownTeeArrow "&#x21A7;"><!ENTITY mapstodown "&#x21A7;"><!ENTITY larrhk "&#x21A9;"><!ENTITY hookleftarrow "&#x21A9;"><!ENTITY rarrhk "&#x21AA;"><!ENTITY hookrightarrow "&#x21AA;"><!ENTITY larrlp "&#x21AB;"><!ENTITY looparrowleft "&#x21AB;"><!ENTITY rarrlp "&#x21AC;"><!ENTITY looparrowright "&#x21AC;"><!ENTITY harrw "&#x21AD;"><!ENTITY leftrightsquigarrow "&#x21AD;"><!ENTITY nharr "&#x21AE;"><!ENTITY nleftrightarrow "&#x21AE;"><!ENTITY lsh "&#x21B0;"><!ENTITY Lsh "&#x21B0;"><!ENTITY rsh "&#x21B1;"><!ENTITY Rsh "&#x21B1;"><!ENTITY ldsh "&#x21B2;"><!ENTITY rdsh "&#x21B3;"><!ENTITY crarr "&#x21B5;"><!ENTITY cularr "&#x21B6;"><!ENTITY curvearrowleft "&#x21B6;"><!ENTITY curarr "&#x21B7;"><!ENTITY curvearrowright "&#x21B7;"><!ENTITY olarr "&#x21BA;"><!ENTITY circlearrowleft "&#x21BA;"><!ENTITY orarr "&#x21BB;"><!ENTITY circlearrowright "&#x21BB;"><!ENTITY lharu "&#x21BC;"><!ENTITY LeftVector "&#x21BC;"><!ENTITY leftharpoonup "&#x21BC;"><!ENTITY lhard "&#x21BD;"><!ENTITY leftharpoondown "&#x21BD;"><!ENTITY DownLeftVector "&#x21BD;"><!ENTITY uharr "&#x21BE;"><!ENTITY upharpoonright "&#x21BE;"><!ENTITY RightUpVector "&#x21BE;"><!ENTITY uharl "&#x21BF;"><!ENTITY upharpoonleft "&#x21BF;"><!ENTITY LeftUpVector "&#x21BF;"><!ENTITY rharu "&#x21C0;"><!ENTITY RightVector "&#x21C0;"><!ENTITY rightharpoonup "&#x21C0;"><!ENTITY rhard "&#x21C1;"><!ENTITY rightharpoondown "&#x21C1;"><!ENTITY DownRightVector "&#x21C1;"><!ENTITY dharr "&#x21C2;"><!ENTITY RightDownVector "&#x21C2;"><!ENTITY downharpoonright "&#x21C2;"><!ENTITY dharl "&#x21C3;"><!ENTITY LeftDownVector "&#x21C3;"><!ENTITY downharpoonleft "&#x21C3;"><!ENTITY rlarr "&#x21C4;"><!ENTITY rightleftarrows "&#x21C4;"><!ENTITY RightArrowLeftArrow "&#x21C4;"><!ENTITY udarr "&#x21C5;"><!ENTITY UpArrowDownArrow "&#x21C5;"><!ENTITY lrarr "&#x21C6;"><!ENTITY leftrightarrows "&#x21C6;"><!ENTITY LeftArrowRightArrow "&#x21C6;"><!ENTITY llarr "&#x21C7;"><!ENTITY leftleftarrows "&#x21C7;"><!ENTITY uuarr "&#x21C8;"><!ENTITY upuparrows "&#x21C8;"><!ENTITY rrarr "&#x21C9;"><!ENTITY rightrightarrows "&#x21C9;"><!ENTITY ddarr "&#x21CA;"><!ENTITY downdownarrows "&#x21CA;"><!ENTITY lrhar "&#x21CB;"><!ENTITY ReverseEquilibrium "&#x21CB;"><!ENTITY leftrightharpoons "&#x21CB;"><!ENTITY rlhar "&#x21CC;"><!ENTITY rightleftharpoons "&#x21CC;"><!ENTITY Equilibrium "&#x21CC;"><!ENTITY nlArr "&#x21CD;"><!ENTITY nLeftarrow "&#x21CD;"><!ENTITY nhArr "&#x21CE;"><!ENTITY nLeftrightarrow "&#x21CE;"><!ENTITY nrArr "&#x21CF;"><!ENTITY nRightarrow "&#x21CF;"><!ENTITY lArr "&#x21D0;"><!ENTITY Leftarrow "&#x21D0;"><!ENTITY DoubleLeftArrow "&#x21D0;"><!ENTITY uArr "&#x21D1;"><!ENTITY Uparrow "&#x21D1;"><!ENTITY DoubleUpArrow "&#x21D1;"><!ENTITY rArr "&#x21D2;"><!ENTITY Rightarrow "&#x21D2;"><!ENTITY Implies "&#x21D2;"><!ENTITY DoubleRightArrow "&#x21D2;"><!ENTITY dArr "&#x21D3;"><!ENTITY Downarrow "&#x21D3;"><!ENTITY DoubleDownArrow "&#x21D3;"><!ENTITY hArr "&#x21D4;"><!ENTITY Leftrightarrow "&#x21D4;"><!ENTITY DoubleLeftRightArrow "&#x21D4;"><!ENTITY iff "&#x21D4;"><!ENTITY vArr "&#x21D5;"><!ENTITY Updownarrow "&#x21D5;"><!ENTITY DoubleUpDownArrow "&#x21D5;"><!ENTITY nwArr "&#x21D6;"><!ENTITY neArr "&#x21D7;"><!ENTITY seArr "&#x21D8;"><!ENTITY swArr "&#x21D9;"><!ENTITY lAarr "&#x21DA;"><!ENTITY Lleftarrow "&#x21DA;"><!ENTITY rAarr "&#x21DB;"><!ENTITY Rrightarrow "&#x21DB;"><!ENTITY zigrarr "&#x21DD;"><!ENTITY larrb "&#x21E4;"><!ENTITY LeftArrowBar "&#x21E4;"><!ENTITY rarrb "&#x21E5;"><!ENTITY RightArrowBar "&#x21E5;"><!ENTITY duarr "&#x21F5;"><!ENTITY DownArrowUpArrow "&#x21F5;"><!ENTITY loarr "&#x21FD;"><!ENTITY roarr "&#x21FE;"><!ENTITY hoarr "&#x21FF;"><!ENTITY forall "&#x2200;"><!ENTITY ForAll "&#x2200;"><!ENTITY comp "&#x2201;"><!ENTITY complement "&#x2201;"><!ENTITY part "&#x2202;"><!ENTITY PartialD "&#x2202;"><!ENTITY npart "&#x2202;&#x338;"><!ENTITY exist "&#x2203;"><!ENTITY Exists "&#x2203;"><!ENTITY nexist "&#x2204;"><!ENTITY NotExists "&#x2204;"><!ENTITY nexists "&#x2204;"><!ENTITY empty "&#x2205;"><!ENTITY emptyset "&#x2205;"><!ENTITY emptyv "&#x2205;"><!ENTITY varnothing "&#x2205;"><!ENTITY nabla "&#x2207;"><!ENTITY Del "&#x2207;"><!ENTITY isin "&#x2208;"><!ENTITY isinv "&#x2208;"><!ENTITY Element "&#x2208;"><!ENTITY in "&#x2208;"><!ENTITY notin "&#x2209;"><!ENTITY NotElement "&#x2209;"><!ENTITY notinva "&#x2209;"><!ENTITY niv "&#x220B;"><!ENTITY ReverseElement "&#x220B;"><!ENTITY ni "&#x220B;"><!ENTITY SuchThat "&#x220B;"><!ENTITY notni "&#x220C;"><!ENTITY notniva "&#x220C;"><!ENTITY NotReverseElement "&#x220C;"><!ENTITY prod "&#x220F;"><!ENTITY Product "&#x220F;"><!ENTITY coprod "&#x2210;"><!ENTITY Coproduct "&#x2210;"><!ENTITY sum "&#x2211;"><!ENTITY Sum "&#x2211;"><!ENTITY minus "&#x2212;"><!ENTITY mnplus "&#x2213;"><!ENTITY mp "&#x2213;"><!ENTITY MinusPlus "&#x2213;"><!ENTITY plusdo "&#x2214;"><!ENTITY dotplus "&#x2214;"><!ENTITY setmn "&#x2216;"><!ENTITY setminus "&#x2216;"><!ENTITY Backslash "&#x2216;"><!ENTITY ssetmn "&#x2216;"><!ENTITY smallsetminus "&#x2216;"><!ENTITY lowast "&#x2217;"><!ENTITY compfn "&#x2218;"><!ENTITY SmallCircle "&#x2218;"><!ENTITY radic "&#x221A;"><!ENTITY Sqrt "&#x221A;"><!ENTITY prop "&#x221D;"><!ENTITY propto "&#x221D;"><!ENTITY Proportional "&#x221D;"><!ENTITY vprop "&#x221D;"><!ENTITY varpropto "&#x221D;"><!ENTITY infin "&#x221E;"><!ENTITY angrt "&#x221F;"><!ENTITY ang "&#x2220;"><!ENTITY angle "&#x2220;"><!ENTITY nang "&#x2220;&#x20D2;"><!ENTITY angmsd "&#x2221;"><!ENTITY measuredangle "&#x2221;"><!ENTITY angsph "&#x2222;"><!ENTITY mid "&#x2223;"><!ENTITY VerticalBar "&#x2223;"><!ENTITY smid "&#x2223;"><!ENTITY shortmid "&#x2223;"><!ENTITY nmid "&#x2224;"><!ENTITY NotVerticalBar "&#x2224;"><!ENTITY nsmid "&#x2224;"><!ENTITY nshortmid "&#x2224;"><!ENTITY par "&#x2225;"><!ENTITY parallel "&#x2225;"><!ENTITY DoubleVerticalBar "&#x2225;"><!ENTITY spar "&#x2225;"><!ENTITY shortparallel "&#x2225;"><!ENTITY npar "&#x2226;"><!ENTITY nparallel "&#x2226;"><!ENTITY NotDoubleVerticalBar "&#x2226;"><!ENTITY nspar "&#x2226;"><!ENTITY nshortparallel "&#x2226;"><!ENTITY and "&#x2227;"><!ENTITY wedge "&#x2227;"><!ENTITY or "&#x2228;"><!ENTITY vee "&#x2228;"><!ENTITY cap "&#x2229;"><!ENTITY caps "&#x2229;&#xFE00;"><!ENTITY cup "&#x222A;"><!ENTITY cups "&#x222A;&#xFE00;"><!ENTITY int "&#x222B;"><!ENTITY Integral "&#x222B;"><!ENTITY Int "&#x222C;"><!ENTITY tint "&#x222D;"><!ENTITY iiint "&#x222D;"><!ENTITY conint "&#x222E;"><!ENTITY oint "&#x222E;"><!ENTITY ContourIntegral "&#x222E;"><!ENTITY Conint "&#x222F;"><!ENTITY DoubleContourIntegral "&#x222F;"><!ENTITY Cconint "&#x2230;"><!ENTITY cwint "&#x2231;"><!ENTITY cwconint "&#x2232;"><!ENTITY ClockwiseContourIntegral "&#x2232;"><!ENTITY awconint "&#x2233;"><!ENTITY CounterClockwiseContourIntegral "&#x2233;"><!ENTITY there4 "&#x2234;"><!ENTITY therefore "&#x2234;"><!ENTITY Therefore "&#x2234;"><!ENTITY becaus "&#x2235;"><!ENTITY because "&#x2235;"><!ENTITY Because "&#x2235;"><!ENTITY ratio "&#x2236;"><!ENTITY Colon "&#x2237;"><!ENTITY Proportion "&#x2237;"><!ENTITY minusd "&#x2238;"><!ENTITY dotminus "&#x2238;"><!ENTITY mDDot "&#x223A;"><!ENTITY homtht "&#x223B;"><!ENTITY sim "&#x223C;"><!ENTITY Tilde "&#x223C;"><!ENTITY thksim "&#x223C;"><!ENTITY thicksim "&#x223C;"><!ENTITY nvsim "&#x223C;&#x20D2;"><!ENTITY bsim "&#x223D;"><!ENTITY backsim "&#x223D;"><!ENTITY race "&#x223D;&#x331;"><!ENTITY ac "&#x223E;"><!ENTITY mstpos "&#x223E;"><!ENTITY acE "&#x223E;&#x333;"><!ENTITY acd "&#x223F;"><!ENTITY wreath "&#x2240;"><!ENTITY VerticalTilde "&#x2240;"><!ENTITY wr "&#x2240;"><!ENTITY nsim "&#x2241;"><!ENTITY NotTilde "&#x2241;"><!ENTITY esim "&#x2242;"><!ENTITY EqualTilde "&#x2242;"><!ENTITY eqsim "&#x2242;"><!ENTITY NotEqualTilde "&#x2242;&#x338;"><!ENTITY nesim "&#x2242;&#x338;"><!ENTITY sime "&#x2243;"><!ENTITY TildeEqual "&#x2243;"><!ENTITY simeq "&#x2243;"><!ENTITY nsime "&#x2244;"><!ENTITY nsimeq "&#x2244;"><!ENTITY NotTildeEqual "&#x2244;"><!ENTITY cong "&#x2245;"><!ENTITY TildeFullEqual "&#x2245;"><!ENTITY simne "&#x2246;"><!ENTITY ncong "&#x2247;"><!ENTITY NotTildeFullEqual "&#x2247;"><!ENTITY asymp "&#x2248;"><!ENTITY ap "&#x2248;"><!ENTITY TildeTilde "&#x2248;"><!ENTITY approx "&#x2248;"><!ENTITY thkap "&#x2248;"><!ENTITY thickapprox "&#x2248;"><!ENTITY nap "&#x2249;"><!ENTITY NotTildeTilde "&#x2249;"><!ENTITY napprox "&#x2249;"><!ENTITY ape "&#x224A;"><!ENTITY approxeq "&#x224A;"><!ENTITY apid "&#x224B;"><!ENTITY napid "&#x224B;&#x338;"><!ENTITY bcong "&#x224C;"><!ENTITY backcong "&#x224C;"><!ENTITY asympeq "&#x224D;"><!ENTITY CupCap "&#x224D;"><!ENTITY nvap "&#x224D;&#x20D2;"><!ENTITY bump "&#x224E;"><!ENTITY HumpDownHump "&#x224E;"><!ENTITY Bumpeq "&#x224E;"><!ENTITY NotHumpDownHump "&#x224E;&#x338;"><!ENTITY nbump "&#x224E;&#x338;"><!ENTITY bumpe "&#x224F;"><!ENTITY HumpEqual "&#x224F;"><!ENTITY bumpeq "&#x224F;"><!ENTITY nbumpe "&#x224F;&#x338;"><!ENTITY NotHumpEqual "&#x224F;&#x338;"><!ENTITY esdot "&#x2250;"><!ENTITY DotEqual "&#x2250;"><!ENTITY doteq "&#x2250;"><!ENTITY nedot "&#x2250;&#x338;"><!ENTITY eDot "&#x2251;"><!ENTITY doteqdot "&#x2251;"><!ENTITY efDot "&#x2252;"><!ENTITY fallingdotseq "&#x2252;"><!ENTITY erDot "&#x2253;"><!ENTITY risingdotseq "&#x2253;"><!ENTITY colone "&#x2254;"><!ENTITY coloneq "&#x2254;"><!ENTITY Assign "&#x2254;"><!ENTITY ecolon "&#x2255;"><!ENTITY eqcolon "&#x2255;"><!ENTITY ecir "&#x2256;"><!ENTITY eqcirc "&#x2256;"><!ENTITY cire "&#x2257;"><!ENTITY circeq "&#x2257;"><!ENTITY wedgeq "&#x2259;"><!ENTITY veeeq "&#x225A;"><!ENTITY trie "&#x225C;"><!ENTITY triangleq "&#x225C;"><!ENTITY equest "&#x225F;"><!ENTITY questeq "&#x225F;"><!ENTITY ne "&#x2260;"><!ENTITY NotEqual "&#x2260;"><!ENTITY equiv "&#x2261;"><!ENTITY Congruent "&#x2261;"><!ENTITY bnequiv "&#x2261;&#x20E5;"><!ENTITY nequiv "&#x2262;"><!ENTITY NotCongruent "&#x2262;"><!ENTITY le "&#x2264;"><!ENTITY leq "&#x2264;"><!ENTITY nvle "&#x2264;&#x20D2;"><!ENTITY ge "&#x2265;"><!ENTITY GreaterEqual "&#x2265;"><!ENTITY geq "&#x2265;"><!ENTITY nvge "&#x2265;&#x20D2;"><!ENTITY lE "&#x2266;"><!ENTITY LessFullEqual "&#x2266;"><!ENTITY leqq "&#x2266;"><!ENTITY nlE "&#x2266;&#x338;"><!ENTITY nleqq "&#x2266;&#x338;"><!ENTITY gE "&#x2267;"><!ENTITY GreaterFullEqual "&#x2267;"><!ENTITY geqq "&#x2267;"><!ENTITY ngE "&#x2267;&#x338;"><!ENTITY ngeqq "&#x2267;&#x338;"><!ENTITY NotGreaterFullEqual "&#x2267;&#x338;"><!ENTITY lnE "&#x2268;"><!ENTITY lneqq "&#x2268;"><!ENTITY lvnE "&#x2268;&#xFE00;"><!ENTITY lvertneqq "&#x2268;&#xFE00;"><!ENTITY gnE "&#x2269;"><!ENTITY gneqq "&#x2269;"><!ENTITY gvnE "&#x2269;&#xFE00;"><!ENTITY gvertneqq "&#x2269;&#xFE00;"><!ENTITY Lt "&#x226A;"><!ENTITY NestedLessLess "&#x226A;"><!ENTITY ll "&#x226A;"><!ENTITY nLtv "&#x226A;&#x338;"><!ENTITY NotLessLess "&#x226A;&#x338;"><!ENTITY nLt "&#x226A;&#x20D2;"><!ENTITY Gt "&#x226B;"><!ENTITY NestedGreaterGreater "&#x226B;"><!ENTITY gg "&#x226B;"><!ENTITY nGtv "&#x226B;&#x338;"><!ENTITY NotGreaterGreater "&#x226B;&#x338;"><!ENTITY nGt "&#x226B;&#x20D2;"><!ENTITY twixt "&#x226C;"><!ENTITY between "&#x226C;"><!ENTITY NotCupCap "&#x226D;"><!ENTITY nlt "&#x226E;"><!ENTITY NotLess "&#x226E;"><!ENTITY nless "&#x226E;"><!ENTITY ngt "&#x226F;"><!ENTITY NotGreater "&#x226F;"><!ENTITY ngtr "&#x226F;"><!ENTITY nle "&#x2270;"><!ENTITY NotLessEqual "&#x2270;"><!ENTITY nleq "&#x2270;"><!ENTITY nge "&#x2271;"><!ENTITY NotGreaterEqual "&#x2271;"><!ENTITY ngeq "&#x2271;"><!ENTITY lsim "&#x2272;"><!ENTITY LessTilde "&#x2272;"><!ENTITY lesssim "&#x2272;"><!ENTITY gsim "&#x2273;"><!ENTITY gtrsim "&#x2273;"><!ENTITY GreaterTilde "&#x2273;"><!ENTITY nlsim "&#x2274;"><!ENTITY NotLessTilde "&#x2274;"><!ENTITY ngsim "&#x2275;"><!ENTITY NotGreaterTilde "&#x2275;"><!ENTITY lg "&#x2276;"><!ENTITY lessgtr "&#x2276;"><!ENTITY LessGreater "&#x2276;"><!ENTITY gl "&#x2277;"><!ENTITY gtrless "&#x2277;"><!ENTITY GreaterLess "&#x2277;"><!ENTITY ntlg "&#x2278;"><!ENTITY NotLessGreater "&#x2278;"><!ENTITY ntgl "&#x2279;"><!ENTITY NotGreaterLess "&#x2279;"><!ENTITY pr "&#x227A;"><!ENTITY Precedes "&#x227A;"><!ENTITY prec "&#x227A;"><!ENTITY sc "&#x227B;"><!ENTITY Succeeds "&#x227B;"><!ENTITY succ "&#x227B;"><!ENTITY prcue "&#x227C;"><!ENTITY PrecedesSlantEqual "&#x227C;"><!ENTITY preccurlyeq "&#x227C;"><!ENTITY sccue "&#x227D;"><!ENTITY SucceedsSlantEqual "&#x227D;"><!ENTITY succcurlyeq "&#x227D;"><!ENTITY prsim "&#x227E;"><!ENTITY precsim "&#x227E;"><!ENTITY PrecedesTilde "&#x227E;"><!ENTITY scsim "&#x227F;"><!ENTITY succsim "&#x227F;"><!ENTITY SucceedsTilde "&#x227F;"><!ENTITY NotSucceedsTilde "&#x227F;&#x338;"><!ENTITY npr "&#x2280;"><!ENTITY nprec "&#x2280;"><!ENTITY NotPrecedes "&#x2280;"><!ENTITY nsc "&#x2281;"><!ENTITY nsucc "&#x2281;"><!ENTITY NotSucceeds "&#x2281;"><!ENTITY sub "&#x2282;"><!ENTITY subset "&#x2282;"><!ENTITY vnsub "&#x2282;&#x20D2;"><!ENTITY nsubset "&#x2282;&#x20D2;"><!ENTITY NotSubset "&#x2282;&#x20D2;"><!ENTITY sup "&#x2283;"><!ENTITY supset "&#x2283;"><!ENTITY Superset "&#x2283;"><!ENTITY vnsup "&#x2283;&#x20D2;"><!ENTITY nsupset "&#x2283;&#x20D2;"><!ENTITY NotSuperset "&#x2283;&#x20D2;"><!ENTITY nsub "&#x2284;"><!ENTITY nsup "&#x2285;"><!ENTITY sube "&#x2286;"><!ENTITY SubsetEqual "&#x2286;"><!ENTITY subseteq "&#x2286;"><!ENTITY supe "&#x2287;"><!ENTITY supseteq "&#x2287;"><!ENTITY SupersetEqual "&#x2287;"><!ENTITY nsube "&#x2288;"><!ENTITY nsubseteq "&#x2288;"><!ENTITY NotSubsetEqual "&#x2288;"><!ENTITY nsupe "&#x2289;"><!ENTITY nsupseteq "&#x2289;"><!ENTITY NotSupersetEqual "&#x2289;"><!ENTITY subne "&#x228A;"><!ENTITY subsetneq "&#x228A;"><!ENTITY vsubne "&#x228A;&#xFE00;"><!ENTITY varsubsetneq "&#x228A;&#xFE00;"><!ENTITY supne "&#x228B;"><!ENTITY supsetneq "&#x228B;"><!ENTITY vsupne "&#x228B;&#xFE00;"><!ENTITY varsupsetneq "&#x228B;&#xFE00;"><!ENTITY cupdot "&#x228D;"><!ENTITY uplus "&#x228E;"><!ENTITY UnionPlus "&#x228E;"><!ENTITY sqsub "&#x228F;"><!ENTITY SquareSubset "&#x228F;"><!ENTITY sqsubset "&#x228F;"><!ENTITY NotSquareSubset "&#x228F;&#x338;"><!ENTITY sqsup "&#x2290;"><!ENTITY SquareSuperset "&#x2290;"><!ENTITY sqsupset "&#x2290;"><!ENTITY NotSquareSuperset "&#x2290;&#x338;"><!ENTITY sqsube "&#x2291;"><!ENTITY SquareSubsetEqual "&#x2291;"><!ENTITY sqsubseteq "&#x2291;"><!ENTITY sqsupe "&#x2292;"><!ENTITY SquareSupersetEqual "&#x2292;"><!ENTITY sqsupseteq "&#x2292;"><!ENTITY sqcap "&#x2293;"><!ENTITY SquareIntersection "&#x2293;"><!ENTITY sqcaps "&#x2293;&#xFE00;"><!ENTITY sqcup "&#x2294;"><!ENTITY SquareUnion "&#x2294;"><!ENTITY sqcups "&#x2294;&#xFE00;"><!ENTITY oplus "&#x2295;"><!ENTITY CirclePlus "&#x2295;"><!ENTITY ominus "&#x2296;"><!ENTITY CircleMinus "&#x2296;"><!ENTITY otimes "&#x2297;"><!ENTITY CircleTimes "&#x2297;"><!ENTITY osol "&#x2298;"><!ENTITY odot "&#x2299;"><!ENTITY CircleDot "&#x2299;"><!ENTITY ocir "&#x229A;"><!ENTITY circledcirc "&#x229A;"><!ENTITY oast "&#x229B;"><!ENTITY circledast "&#x229B;"><!ENTITY odash "&#x229D;"><!ENTITY circleddash "&#x229D;"><!ENTITY plusb "&#x229E;"><!ENTITY boxplus "&#x229E;"><!ENTITY minusb "&#x229F;"><!ENTITY boxminus "&#x229F;"><!ENTITY timesb "&#x22A0;"><!ENTITY boxtimes "&#x22A0;"><!ENTITY sdotb "&#x22A1;"><!ENTITY dotsquare "&#x22A1;"><!ENTITY vdash "&#x22A2;"><!ENTITY RightTee "&#x22A2;"><!ENTITY dashv "&#x22A3;"><!ENTITY LeftTee "&#x22A3;"><!ENTITY top "&#x22A4;"><!ENTITY DownTee "&#x22A4;"><!ENTITY bottom "&#x22A5;"><!ENTITY bot "&#x22A5;"><!ENTITY perp "&#x22A5;"><!ENTITY UpTee "&#x22A5;"><!ENTITY models "&#x22A7;"><!ENTITY vDash "&#x22A8;"><!ENTITY DoubleRightTee "&#x22A8;"><!ENTITY Vdash "&#x22A9;"><!ENTITY Vvdash "&#x22AA;"><!ENTITY VDash "&#x22AB;"><!ENTITY nvdash "&#x22AC;"><!ENTITY nvDash "&#x22AD;"><!ENTITY nVdash "&#x22AE;"><!ENTITY nVDash "&#x22AF;"><!ENTITY prurel "&#x22B0;"><!ENTITY vltri "&#x22B2;"><!ENTITY vartriangleleft "&#x22B2;"><!ENTITY LeftTriangle "&#x22B2;"><!ENTITY vrtri "&#x22B3;"><!ENTITY vartriangleright "&#x22B3;"><!ENTITY RightTriangle "&#x22B3;"><!ENTITY ltrie "&#x22B4;"><!ENTITY trianglelefteq "&#x22B4;"><!ENTITY LeftTriangleEqual "&#x22B4;"><!ENTITY nvltrie "&#x22B4;&#x20D2;"><!ENTITY rtrie "&#x22B5;"><!ENTITY trianglerighteq "&#x22B5;"><!ENTITY RightTriangleEqual "&#x22B5;"><!ENTITY nvrtrie "&#x22B5;&#x20D2;"><!ENTITY origof "&#x22B6;"><!ENTITY imof "&#x22B7;"><!ENTITY mumap "&#x22B8;"><!ENTITY multimap "&#x22B8;"><!ENTITY hercon "&#x22B9;"><!ENTITY intcal "&#x22BA;"><!ENTITY intercal "&#x22BA;"><!ENTITY veebar "&#x22BB;"><!ENTITY barvee "&#x22BD;"><!ENTITY angrtvb "&#x22BE;"><!ENTITY lrtri "&#x22BF;"><!ENTITY xwedge "&#x22C0;"><!ENTITY Wedge "&#x22C0;"><!ENTITY bigwedge "&#x22C0;"><!ENTITY xvee "&#x22C1;"><!ENTITY Vee "&#x22C1;"><!ENTITY bigvee "&#x22C1;"><!ENTITY xcap "&#x22C2;"><!ENTITY Intersection "&#x22C2;"><!ENTITY bigcap "&#x22C2;"><!ENTITY xcup "&#x22C3;"><!ENTITY Union "&#x22C3;"><!ENTITY bigcup "&#x22C3;"><!ENTITY diam "&#x22C4;"><!ENTITY diamond "&#x22C4;"><!ENTITY Diamond "&#x22C4;"><!ENTITY sdot "&#x22C5;"><!ENTITY sstarf "&#x22C6;"><!ENTITY Star "&#x22C6;"><!ENTITY divonx "&#x22C7;"><!ENTITY divideontimes "&#x22C7;"><!ENTITY bowtie "&#x22C8;"><!ENTITY ltimes "&#x22C9;"><!ENTITY rtimes "&#x22CA;"><!ENTITY lthree "&#x22CB;"><!ENTITY leftthreetimes "&#x22CB;"><!ENTITY rthree "&#x22CC;"><!ENTITY rightthreetimes "&#x22CC;"><!ENTITY bsime "&#x22CD;"><!ENTITY backsimeq "&#x22CD;"><!ENTITY cuvee "&#x22CE;"><!ENTITY curlyvee "&#x22CE;"><!ENTITY cuwed "&#x22CF;"><!ENTITY curlywedge "&#x22CF;"><!ENTITY Sub "&#x22D0;"><!ENTITY Subset "&#x22D0;"><!ENTITY Sup "&#x22D1;"><!ENTITY Supset "&#x22D1;"><!ENTITY Cap "&#x22D2;"><!ENTITY Cup "&#x22D3;"><!ENTITY fork "&#x22D4;"><!ENTITY pitchfork "&#x22D4;"><!ENTITY epar "&#x22D5;"><!ENTITY ltdot "&#x22D6;"><!ENTITY lessdot "&#x22D6;"><!ENTITY gtdot "&#x22D7;"><!ENTITY gtrdot "&#x22D7;"><!ENTITY Ll "&#x22D8;"><!ENTITY nLl "&#x22D8;&#x338;"><!ENTITY Gg "&#x22D9;"><!ENTITY ggg "&#x22D9;"><!ENTITY nGg "&#x22D9;&#x338;"><!ENTITY leg "&#x22DA;"><!ENTITY LessEqualGreater "&#x22DA;"><!ENTITY lesseqgtr "&#x22DA;"><!ENTITY lesg "&#x22DA;&#xFE00;"><!ENTITY gel "&#x22DB;"><!ENTITY gtreqless "&#x22DB;"><!ENTITY GreaterEqualLess "&#x22DB;"><!ENTITY gesl "&#x22DB;&#xFE00;"><!ENTITY cuepr "&#x22DE;"><!ENTITY curlyeqprec "&#x22DE;"><!ENTITY cuesc "&#x22DF;"><!ENTITY curlyeqsucc "&#x22DF;"><!ENTITY nprcue "&#x22E0;"><!ENTITY NotPrecedesSlantEqual "&#x22E0;"><!ENTITY nsccue "&#x22E1;"><!ENTITY NotSucceedsSlantEqual "&#x22E1;"><!ENTITY nsqsube "&#x22E2;"><!ENTITY NotSquareSubsetEqual "&#x22E2;"><!ENTITY nsqsupe "&#x22E3;"><!ENTITY NotSquareSupersetEqual "&#x22E3;"><!ENTITY lnsim "&#x22E6;"><!ENTITY gnsim "&#x22E7;"><!ENTITY prnsim "&#x22E8;"><!ENTITY precnsim "&#x22E8;"><!ENTITY scnsim "&#x22E9;"><!ENTITY succnsim "&#x22E9;"><!ENTITY nltri "&#x22EA;"><!ENTITY ntriangleleft "&#x22EA;"><!ENTITY NotLeftTriangle "&#x22EA;"><!ENTITY nrtri "&#x22EB;"><!ENTITY ntriangleright "&#x22EB;"><!ENTITY NotRightTriangle "&#x22EB;"><!ENTITY nltrie "&#x22EC;"><!ENTITY ntrianglelefteq "&#x22EC;"><!ENTITY NotLeftTriangleEqual "&#x22EC;"><!ENTITY nrtrie "&#x22ED;"><!ENTITY ntrianglerighteq "&#x22ED;"><!ENTITY NotRightTriangleEqual "&#x22ED;"><!ENTITY vellip "&#x22EE;"><!ENTITY ctdot "&#x22EF;"><!ENTITY utdot "&#x22F0;"><!ENTITY dtdot "&#x22F1;"><!ENTITY disin "&#x22F2;"><!ENTITY isinsv "&#x22F3;"><!ENTITY isins "&#x22F4;"><!ENTITY isindot "&#x22F5;"><!ENTITY notindot "&#x22F5;&#x338;"><!ENTITY notinvc "&#x22F6;"><!ENTITY notinvb "&#x22F7;"><!ENTITY isinE "&#x22F9;"><!ENTITY notinE "&#x22F9;&#x338;"><!ENTITY nisd "&#x22FA;"><!ENTITY xnis "&#x22FB;"><!ENTITY nis "&#x22FC;"><!ENTITY notnivc "&#x22FD;"><!ENTITY notnivb "&#x22FE;"><!ENTITY barwed "&#x2305;"><!ENTITY barwedge "&#x2305;"><!ENTITY Barwed "&#x2306;"><!ENTITY doublebarwedge "&#x2306;"><!ENTITY lceil "&#x2308;"><!ENTITY LeftCeiling "&#x2308;"><!ENTITY rceil "&#x2309;"><!ENTITY RightCeiling "&#x2309;"><!ENTITY lfloor "&#x230A;"><!ENTITY LeftFloor "&#x230A;"><!ENTITY rfloor "&#x230B;"><!ENTITY RightFloor "&#x230B;"><!ENTITY drcrop "&#x230C;"><!ENTITY dlcrop "&#x230D;"><!ENTITY urcrop "&#x230E;"><!ENTITY ulcrop "&#x230F;"><!ENTITY bnot "&#x2310;"><!ENTITY profline "&#x2312;"><!ENTITY profsurf "&#x2313;"><!ENTITY telrec "&#x2315;"><!ENTITY target "&#x2316;"><!ENTITY ulcorn "&#x231C;"><!ENTITY ulcorner "&#x231C;"><!ENTITY urcorn "&#x231D;"><!ENTITY urcorner "&#x231D;"><!ENTITY dlcorn "&#x231E;"><!ENTITY llcorner "&#x231E;"><!ENTITY drcorn "&#x231F;"><!ENTITY lrcorner "&#x231F;"><!ENTITY frown "&#x2322;"><!ENTITY sfrown "&#x2322;"><!ENTITY smile "&#x2323;"><!ENTITY ssmile "&#x2323;"><!ENTITY cylcty "&#x232D;"><!ENTITY profalar "&#x232E;"><!ENTITY topbot "&#x2336;"><!ENTITY ovbar "&#x233D;"><!ENTITY solbar "&#x233F;"><!ENTITY angzarr "&#x237C;"><!ENTITY lmoust "&#x23B0;"><!ENTITY lmoustache "&#x23B0;"><!ENTITY rmoust "&#x23B1;"><!ENTITY rmoustache "&#x23B1;"><!ENTITY tbrk "&#x23B4;"><!ENTITY OverBracket "&#x23B4;"><!ENTITY bbrk "&#x23B5;"><!ENTITY UnderBracket "&#x23B5;"><!ENTITY bbrktbrk "&#x23B6;"><!ENTITY OverParenthesis "&#x23DC;"><!ENTITY UnderParenthesis "&#x23DD;"><!ENTITY OverBrace "&#x23DE;"><!ENTITY UnderBrace "&#x23DF;"><!ENTITY trpezium "&#x23E2;"><!ENTITY elinters "&#x23E7;"><!ENTITY blank "&#x2423;"><!ENTITY oS "&#x24C8;"><!ENTITY circledS "&#x24C8;"><!ENTITY boxh "&#x2500;"><!ENTITY HorizontalLine "&#x2500;"><!ENTITY boxv "&#x2502;"><!ENTITY boxdr "&#x250C;"><!ENTITY boxdl "&#x2510;"><!ENTITY boxur "&#x2514;"><!ENTITY boxul "&#x2518;"><!ENTITY boxvr "&#x251C;"><!ENTITY boxvl "&#x2524;"><!ENTITY boxhd "&#x252C;"><!ENTITY boxhu "&#x2534;"><!ENTITY boxvh "&#x253C;"><!ENTITY boxH "&#x2550;"><!ENTITY boxV "&#x2551;"><!ENTITY boxdR "&#x2552;"><!ENTITY boxDr "&#x2553;"><!ENTITY boxDR "&#x2554;"><!ENTITY boxdL "&#x2555;"><!ENTITY boxDl "&#x2556;"><!ENTITY boxDL "&#x2557;"><!ENTITY boxuR "&#x2558;"><!ENTITY boxUr "&#x2559;"><!ENTITY boxUR "&#x255A;"><!ENTITY boxuL "&#x255B;"><!ENTITY boxUl "&#x255C;"><!ENTITY boxUL "&#x255D;"><!ENTITY boxvR "&#x255E;"><!ENTITY boxVr "&#x255F;"><!ENTITY boxVR "&#x2560;"><!ENTITY boxvL "&#x2561;"><!ENTITY boxVl "&#x2562;"><!ENTITY boxVL "&#x2563;"><!ENTITY boxHd "&#x2564;"><!ENTITY boxhD "&#x2565;"><!ENTITY boxHD "&#x2566;"><!ENTITY boxHu "&#x2567;"><!ENTITY boxhU "&#x2568;"><!ENTITY boxHU "&#x2569;"><!ENTITY boxvH "&#x256A;"><!ENTITY boxVh "&#x256B;"><!ENTITY boxVH "&#x256C;"><!ENTITY uhblk "&#x2580;"><!ENTITY lhblk "&#x2584;"><!ENTITY block "&#x2588;"><!ENTITY blk14 "&#x2591;"><!ENTITY blk12 "&#x2592;"><!ENTITY blk34 "&#x2593;"><!ENTITY squ "&#x25A1;"><!ENTITY square "&#x25A1;"><!ENTITY Square "&#x25A1;"><!ENTITY squf "&#x25AA;"><!ENTITY squarf "&#x25AA;"><!ENTITY blacksquare "&#x25AA;"><!ENTITY FilledVerySmallSquare "&#x25AA;"><!ENTITY EmptyVerySmallSquare "&#x25AB;"><!ENTITY rect "&#x25AD;"><!ENTITY marker "&#x25AE;"><!ENTITY fltns "&#x25B1;"><!ENTITY xutri "&#x25B3;"><!ENTITY bigtriangleup "&#x25B3;"><!ENTITY utrif "&#x25B4;"><!ENTITY blacktriangle "&#x25B4;"><!ENTITY utri "&#x25B5;"><!ENTITY triangle "&#x25B5;"><!ENTITY rtrif "&#x25B8;"><!ENTITY blacktriangleright "&#x25B8;"><!ENTITY rtri "&#x25B9;"><!ENTITY triangleright "&#x25B9;"><!ENTITY xdtri "&#x25BD;"><!ENTITY bigtriangledown "&#x25BD;"><!ENTITY dtrif "&#x25BE;"><!ENTITY blacktriangledown "&#x25BE;"><!ENTITY dtri "&#x25BF;"><!ENTITY triangledown "&#x25BF;"><!ENTITY ltrif "&#x25C2;"><!ENTITY blacktriangleleft "&#x25C2;"><!ENTITY ltri "&#x25C3;"><!ENTITY triangleleft "&#x25C3;"><!ENTITY loz "&#x25CA;"><!ENTITY lozenge "&#x25CA;"><!ENTITY cir "&#x25CB;"><!ENTITY tridot "&#x25EC;"><!ENTITY xcirc "&#x25EF;"><!ENTITY bigcirc "&#x25EF;"><!ENTITY ultri "&#x25F8;"><!ENTITY urtri "&#x25F9;"><!ENTITY lltri "&#x25FA;"><!ENTITY EmptySmallSquare "&#x25FB;"><!ENTITY FilledSmallSquare "&#x25FC;"><!ENTITY starf "&#x2605;"><!ENTITY bigstar "&#x2605;"><!ENTITY star "&#x2606;"><!ENTITY phone "&#x260E;"><!ENTITY female "&#x2640;"><!ENTITY male "&#x2642;"><!ENTITY spades "&#x2660;"><!ENTITY spadesuit "&#x2660;"><!ENTITY clubs "&#x2663;"><!ENTITY clubsuit "&#x2663;"><!ENTITY hearts "&#x2665;"><!ENTITY heartsuit "&#x2665;"><!ENTITY diams "&#x2666;"><!ENTITY diamondsuit "&#x2666;"><!ENTITY sung "&#x266A;"><!ENTITY flat "&#x266D;"><!ENTITY natur "&#x266E;"><!ENTITY natural "&#x266E;"><!ENTITY sharp "&#x266F;"><!ENTITY check "&#x2713;"><!ENTITY checkmark "&#x2713;"><!ENTITY cross "&#x2717;"><!ENTITY malt "&#x2720;"><!ENTITY maltese "&#x2720;"><!ENTITY sext "&#x2736;"><!ENTITY VerticalSeparator "&#x2758;"><!ENTITY lbbrk "&#x2772;"><!ENTITY rbbrk "&#x2773;"><!ENTITY bsolhsub "&#x27C8;"><!ENTITY suphsol "&#x27C9;"><!ENTITY lobrk "&#x27E6;"><!ENTITY LeftDoubleBracket "&#x27E6;"><!ENTITY robrk "&#x27E7;"><!ENTITY RightDoubleBracket "&#x27E7;"><!ENTITY lang "&#x27E8;"><!ENTITY LeftAngleBracket "&#x27E8;"><!ENTITY langle "&#x27E8;"><!ENTITY rang "&#x27E9;"><!ENTITY RightAngleBracket "&#x27E9;"><!ENTITY rangle "&#x27E9;"><!ENTITY Lang "&#x27EA;"><!ENTITY Rang "&#x27EB;"><!ENTITY loang "&#x27EC;"><!ENTITY roang "&#x27ED;"><!ENTITY xlarr "&#x27F5;"><!ENTITY longleftarrow "&#x27F5;"><!ENTITY LongLeftArrow "&#x27F5;"><!ENTITY xrarr "&#x27F6;"><!ENTITY longrightarrow "&#x27F6;"><!ENTITY LongRightArrow "&#x27F6;"><!ENTITY xharr "&#x27F7;"><!ENTITY longleftrightarrow "&#x27F7;"><!ENTITY LongLeftRightArrow "&#x27F7;"><!ENTITY xlArr "&#x27F8;"><!ENTITY Longleftarrow "&#x27F8;"><!ENTITY DoubleLongLeftArrow "&#x27F8;"><!ENTITY xrArr "&#x27F9;"><!ENTITY Longrightarrow "&#x27F9;"><!ENTITY DoubleLongRightArrow "&#x27F9;"><!ENTITY xhArr "&#x27FA;"><!ENTITY Longleftrightarrow "&#x27FA;"><!ENTITY DoubleLongLeftRightArrow "&#x27FA;"><!ENTITY xmap "&#x27FC;"><!ENTITY longmapsto "&#x27FC;"><!ENTITY dzigrarr "&#x27FF;"><!ENTITY nvlArr "&#x2902;"><!ENTITY nvrArr "&#x2903;"><!ENTITY nvHarr "&#x2904;"><!ENTITY Map "&#x2905;"><!ENTITY lbarr "&#x290C;"><!ENTITY rbarr "&#x290D;"><!ENTITY bkarow "&#x290D;"><!ENTITY lBarr "&#x290E;"><!ENTITY rBarr "&#x290F;"><!ENTITY dbkarow "&#x290F;"><!ENTITY RBarr "&#x2910;"><!ENTITY drbkarow "&#x2910;"><!ENTITY DDotrahd "&#x2911;"><!ENTITY UpArrowBar "&#x2912;"><!ENTITY DownArrowBar "&#x2913;"><!ENTITY Rarrtl "&#x2916;"><!ENTITY latail "&#x2919;"><!ENTITY ratail "&#x291A;"><!ENTITY lAtail "&#x291B;"><!ENTITY rAtail "&#x291C;"><!ENTITY larrfs "&#x291D;"><!ENTITY rarrfs "&#x291E;"><!ENTITY larrbfs "&#x291F;"><!ENTITY rarrbfs "&#x2920;"><!ENTITY nwarhk "&#x2923;"><!ENTITY nearhk "&#x2924;"><!ENTITY searhk "&#x2925;"><!ENTITY hksearow "&#x2925;"><!ENTITY swarhk "&#x2926;"><!ENTITY hkswarow "&#x2926;"><!ENTITY nwnear "&#x2927;"><!ENTITY nesear "&#x2928;"><!ENTITY toea "&#x2928;"><!ENTITY seswar "&#x2929;"><!ENTITY tosa "&#x2929;"><!ENTITY swnwar "&#x292A;"><!ENTITY rarrc "&#x2933;"><!ENTITY nrarrc "&#x2933;&#x338;"><!ENTITY cudarrr "&#x2935;"><!ENTITY ldca "&#x2936;"><!ENTITY rdca "&#x2937;"><!ENTITY cudarrl "&#x2938;"><!ENTITY larrpl "&#x2939;"><!ENTITY curarrm "&#x293C;"><!ENTITY cularrp "&#x293D;"><!ENTITY rarrpl "&#x2945;"><!ENTITY harrcir "&#x2948;"><!ENTITY Uarrocir "&#x2949;"><!ENTITY lurdshar "&#x294A;"><!ENTITY ldrushar "&#x294B;"><!ENTITY LeftRightVector "&#x294E;"><!ENTITY RightUpDownVector "&#x294F;"><!ENTITY DownLeftRightVector "&#x2950;"><!ENTITY LeftUpDownVector "&#x2951;"><!ENTITY LeftVectorBar "&#x2952;"><!ENTITY RightVectorBar "&#x2953;"><!ENTITY RightUpVectorBar "&#x2954;"><!ENTITY RightDownVectorBar "&#x2955;"><!ENTITY DownLeftVectorBar "&#x2956;"><!ENTITY DownRightVectorBar "&#x2957;"><!ENTITY LeftUpVectorBar "&#x2958;"><!ENTITY LeftDownVectorBar "&#x2959;"><!ENTITY LeftTeeVector "&#x295A;"><!ENTITY RightTeeVector "&#x295B;"><!ENTITY RightUpTeeVector "&#x295C;"><!ENTITY RightDownTeeVector "&#x295D;"><!ENTITY DownLeftTeeVector "&#x295E;"><!ENTITY DownRightTeeVector "&#x295F;"><!ENTITY LeftUpTeeVector "&#x2960;"><!ENTITY LeftDownTeeVector "&#x2961;"><!ENTITY lHar "&#x2962;"><!ENTITY uHar "&#x2963;"><!ENTITY rHar "&#x2964;"><!ENTITY dHar "&#x2965;"><!ENTITY luruhar "&#x2966;"><!ENTITY ldrdhar "&#x2967;"><!ENTITY ruluhar "&#x2968;"><!ENTITY rdldhar "&#x2969;"><!ENTITY lharul "&#x296A;"><!ENTITY llhard "&#x296B;"><!ENTITY rharul "&#x296C;"><!ENTITY lrhard "&#x296D;"><!ENTITY udhar "&#x296E;"><!ENTITY UpEquilibrium "&#x296E;"><!ENTITY duhar "&#x296F;"><!ENTITY ReverseUpEquilibrium "&#x296F;"><!ENTITY RoundImplies "&#x2970;"><!ENTITY erarr "&#x2971;"><!ENTITY simrarr "&#x2972;"><!ENTITY larrsim "&#x2973;"><!ENTITY rarrsim "&#x2974;"><!ENTITY rarrap "&#x2975;"><!ENTITY ltlarr "&#x2976;"><!ENTITY gtrarr "&#x2978;"><!ENTITY subrarr "&#x2979;"><!ENTITY suplarr "&#x297B;"><!ENTITY lfisht "&#x297C;"><!ENTITY rfisht "&#x297D;"><!ENTITY ufisht "&#x297E;"><!ENTITY dfisht "&#x297F;"><!ENTITY lopar "&#x2985;"><!ENTITY ropar "&#x2986;"><!ENTITY lbrke "&#x298B;"><!ENTITY rbrke "&#x298C;"><!ENTITY lbrkslu "&#x298D;"><!ENTITY rbrksld "&#x298E;"><!ENTITY lbrksld "&#x298F;"><!ENTITY rbrkslu "&#x2990;"><!ENTITY langd "&#x2991;"><!ENTITY rangd "&#x2992;"><!ENTITY lparlt "&#x2993;"><!ENTITY rpargt "&#x2994;"><!ENTITY gtlPar "&#x2995;"><!ENTITY ltrPar "&#x2996;"><!ENTITY vzigzag "&#x299A;"><!ENTITY vangrt "&#x299C;"><!ENTITY angrtvbd "&#x299D;"><!ENTITY ange "&#x29A4;"><!ENTITY range "&#x29A5;"><!ENTITY dwangle "&#x29A6;"><!ENTITY uwangle "&#x29A7;"><!ENTITY angmsdaa "&#x29A8;"><!ENTITY angmsdab "&#x29A9;"><!ENTITY angmsdac "&#x29AA;"><!ENTITY angmsdad "&#x29AB;"><!ENTITY angmsdae "&#x29AC;"><!ENTITY angmsdaf "&#x29AD;"><!ENTITY angmsdag "&#x29AE;"><!ENTITY angmsdah "&#x29AF;"><!ENTITY bemptyv "&#x29B0;"><!ENTITY demptyv "&#x29B1;"><!ENTITY cemptyv "&#x29B2;"><!ENTITY raemptyv "&#x29B3;"><!ENTITY laemptyv "&#x29B4;"><!ENTITY ohbar "&#x29B5;"><!ENTITY omid "&#x29B6;"><!ENTITY opar "&#x29B7;"><!ENTITY operp "&#x29B9;"><!ENTITY olcross "&#x29BB;"><!ENTITY odsold "&#x29BC;"><!ENTITY olcir "&#x29BE;"><!ENTITY ofcir "&#x29BF;"><!ENTITY olt "&#x29C0;"><!ENTITY ogt "&#x29C1;"><!ENTITY cirscir "&#x29C2;"><!ENTITY cirE "&#x29C3;"><!ENTITY solb "&#x29C4;"><!ENTITY bsolb "&#x29C5;"><!ENTITY boxbox "&#x29C9;"><!ENTITY trisb "&#x29CD;"><!ENTITY rtriltri "&#x29CE;"><!ENTITY LeftTriangleBar "&#x29CF;"><!ENTITY NotLeftTriangleBar "&#x29CF;&#x338;"><!ENTITY RightTriangleBar "&#x29D0;"><!ENTITY NotRightTriangleBar "&#x29D0;&#x338;"><!ENTITY iinfin "&#x29DC;"><!ENTITY infintie "&#x29DD;"><!ENTITY nvinfin "&#x29DE;"><!ENTITY eparsl "&#x29E3;"><!ENTITY smeparsl "&#x29E4;"><!ENTITY eqvparsl "&#x29E5;"><!ENTITY lozf "&#x29EB;"><!ENTITY blacklozenge "&#x29EB;"><!ENTITY RuleDelayed "&#x29F4;"><!ENTITY dsol "&#x29F6;"><!ENTITY xodot "&#x2A00;"><!ENTITY bigodot "&#x2A00;"><!ENTITY xoplus "&#x2A01;"><!ENTITY bigoplus "&#x2A01;"><!ENTITY xotime "&#x2A02;"><!ENTITY bigotimes "&#x2A02;"><!ENTITY xuplus "&#x2A04;"><!ENTITY biguplus "&#x2A04;"><!ENTITY xsqcup "&#x2A06;"><!ENTITY bigsqcup "&#x2A06;"><!ENTITY qint "&#x2A0C;"><!ENTITY iiiint "&#x2A0C;"><!ENTITY fpartint "&#x2A0D;"><!ENTITY cirfnint "&#x2A10;"><!ENTITY awint "&#x2A11;"><!ENTITY rppolint "&#x2A12;"><!ENTITY scpolint "&#x2A13;"><!ENTITY npolint "&#x2A14;"><!ENTITY pointint "&#x2A15;"><!ENTITY quatint "&#x2A16;"><!ENTITY intlarhk "&#x2A17;"><!ENTITY pluscir "&#x2A22;"><!ENTITY plusacir "&#x2A23;"><!ENTITY simplus "&#x2A24;"><!ENTITY plusdu "&#x2A25;"><!ENTITY plussim "&#x2A26;"><!ENTITY plustwo "&#x2A27;"><!ENTITY mcomma "&#x2A29;"><!ENTITY minusdu "&#x2A2A;"><!ENTITY loplus "&#x2A2D;"><!ENTITY roplus "&#x2A2E;"><!ENTITY Cross "&#x2A2F;"><!ENTITY timesd "&#x2A30;"><!ENTITY timesbar "&#x2A31;"><!ENTITY smashp "&#x2A33;"><!ENTITY lotimes "&#x2A34;"><!ENTITY rotimes "&#x2A35;"><!ENTITY otimesas "&#x2A36;"><!ENTITY Otimes "&#x2A37;"><!ENTITY odiv "&#x2A38;"><!ENTITY triplus "&#x2A39;"><!ENTITY triminus "&#x2A3A;"><!ENTITY tritime "&#x2A3B;"><!ENTITY iprod "&#x2A3C;"><!ENTITY intprod "&#x2A3C;"><!ENTITY amalg "&#x2A3F;"><!ENTITY capdot "&#x2A40;"><!ENTITY ncup "&#x2A42;"><!ENTITY ncap "&#x2A43;"><!ENTITY capand "&#x2A44;"><!ENTITY cupor "&#x2A45;"><!ENTITY cupcap "&#x2A46;"><!ENTITY capcup "&#x2A47;"><!ENTITY cupbrcap "&#x2A48;"><!ENTITY capbrcup "&#x2A49;"><!ENTITY cupcup "&#x2A4A;"><!ENTITY capcap "&#x2A4B;"><!ENTITY ccups "&#x2A4C;"><!ENTITY ccaps "&#x2A4D;"><!ENTITY ccupssm "&#x2A50;"><!ENTITY And "&#x2A53;"><!ENTITY Or "&#x2A54;"><!ENTITY andand "&#x2A55;"><!ENTITY oror "&#x2A56;"><!ENTITY orslope "&#x2A57;"><!ENTITY andslope "&#x2A58;"><!ENTITY andv "&#x2A5A;"><!ENTITY orv "&#x2A5B;"><!ENTITY andd "&#x2A5C;"><!ENTITY ord "&#x2A5D;"><!ENTITY wedbar "&#x2A5F;"><!ENTITY sdote "&#x2A66;"><!ENTITY simdot "&#x2A6A;"><!ENTITY congdot "&#x2A6D;"><!ENTITY ncongdot "&#x2A6D;&#x338;"><!ENTITY easter "&#x2A6E;"><!ENTITY apacir "&#x2A6F;"><!ENTITY apE "&#x2A70;"><!ENTITY napE "&#x2A70;&#x338;"><!ENTITY eplus "&#x2A71;"><!ENTITY pluse "&#x2A72;"><!ENTITY Esim "&#x2A73;"><!ENTITY Colone "&#x2A74;"><!ENTITY Equal "&#x2A75;"><!ENTITY eDDot "&#x2A77;"><!ENTITY ddotseq "&#x2A77;"><!ENTITY equivDD "&#x2A78;"><!ENTITY ltcir "&#x2A79;"><!ENTITY gtcir "&#x2A7A;"><!ENTITY ltquest "&#x2A7B;"><!ENTITY gtquest "&#x2A7C;"><!ENTITY les "&#x2A7D;"><!ENTITY LessSlantEqual "&#x2A7D;"><!ENTITY leqslant "&#x2A7D;"><!ENTITY nles "&#x2A7D;&#x338;"><!ENTITY NotLessSlantEqual "&#x2A7D;&#x338;"><!ENTITY nleqslant "&#x2A7D;&#x338;"><!ENTITY ges "&#x2A7E;"><!ENTITY GreaterSlantEqual "&#x2A7E;"><!ENTITY geqslant "&#x2A7E;"><!ENTITY nges "&#x2A7E;&#x338;"><!ENTITY NotGreaterSlantEqual "&#x2A7E;&#x338;"><!ENTITY ngeqslant "&#x2A7E;&#x338;"><!ENTITY lesdot "&#x2A7F;"><!ENTITY gesdot "&#x2A80;"><!ENTITY lesdoto "&#x2A81;"><!ENTITY gesdoto "&#x2A82;"><!ENTITY lesdotor "&#x2A83;"><!ENTITY gesdotol "&#x2A84;"><!ENTITY lap "&#x2A85;"><!ENTITY lessapprox "&#x2A85;"><!ENTITY gap "&#x2A86;"><!ENTITY gtrapprox "&#x2A86;"><!ENTITY lne "&#x2A87;"><!ENTITY lneq "&#x2A87;"><!ENTITY gne "&#x2A88;"><!ENTITY gneq "&#x2A88;"><!ENTITY lnap "&#x2A89;"><!ENTITY lnapprox "&#x2A89;"><!ENTITY gnap "&#x2A8A;"><!ENTITY gnapprox "&#x2A8A;"><!ENTITY lEg "&#x2A8B;"><!ENTITY lesseqqgtr "&#x2A8B;"><!ENTITY gEl "&#x2A8C;"><!ENTITY gtreqqless "&#x2A8C;"><!ENTITY lsime "&#x2A8D;"><!ENTITY gsime "&#x2A8E;"><!ENTITY lsimg "&#x2A8F;"><!ENTITY gsiml "&#x2A90;"><!ENTITY lgE "&#x2A91;"><!ENTITY glE "&#x2A92;"><!ENTITY lesges "&#x2A93;"><!ENTITY gesles "&#x2A94;"><!ENTITY els "&#x2A95;"><!ENTITY eqslantless "&#x2A95;"><!ENTITY egs "&#x2A96;"><!ENTITY eqslantgtr "&#x2A96;"><!ENTITY elsdot "&#x2A97;"><!ENTITY egsdot "&#x2A98;"><!ENTITY el "&#x2A99;"><!ENTITY eg "&#x2A9A;"><!ENTITY siml "&#x2A9D;"><!ENTITY simg "&#x2A9E;"><!ENTITY simlE "&#x2A9F;"><!ENTITY simgE "&#x2AA0;"><!ENTITY LessLess "&#x2AA1;"><!ENTITY NotNestedLessLess "&#x2AA1;&#x338;"><!ENTITY GreaterGreater "&#x2AA2;"><!ENTITY NotNestedGreaterGreater "&#x2AA2;&#x338;"><!ENTITY glj "&#x2AA4;"><!ENTITY gla "&#x2AA5;"><!ENTITY ltcc "&#x2AA6;"><!ENTITY gtcc "&#x2AA7;"><!ENTITY lescc "&#x2AA8;"><!ENTITY gescc "&#x2AA9;"><!ENTITY smt "&#x2AAA;"><!ENTITY lat "&#x2AAB;"><!ENTITY smte "&#x2AAC;"><!ENTITY smtes "&#x2AAC;&#xFE00;"><!ENTITY late "&#x2AAD;"><!ENTITY lates "&#x2AAD;&#xFE00;"><!ENTITY bumpE "&#x2AAE;"><!ENTITY pre "&#x2AAF;"><!ENTITY preceq "&#x2AAF;"><!ENTITY PrecedesEqual "&#x2AAF;"><!ENTITY npre "&#x2AAF;&#x338;"><!ENTITY npreceq "&#x2AAF;&#x338;"><!ENTITY NotPrecedesEqual "&#x2AAF;&#x338;"><!ENTITY sce "&#x2AB0;"><!ENTITY succeq "&#x2AB0;"><!ENTITY SucceedsEqual "&#x2AB0;"><!ENTITY nsce "&#x2AB0;&#x338;"><!ENTITY nsucceq "&#x2AB0;&#x338;"><!ENTITY NotSucceedsEqual "&#x2AB0;&#x338;"><!ENTITY prE "&#x2AB3;"><!ENTITY scE "&#x2AB4;"><!ENTITY prnE "&#x2AB5;"><!ENTITY precneqq "&#x2AB5;"><!ENTITY scnE "&#x2AB6;"><!ENTITY succneqq "&#x2AB6;"><!ENTITY prap "&#x2AB7;"><!ENTITY precapprox "&#x2AB7;"><!ENTITY scap "&#x2AB8;"><!ENTITY succapprox "&#x2AB8;"><!ENTITY prnap "&#x2AB9;"><!ENTITY precnapprox "&#x2AB9;"><!ENTITY scnap "&#x2ABA;"><!ENTITY succnapprox "&#x2ABA;"><!ENTITY Pr "&#x2ABB;"><!ENTITY Sc "&#x2ABC;"><!ENTITY subdot "&#x2ABD;"><!ENTITY supdot "&#x2ABE;"><!ENTITY subplus "&#x2ABF;"><!ENTITY supplus "&#x2AC0;"><!ENTITY submult "&#x2AC1;"><!ENTITY supmult "&#x2AC2;"><!ENTITY subedot "&#x2AC3;"><!ENTITY supedot "&#x2AC4;"><!ENTITY subE "&#x2AC5;"><!ENTITY subseteqq "&#x2AC5;"><!ENTITY nsubE "&#x2AC5;&#x338;"><!ENTITY nsubseteqq "&#x2AC5;&#x338;"><!ENTITY supE "&#x2AC6;"><!ENTITY supseteqq "&#x2AC6;"><!ENTITY nsupE "&#x2AC6;&#x338;"><!ENTITY nsupseteqq "&#x2AC6;&#x338;"><!ENTITY subsim "&#x2AC7;"><!ENTITY supsim "&#x2AC8;"><!ENTITY subnE "&#x2ACB;"><!ENTITY subsetneqq "&#x2ACB;"><!ENTITY vsubnE "&#x2ACB;&#xFE00;"><!ENTITY varsubsetneqq "&#x2ACB;&#xFE00;"><!ENTITY supnE "&#x2ACC;"><!ENTITY supsetneqq "&#x2ACC;"><!ENTITY vsupnE "&#x2ACC;&#xFE00;"><!ENTITY varsupsetneqq "&#x2ACC;&#xFE00;"><!ENTITY csub "&#x2ACF;"><!ENTITY csup "&#x2AD0;"><!ENTITY csube "&#x2AD1;"><!ENTITY csupe "&#x2AD2;"><!ENTITY subsup "&#x2AD3;"><!ENTITY supsub "&#x2AD4;"><!ENTITY subsub "&#x2AD5;"><!ENTITY supsup "&#x2AD6;"><!ENTITY suphsub "&#x2AD7;"><!ENTITY supdsub "&#x2AD8;"><!ENTITY forkv "&#x2AD9;"><!ENTITY topfork "&#x2ADA;"><!ENTITY mlcp "&#x2ADB;"><!ENTITY Dashv "&#x2AE4;"><!ENTITY DoubleLeftTee "&#x2AE4;"><!ENTITY Vdashl "&#x2AE6;"><!ENTITY Barv "&#x2AE7;"><!ENTITY vBar "&#x2AE8;"><!ENTITY vBarv "&#x2AE9;"><!ENTITY Vbar "&#x2AEB;"><!ENTITY Not "&#x2AEC;"><!ENTITY bNot "&#x2AED;"><!ENTITY rnmid "&#x2AEE;"><!ENTITY cirmid "&#x2AEF;"><!ENTITY midcir "&#x2AF0;"><!ENTITY topcir "&#x2AF1;"><!ENTITY nhpar "&#x2AF2;"><!ENTITY parsim "&#x2AF3;"><!ENTITY parsl "&#x2AFD;"><!ENTITY nparsl "&#x2AFD;&#x20E5;"><!ENTITY fflig "&#xFB00;"><!ENTITY filig "&#xFB01;"><!ENTITY fllig "&#xFB02;"><!ENTITY ffilig "&#xFB03;"><!ENTITY ffllig "&#xFB04;"><!ENTITY Ascr "&#x1D49C;"><!ENTITY Cscr "&#x1D49E;"><!ENTITY Dscr "&#x1D49F;"><!ENTITY Gscr "&#x1D4A2;"><!ENTITY Jscr "&#x1D4A5;"><!ENTITY Kscr "&#x1D4A6;"><!ENTITY Nscr "&#x1D4A9;"><!ENTITY Oscr "&#x1D4AA;"><!ENTITY Pscr "&#x1D4AB;"><!ENTITY Qscr "&#x1D4AC;"><!ENTITY Sscr "&#x1D4AE;"><!ENTITY Tscr "&#x1D4AF;"><!ENTITY Uscr "&#x1D4B0;"><!ENTITY Vscr "&#x1D4B1;"><!ENTITY Wscr "&#x1D4B2;"><!ENTITY Xscr "&#x1D4B3;"><!ENTITY Yscr "&#x1D4B4;"><!ENTITY Zscr "&#x1D4B5;"><!ENTITY ascr "&#x1D4B6;"><!ENTITY bscr "&#x1D4B7;"><!ENTITY cscr "&#x1D4B8;"><!ENTITY dscr "&#x1D4B9;"><!ENTITY fscr "&#x1D4BB;"><!ENTITY hscr "&#x1D4BD;"><!ENTITY iscr "&#x1D4BE;"><!ENTITY jscr "&#x1D4BF;"><!ENTITY kscr "&#x1D4C0;"><!ENTITY lscr "&#x1D4C1;"><!ENTITY mscr "&#x1D4C2;"><!ENTITY nscr "&#x1D4C3;"><!ENTITY pscr "&#x1D4C5;"><!ENTITY qscr "&#x1D4C6;"><!ENTITY rscr "&#x1D4C7;"><!ENTITY sscr "&#x1D4C8;"><!ENTITY tscr "&#x1D4C9;"><!ENTITY uscr "&#x1D4CA;"><!ENTITY vscr "&#x1D4CB;"><!ENTITY wscr "&#x1D4CC;"><!ENTITY xscr "&#x1D4CD;"><!ENTITY yscr "&#x1D4CE;"><!ENTITY zscr "&#x1D4CF;"><!ENTITY Afr "&#x1D504;"><!ENTITY Bfr "&#x1D505;"><!ENTITY Dfr "&#x1D507;"><!ENTITY Efr "&#x1D508;"><!ENTITY Ffr "&#x1D509;"><!ENTITY Gfr "&#x1D50A;"><!ENTITY Jfr "&#x1D50D;"><!ENTITY Kfr "&#x1D50E;"><!ENTITY Lfr "&#x1D50F;"><!ENTITY Mfr "&#x1D510;"><!ENTITY Nfr "&#x1D511;"><!ENTITY Ofr "&#x1D512;"><!ENTITY Pfr "&#x1D513;"><!ENTITY Qfr "&#x1D514;"><!ENTITY Sfr "&#x1D516;"><!ENTITY Tfr "&#x1D517;"><!ENTITY Ufr "&#x1D518;"><!ENTITY Vfr "&#x1D519;"><!ENTITY Wfr "&#x1D51A;"><!ENTITY Xfr "&#x1D51B;"><!ENTITY Yfr "&#x1D51C;"><!ENTITY afr "&#x1D51E;"><!ENTITY bfr "&#x1D51F;"><!ENTITY cfr "&#x1D520;"><!ENTITY dfr "&#x1D521;"><!ENTITY efr "&#x1D522;"><!ENTITY ffr "&#x1D523;"><!ENTITY gfr "&#x1D524;"><!ENTITY hfr "&#x1D525;"><!ENTITY ifr "&#x1D526;"><!ENTITY jfr "&#x1D527;"><!ENTITY kfr "&#x1D528;"><!ENTITY lfr "&#x1D529;"><!ENTITY mfr "&#x1D52A;"><!ENTITY nfr "&#x1D52B;"><!ENTITY ofr "&#x1D52C;"><!ENTITY pfr "&#x1D52D;"><!ENTITY qfr "&#x1D52E;"><!ENTITY rfr "&#x1D52F;"><!ENTITY sfr "&#x1D530;"><!ENTITY tfr "&#x1D531;"><!ENTITY ufr "&#x1D532;"><!ENTITY vfr "&#x1D533;"><!ENTITY wfr "&#x1D534;"><!ENTITY xfr "&#x1D535;"><!ENTITY yfr "&#x1D536;"><!ENTITY zfr "&#x1D537;"><!ENTITY Aopf "&#x1D538;"><!ENTITY Bopf "&#x1D539;"><!ENTITY Dopf "&#x1D53B;"><!ENTITY Eopf "&#x1D53C;"><!ENTITY Fopf "&#x1D53D;"><!ENTITY Gopf "&#x1D53E;"><!ENTITY Iopf "&#x1D540;"><!ENTITY Jopf "&#x1D541;"><!ENTITY Kopf "&#x1D542;"><!ENTITY Lopf "&#x1D543;"><!ENTITY Mopf "&#x1D544;"><!ENTITY Oopf "&#x1D546;"><!ENTITY Sopf "&#x1D54A;"><!ENTITY Topf "&#x1D54B;"><!ENTITY Uopf "&#x1D54C;"><!ENTITY Vopf "&#x1D54D;"><!ENTITY Wopf "&#x1D54E;"><!ENTITY Xopf "&#x1D54F;"><!ENTITY Yopf "&#x1D550;"><!ENTITY aopf "&#x1D552;"><!ENTITY bopf "&#x1D553;"><!ENTITY copf "&#x1D554;"><!ENTITY dopf "&#x1D555;"><!ENTITY eopf "&#x1D556;"><!ENTITY fopf "&#x1D557;"><!ENTITY gopf "&#x1D558;"><!ENTITY hopf "&#x1D559;"><!ENTITY iopf "&#x1D55A;"><!ENTITY jopf "&#x1D55B;"><!ENTITY kopf "&#x1D55C;"><!ENTITY lopf "&#x1D55D;"><!ENTITY mopf "&#x1D55E;"><!ENTITY nopf "&#x1D55F;"><!ENTITY oopf "&#x1D560;"><!ENTITY popf "&#x1D561;"><!ENTITY qopf "&#x1D562;"><!ENTITY ropf "&#x1D563;"><!ENTITY sopf "&#x1D564;"><!ENTITY topf "&#x1D565;"><!ENTITY uopf "&#x1D566;"><!ENTITY vopf "&#x1D567;"><!ENTITY wopf "&#x1D568;"><!ENTITY xopf "&#x1D569;"><!ENTITY yopf "&#x1D56A;"><!ENTITY zopf "&#x1D56B;">
)xmlxmlxml"sv;
}
