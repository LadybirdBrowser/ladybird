/*
 * Copyright (c) 2020-2024, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/SourceLocation.h>
#include <AK/Utf32View.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MathML/TagNames.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWeb/SVG/TagNames.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLParser);

static inline void log_parse_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(HTML_PARSER_DEBUG, "Parse error! {}", location);
}

static Vector<StringView> const s_quirks_public_ids = {
    "+//Silmaril//dtd html Pro v0r11 19970101//"sv,
    "-//AS//DTD HTML 3.0 asWedit + extensions//"sv,
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//"sv,
    "-//IETF//DTD HTML 2.0 Level 1//"sv,
    "-//IETF//DTD HTML 2.0 Level 2//"sv,
    "-//IETF//DTD HTML 2.0 Strict Level 1//"sv,
    "-//IETF//DTD HTML 2.0 Strict Level 2//"sv,
    "-//IETF//DTD HTML 2.0 Strict//"sv,
    "-//IETF//DTD HTML 2.0//"sv,
    "-//IETF//DTD HTML 2.1E//"sv,
    "-//IETF//DTD HTML 3.0//"sv,
    "-//IETF//DTD HTML 3.2 Final//"sv,
    "-//IETF//DTD HTML 3.2//"sv,
    "-//IETF//DTD HTML 3//"sv,
    "-//IETF//DTD HTML Level 0//"sv,
    "-//IETF//DTD HTML Level 1//"sv,
    "-//IETF//DTD HTML Level 2//"sv,
    "-//IETF//DTD HTML Level 3//"sv,
    "-//IETF//DTD HTML Strict Level 0//"sv,
    "-//IETF//DTD HTML Strict Level 1//"sv,
    "-//IETF//DTD HTML Strict Level 2//"sv,
    "-//IETF//DTD HTML Strict Level 3//"sv,
    "-//IETF//DTD HTML Strict//"sv,
    "-//IETF//DTD HTML//"sv,
    "-//Metrius//DTD Metrius Presentational//"sv,
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//"sv,
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//"sv,
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//"sv,
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//"sv,
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//"sv,
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//"sv,
    "-//Netscape Comm. Corp.//DTD HTML//"sv,
    "-//Netscape Comm. Corp.//DTD Strict HTML//"sv,
    "-//O'Reilly and Associates//DTD HTML 2.0//"sv,
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//"sv,
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//"sv,
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//"sv,
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//"sv,
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//"sv,
    "-//Spyglass//DTD HTML 2.0 Extended//"sv,
    "-//Sun Microsystems Corp.//DTD HotJava HTML//"sv,
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//"sv,
    "-//W3C//DTD HTML 3 1995-03-24//"sv,
    "-//W3C//DTD HTML 3.2 Draft//"sv,
    "-//W3C//DTD HTML 3.2 Final//"sv,
    "-//W3C//DTD HTML 3.2//"sv,
    "-//W3C//DTD HTML 3.2S Draft//"sv,
    "-//W3C//DTD HTML 4.0 Frameset//"sv,
    "-//W3C//DTD HTML 4.0 Transitional//"sv,
    "-//W3C//DTD HTML Experimental 19960712//"sv,
    "-//W3C//DTD HTML Experimental 970421//"sv,
    "-//W3C//DTD W3 HTML//"sv,
    "-//W3O//DTD W3 HTML 3.0//"sv,
    "-//WebTechs//DTD Mozilla HTML 2.0//"sv,
    "-//WebTechs//DTD Mozilla HTML//"sv,
};

// https://html.spec.whatwg.org/multipage/parsing.html#mathml-text-integration-point
static bool is_mathml_text_integration_point(DOM::Element const&)
{
    // FIXME: Implement.
    return false;
}

// https://html.spec.whatwg.org/multipage/parsing.html#html-integration-point
static bool is_html_integration_point(DOM::Element const& element)
{
    // A node is an HTML integration point if it is one of the following elements:
    // FIXME: A MathML annotation-xml element whose start tag token had an attribute with the name "encoding" whose value was an ASCII case-insensitive match for the string "text/html"
    // FIXME: A MathML annotation-xml element whose start tag token had an attribute with the name "encoding" whose value was an ASCII case-insensitive match for the string "application/xhtml+xml"

    // An SVG foreignObject element
    // An SVG desc element
    // An SVG title element
    if (element.tag_name().is_one_of(SVG::TagNames::foreignObject, SVG::TagNames::desc, SVG::TagNames::title))
        return true;

    return false;
}

HTMLParser::HTMLParser(DOM::Document& document, StringView input, StringView encoding)
    : m_tokenizer(input, encoding)
    , m_scripting_enabled(document.is_scripting_enabled())
    , m_document(JS::make_handle(document))
{
    m_tokenizer.set_parser({}, *this);
    m_document->set_parser({}, *this);
    auto standardized_encoding = TextCodec::get_standardized_encoding(encoding);
    VERIFY(standardized_encoding.has_value());
    m_document->set_encoding(MUST(String::from_utf8(standardized_encoding.value())));
}

HTMLParser::HTMLParser(DOM::Document& document)
    : m_scripting_enabled(document.is_scripting_enabled())
    , m_document(JS::make_handle(document))
{
    m_document->set_parser({}, *this);
    m_tokenizer.set_parser({}, *this);
}

HTMLParser::~HTMLParser()
{
}

void HTMLParser::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_head_element);
    visitor.visit(m_form_element);
    visitor.visit(m_context_element);
    visitor.visit(m_character_insertion_node);

    m_stack_of_open_elements.visit_edges(visitor);
    m_list_of_active_formatting_elements.visit_edges(visitor);
}

void HTMLParser::run(HTMLTokenizer::StopAtInsertionPoint stop_at_insertion_point)
{
    for (;;) {
        // FIXME: Find a better way to say that we come from Document::close() and want to process EOF.
        if (!m_tokenizer.is_eof_inserted() && m_tokenizer.is_insertion_point_reached())
            return;

        auto optional_token = m_tokenizer.next_token(stop_at_insertion_point);
        if (!optional_token.has_value())
            break;
        auto& token = optional_token.value();

        dbgln_if(HTML_PARSER_DEBUG, "[{}] {}", insertion_mode_name(), token.to_string());

        // https://html.spec.whatwg.org/multipage/parsing.html#tree-construction-dispatcher
        // As each token is emitted from the tokenizer, the user agent must follow the appropriate steps from the following list, known as the tree construction dispatcher:
        if (m_stack_of_open_elements.is_empty()
            || adjusted_current_node().namespace_uri() == Namespace::HTML
            || (is_html_integration_point(adjusted_current_node()) && (token.is_start_tag() || token.is_character()))
            || token.is_end_of_file()) {
            // -> If the stack of open elements is empty
            // -> If the adjusted current node is an element in the HTML namespace
            // FIXME: -> If the adjusted current node is a MathML text integration point and the token is a start tag whose tag name is neither "mglyph" nor "malignmark"
            // FIXME: -> If the adjusted current node is a MathML text integration point and the token is a character token
            // FIXME: -> If the adjusted current node is a MathML annotation-xml element and the token is a start tag whose tag name is "svg"
            // -> If the adjusted current node is an HTML integration point and the token is a start tag
            // -> If the adjusted current node is an HTML integration point and the token is a character token
            // -> If the token is an end-of-file token

            // Process the token according to the rules given in the section corresponding to the current insertion mode in HTML content.
            process_using_the_rules_for(m_insertion_mode, token);
        } else {
            // -> Otherwise

            // Process the token according to the rules given in the section for parsing tokens in foreign content.
            process_using_the_rules_for_foreign_content(token);
        }

        if (m_stop_parsing) {
            dbgln_if(HTML_PARSER_DEBUG, "Stop parsing{}! :^)", m_parsing_fragment ? " fragment" : "");
            break;
        }
    }

    flush_character_insertions();
}

void HTMLParser::run(const URL::URL& url, HTMLTokenizer::StopAtInsertionPoint stop_at_insertion_point)
{
    m_document->set_url(url);
    m_document->set_source(MUST(String::from_byte_string(m_tokenizer.source())));
    run(stop_at_insertion_point);
    the_end(*m_document, this);
    m_document->detach_parser({});
}

// https://html.spec.whatwg.org/multipage/parsing.html#the-end
void HTMLParser::the_end(JS::NonnullGCPtr<DOM::Document> document, JS::GCPtr<HTMLParser> parser)
{
    // Once the user agent stops parsing the document, the user agent must run the following steps:

    // NOTE: This is a static method because the spec sometimes wants us to "act as if the user agent had stopped
    //       parsing document" which means running these steps without an HTML Parser. That makes it awkward to call,
    //       but it's preferable to duplicating so much code.

    if (parser)
        VERIFY(document == parser->m_document);

    // The entirety of "the end" should be a no-op for HTML fragment parsers, because:
    // - the temporary document is not accessible, making the DOMContentLoaded event and "ready for post load tasks" do
    //   nothing, making the parser not re-entrant from document.{open,write,close} and document.readyState inaccessible
    // - there is no Window associated with it and no associated browsing context with the temporary document (meaning
    //   the Window load event is skipped and making the load timing info inaccessible)
    // - scripts are not able to be prepared, meaning the script queues are empty.
    // However, the unconditional "spin the event loop" invocations cause two issues:
    // - Microtask timing is changed, as "spin the event loop" performs an unconditional microtask checkpoint, causing
    //   things to happen out of order. For example, YouTube sets the innerHTML of a <template> element in the constructor
    //   of the ytd-app custom element _before_ setting up class attributes. Since custom elements use microtasks to run
    //   callbacks, this causes custom element callbacks that rely on attributes setup by the constructor to run before
    //   the attributes are set up, causing unhandled exceptions.
    // - Load event delaying can spin forever, e.g. if the fragment contains an <img> element which stops delaying the
    //   load event from an element task. Since tasks are not considered runnable if they're from a document with no
    //   browsing context (i.e. the temporary document made for innerHTML), the <img> element will forever delay the load
    //   event and cause an infinite loop.
    // We can avoid these issues and also avoid doing unnecessary work by simply skipping "the end" for HTML fragment
    // parsers.
    // See the message of the commit that added this for more details.
    if (parser && parser->m_parsing_fragment)
        return;

    // FIXME: 1. If the active speculative HTML parser is not null, then stop the speculative HTML parser and return.

    // 2. Set the insertion point to undefined.
    if (parser)
        parser->m_tokenizer.undefine_insertion_point();

    // 3. Update the current document readiness to "interactive".
    document->update_readiness(HTML::DocumentReadyState::Interactive);

    // 4. Pop all the nodes off the stack of open elements.
    if (parser) {
        while (!parser->m_stack_of_open_elements.is_empty())
            (void)parser->m_stack_of_open_elements.pop();
    }

    // 5. While the list of scripts that will execute when the document has finished parsing is not empty:
    while (!document->scripts_to_execute_when_parsing_has_finished().is_empty()) {
        // 1. Spin the event loop until the first script in the list of scripts that will execute when the document has finished parsing
        //    has its "ready to be parser-executed" flag set and the parser's Document has no style sheet that is blocking scripts.
        main_thread_event_loop().spin_until([&] {
            return document->scripts_to_execute_when_parsing_has_finished().first()->is_ready_to_be_parser_executed()
                && !document->has_a_style_sheet_that_is_blocking_scripts();
        });

        // 2. Execute the first script in the list of scripts that will execute when the document has finished parsing.
        document->scripts_to_execute_when_parsing_has_finished().first()->execute_script();

        // 3. Remove the first script element from the list of scripts that will execute when the document has finished parsing (i.e. shift out the first entry in the list).
        (void)document->scripts_to_execute_when_parsing_has_finished().take_first();
    }

    // 6. Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following substeps:
    queue_global_task(HTML::Task::Source::DOMManipulation, *document, JS::create_heap_function(document->heap(), [document = document] {
        // 1. Set the Document's load timing info's DOM content loaded event start time to the current high resolution time given the Document's relevant global object.
        document->load_timing_info().dom_content_loaded_event_start_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(*document));

        // 2. Fire an event named DOMContentLoaded at the Document object, with its bubbles attribute initialized to true.
        auto content_loaded_event = DOM::Event::create(document->realm(), HTML::EventNames::DOMContentLoaded);
        content_loaded_event->set_bubbles(true);
        document->dispatch_event(content_loaded_event);

        // 3. Set the Document's load timing info's DOM content loaded event end time to the current high resolution time given the Document's relevant global object.
        document->load_timing_info().dom_content_loaded_event_end_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(*document));

        // FIXME: 4. Enable the client message queue of the ServiceWorkerContainer object whose associated service worker client is the Document object's relevant settings object.

        // FIXME: 5. Invoke WebDriver BiDi DOM content loaded with the Document's browsing context, and a new WebDriver BiDi navigation status whose id is the Document object's navigation id, status is "pending", and url is the Document object's URL.
    }));

    // 7. Spin the event loop until the set of scripts that will execute as soon as possible and the list of scripts that will execute in order as soon as possible are empty.
    main_thread_event_loop().spin_until([&] {
        return document->scripts_to_execute_as_soon_as_possible().is_empty();
    });

    // 8. Spin the event loop until there is nothing that delays the load event in the Document.
    main_thread_event_loop().spin_until([&] {
        return !document->anything_is_delaying_the_load_event();
    });

    // 9. Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following steps:
    queue_global_task(HTML::Task::Source::DOMManipulation, *document, JS::create_heap_function(document->heap(), [document = document] {
        // 1. Update the current document readiness to "complete".
        document->update_readiness(HTML::DocumentReadyState::Complete);

        // 2. If the Document object's browsing context is null, then abort these steps.
        if (!document->browsing_context())
            return;

        // 3. Let window be the Document's relevant global object.
        auto& window = verify_cast<Window>(relevant_global_object(*document));

        // 4. Set the Document's load timing info's load event start time to the current high resolution time given window.
        document->load_timing_info().load_event_start_time = HighResolutionTime::current_high_resolution_time(window);

        // 5. Fire an event named load at window, with legacy target override flag set.
        // FIXME: The legacy target override flag is currently set by a virtual override of dispatch_event()
        //        We should reorganize this so that the flag appears explicitly here instead.
        window.dispatch_event(DOM::Event::create(document->realm(), HTML::EventNames::load));

        // FIXME: 6. Invoke WebDriver BiDi load complete with the Document's browsing context, and a new WebDriver BiDi navigation status whose id is the Document object's navigation id, status is "complete", and url is the Document object's URL.

        // FIXME: 7. Set the Document object's navigation id to null.

        // 8. Set the Document's load timing info's load event end time to the current high resolution time given window.
        document->load_timing_info().load_event_end_time = HighResolutionTime::current_high_resolution_time(window);

        // 9. Assert: Document's page showing is false.
        VERIFY(!document->page_showing());

        // 10. Set the Document's page showing flag to true.
        document->set_page_showing(true);

        // 11. Fire a page transition event named pageshow at window with false.
        window.fire_a_page_transition_event(HTML::EventNames::pageshow, false);

        // 12. Completely finish loading the Document.
        document->completely_finish_loading();

        // FIXME: 13. Queue the navigation timing entry for the Document.
    }));

    // FIXME: 10. If the Document's print when loaded flag is set, then run the printing steps.

    // 11. The Document is now ready for post-load tasks.
    document->set_ready_for_post_load_tasks(true);
}

void HTMLParser::process_using_the_rules_for(InsertionMode mode, HTMLToken& token)
{
    switch (mode) {
    case InsertionMode::Initial:
        handle_initial(token);
        break;
    case InsertionMode::BeforeHTML:
        handle_before_html(token);
        break;
    case InsertionMode::BeforeHead:
        handle_before_head(token);
        break;
    case InsertionMode::InHead:
        handle_in_head(token);
        break;
    case InsertionMode::InHeadNoscript:
        handle_in_head_noscript(token);
        break;
    case InsertionMode::AfterHead:
        handle_after_head(token);
        break;
    case InsertionMode::InBody:
        handle_in_body(token);
        break;
    case InsertionMode::AfterBody:
        handle_after_body(token);
        break;
    case InsertionMode::AfterAfterBody:
        handle_after_after_body(token);
        break;
    case InsertionMode::Text:
        handle_text(token);
        break;
    case InsertionMode::InTable:
        handle_in_table(token);
        break;
    case InsertionMode::InTableBody:
        handle_in_table_body(token);
        break;
    case InsertionMode::InRow:
        handle_in_row(token);
        break;
    case InsertionMode::InCell:
        handle_in_cell(token);
        break;
    case InsertionMode::InTableText:
        handle_in_table_text(token);
        break;
    case InsertionMode::InSelectInTable:
        handle_in_select_in_table(token);
        break;
    case InsertionMode::InSelect:
        handle_in_select(token);
        break;
    case InsertionMode::InCaption:
        handle_in_caption(token);
        break;
    case InsertionMode::InColumnGroup:
        handle_in_column_group(token);
        break;
    case InsertionMode::InTemplate:
        handle_in_template(token);
        break;
    case InsertionMode::InFrameset:
        handle_in_frameset(token);
        break;
    case InsertionMode::AfterFrameset:
        handle_after_frameset(token);
        break;
    case InsertionMode::AfterAfterFrameset:
        handle_after_after_frameset(token);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

DOM::QuirksMode HTMLParser::which_quirks_mode(HTMLToken const& doctype_token) const
{
    if (doctype_token.doctype_data().force_quirks)
        return DOM::QuirksMode::Yes;

    // NOTE: The tokenizer puts the name into lower case for us.
    if (doctype_token.doctype_data().name != "html")
        return DOM::QuirksMode::Yes;

    auto const& public_identifier = doctype_token.doctype_data().public_identifier;
    auto const& system_identifier = doctype_token.doctype_data().system_identifier;

    if (public_identifier.equals_ignoring_ascii_case("-//W3O//DTD W3 HTML Strict 3.0//EN//"sv))
        return DOM::QuirksMode::Yes;

    if (public_identifier.equals_ignoring_ascii_case("-/W3C/DTD HTML 4.0 Transitional/EN"sv))
        return DOM::QuirksMode::Yes;

    if (public_identifier.equals_ignoring_ascii_case("HTML"sv))
        return DOM::QuirksMode::Yes;

    if (system_identifier.equals_ignoring_ascii_case("http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"sv))
        return DOM::QuirksMode::Yes;

    for (auto const& public_id : s_quirks_public_ids) {
        if (public_identifier.starts_with_bytes(public_id, CaseSensitivity::CaseInsensitive))
            return DOM::QuirksMode::Yes;
    }

    if (doctype_token.doctype_data().missing_system_identifier) {
        if (public_identifier.starts_with_bytes("-//W3C//DTD HTML 4.01 Frameset//"sv, CaseSensitivity::CaseInsensitive))
            return DOM::QuirksMode::Yes;

        if (public_identifier.starts_with_bytes("-//W3C//DTD HTML 4.01 Transitional//"sv, CaseSensitivity::CaseInsensitive))
            return DOM::QuirksMode::Yes;
    }

    if (public_identifier.starts_with_bytes("-//W3C//DTD XHTML 1.0 Frameset//"sv, CaseSensitivity::CaseInsensitive))
        return DOM::QuirksMode::Limited;

    if (public_identifier.starts_with_bytes("-//W3C//DTD XHTML 1.0 Transitional//"sv, CaseSensitivity::CaseInsensitive))
        return DOM::QuirksMode::Limited;

    if (!doctype_token.doctype_data().missing_system_identifier) {
        if (public_identifier.starts_with_bytes("-//W3C//DTD HTML 4.01 Frameset//"sv, CaseSensitivity::CaseInsensitive))
            return DOM::QuirksMode::Limited;

        if (public_identifier.starts_with_bytes("-//W3C//DTD HTML 4.01 Transitional//"sv, CaseSensitivity::CaseInsensitive))
            return DOM::QuirksMode::Limited;
    }

    return DOM::QuirksMode::No;
}

void HTMLParser::handle_initial(HTMLToken& token)
{
    if (token.is_character() && token.is_parser_whitespace()) {
        return;
    }

    if (token.is_comment()) {
        auto comment = realm().heap().allocate<DOM::Comment>(realm(), document(), token.comment());
        MUST(document().append_child(*comment));
        return;
    }

    if (token.is_doctype()) {
        auto doctype = realm().heap().allocate<DOM::DocumentType>(realm(), document());
        doctype->set_name(token.doctype_data().name);
        doctype->set_public_id(token.doctype_data().public_identifier);
        doctype->set_system_id(token.doctype_data().system_identifier);
        MUST(document().append_child(*doctype));
        document().set_quirks_mode(which_quirks_mode(token));
        m_insertion_mode = InsertionMode::BeforeHTML;
        return;
    }

    log_parse_error();
    document().set_quirks_mode(DOM::QuirksMode::Yes);
    m_insertion_mode = InsertionMode::BeforeHTML;
    process_using_the_rules_for(InsertionMode::BeforeHTML, token);
}

// https://html.spec.whatwg.org/multipage/parsing.html#the-before-html-insertion-mode
void HTMLParser::handle_before_html(HTMLToken& token)
{
    // -> A DOCTYPE token
    if (token.is_doctype()) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // -> A comment token
    if (token.is_comment()) {
        // Insert a comment as the last child of the Document object.
        auto comment = realm().heap().allocate<DOM::Comment>(realm(), document(), token.comment());
        MUST(document().append_child(*comment));
        return;
    }

    // -> A character token that is one of U+0009 CHARACTER TABULATION, U+000A LINE FEED (LF), U+000C FORM FEED (FF), U+000D CARRIAGE RETURN (CR), or U+0020 SPACE
    if (token.is_character() && token.is_parser_whitespace()) {
        // Ignore the token.
        return;
    }

    // -> A start tag whose tag name is "html"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        // Create an element for the token in the HTML namespace, with the Document as the intended parent. Append it to the Document object. Put this element in the stack of open elements.
        auto element = create_element_for(token, Namespace::HTML, document());
        MUST(document().append_child(*element));
        m_stack_of_open_elements.push(move(element));

        // Switch the insertion mode to "before head".
        m_insertion_mode = InsertionMode::BeforeHead;
        return;
    }

    // -> An end tag whose tag name is one of: "head", "body", "html", "br"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::head, HTML::TagNames::body, HTML::TagNames::html, HTML::TagNames::br)) {
        // Act as described in the "anything else" entry below.
        goto AnythingElse;
    }

    // -> Any other end tag
    if (token.is_end_tag()) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // -> Anything else
AnythingElse:
    // Create an html element whose node document is the Document object. Append it to the Document object. Put this element in the stack of open elements.
    auto element = create_element(document(), HTML::TagNames::html, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(document().append_child(element));
    m_stack_of_open_elements.push(element);

    // Switch the insertion mode to "before head", then reprocess the token.
    m_insertion_mode = InsertionMode::BeforeHead;
    process_using_the_rules_for(InsertionMode::BeforeHead, token);
    return;
}

DOM::Element& HTMLParser::current_node()
{
    return m_stack_of_open_elements.current_node();
}

// https://html.spec.whatwg.org/multipage/parsing.html#adjusted-current-node
DOM::Element& HTMLParser::adjusted_current_node()
{
    // The adjusted current node is the context element if the parser was created as part of the
    // HTML fragment parsing algorithm and the stack of open elements has only one element in it
    // (fragment case); otherwise, the adjusted current node is the current node.

    if (m_parsing_fragment && m_stack_of_open_elements.elements().size() == 1)
        return *m_context_element;

    return current_node();
}

DOM::Element& HTMLParser::node_before_current_node()
{
    return *m_stack_of_open_elements.elements().at(m_stack_of_open_elements.elements().size() - 2);
}

// https://html.spec.whatwg.org/multipage/parsing.html#appropriate-place-for-inserting-a-node
HTMLParser::AdjustedInsertionLocation HTMLParser::find_appropriate_place_for_inserting_node(JS::GCPtr<DOM::Element> override_target)
{
    // 1. If there was an override target specified, then let target be the override target.
    auto& target = override_target ? *override_target.ptr() : current_node();
    HTMLParser::AdjustedInsertionLocation adjusted_insertion_location;

    // 2. Determine the adjusted insertion location using the first matching steps from the following list:

    // `-> If foster parenting is enabled and target is a table, tbody, tfoot, thead, or tr element
    if (m_foster_parenting && target.local_name().is_one_of(HTML::TagNames::table, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::tr)) {
        // 1. Let last template be the last template element in the stack of open elements, if any.
        auto last_template = m_stack_of_open_elements.last_element_with_tag_name(HTML::TagNames::template_);
        // 2. Let last table be the last table element in the stack of open elements, if any.
        auto last_table = m_stack_of_open_elements.last_element_with_tag_name(HTML::TagNames::table);
        // 3. If there is a last template and either there is no last table,
        //    or there is one, but last template is lower (more recently added) than last table in the stack of open elements,
        if (last_template.element && (!last_table.element || last_template.index > last_table.index)) {
            // then: let adjusted insertion location be inside last template's template contents, after its last child (if any), and abort these steps.

            // NOTE: This returns the template content, so no need to check the parent is a template.
            return { verify_cast<HTMLTemplateElement>(*last_template.element).content().ptr(), nullptr };
        }
        // 4. If there is no last table, then let adjusted insertion location be inside the first element in the stack of open elements (the html element),
        //    after its last child (if any), and abort these steps. (fragment case)
        if (!last_table.element) {
            VERIFY(m_parsing_fragment);
            // Guaranteed not to be a template element (it will be the html element),
            // so no need to check the parent is a template.
            return { *m_stack_of_open_elements.elements().first(), nullptr };
        }
        // 5. If last table has a parent node, then let adjusted insertion location be inside last table's parent node, immediately before last table, and abort these steps.
        if (last_table.element->parent_node()) {
            adjusted_insertion_location = { last_table.element->parent_node(), last_table.element.ptr() };
        } else {
            // 6. Let previous element be the element immediately above last table in the stack of open elements.
            auto previous_element = m_stack_of_open_elements.element_immediately_above(*last_table.element);

            // 7. Let adjusted insertion location be inside previous element, after its last child (if any).
            adjusted_insertion_location = { previous_element.ptr(), nullptr };
        }
    } else {
        // `-> Otherwise
        //     Let adjusted insertion location be inside target, after its last child (if any).
        adjusted_insertion_location = { target, nullptr };
    }

    // 3. If the adjusted insertion location is inside a template element,
    //    let it instead be inside the template element's template contents, after its last child (if any).
    if (is<HTMLTemplateElement>(*adjusted_insertion_location.parent))
        adjusted_insertion_location = { verify_cast<HTMLTemplateElement>(*adjusted_insertion_location.parent).content().ptr(), nullptr };

    // 4. Return the adjusted insertion location.
    return adjusted_insertion_location;
}

// https://html.spec.whatwg.org/multipage/parsing.html#create-an-element-for-the-token
JS::NonnullGCPtr<DOM::Element> HTMLParser::create_element_for(HTMLToken const& token, Optional<FlyString> const& namespace_, DOM::Node& intended_parent)
{
    // FIXME: 1. If the active speculative HTML parser is not null, then return the result of creating a speculative mock element given given namespace, the tag name of the given token, and the attributes of the given token.
    // FIXME: 2. Otherwise, optionally create a speculative mock element given given namespace, the tag name of the given token, and the attributes of the given token.

    // 3. Let document be intended parent's node document.
    JS::NonnullGCPtr<DOM::Document> document = intended_parent.document();

    // 4. Let local name be the tag name of the token.
    auto const& local_name = token.tag_name();

    // 5. Let is be the value of the "is" attribute in the given token, if such an attribute exists, or null otherwise.
    auto is_value = token.attribute(AttributeNames::is);

    // 6. Let definition be the result of looking up a custom element definition given document, given namespace, local name, and is.
    auto definition = document->lookup_custom_element_definition(namespace_, local_name, is_value);

    // 7. If definition is non-null and the parser was not created as part of the HTML fragment parsing algorithm, then let will execute script be true. Otherwise, let it be false.
    bool will_execute_script = definition && !m_parsing_fragment;

    // 8. If will execute script is true, then:
    if (will_execute_script) {
        // 1. Increment document's throw-on-dynamic-markup-insertion counter.
        document->increment_throw_on_dynamic_markup_insertion_counter({});

        // 2. If the JavaScript execution context stack is empty, then perform a microtask checkpoint.
        auto& vm = main_thread_event_loop().vm();
        if (vm.execution_context_stack().is_empty())
            perform_a_microtask_checkpoint();

        // 3. Push a new element queue onto document's relevant agent's custom element reactions stack.
        auto& custom_data = verify_cast<Bindings::WebEngineCustomData>(*vm.custom_data());
        custom_data.custom_element_reactions_stack.element_queue_stack.append({});
    }

    // 9. Let element be the result of creating an element given document, localName, given namespace, null, and is.
    //    If will execute script is true, set the synchronous custom elements flag; otherwise, leave it unset.
    auto element = create_element(*document, local_name, namespace_, {}, is_value, will_execute_script).release_value_but_fixme_should_propagate_errors();

    // 10. Append each attribute in the given token to element.
    token.for_each_attribute([&](auto const& attribute) {
        DOM::QualifiedName qualified_name { attribute.local_name, attribute.prefix, attribute.namespace_ };
        auto dom_attribute = realm().heap().allocate<DOM::Attr>(realm(), *document, move(qualified_name), attribute.value, element);
        element->append_attribute(dom_attribute);
        return IterationDecision::Continue;
    });

    // 11. If will execute script is true, then:
    if (will_execute_script) {
        // 1. Let queue be the result of popping from document's relevant agent's custom element reactions stack. (This will be the same element queue as was pushed above.)
        auto& vm = main_thread_event_loop().vm();
        auto& custom_data = verify_cast<Bindings::WebEngineCustomData>(*vm.custom_data());
        auto queue = custom_data.custom_element_reactions_stack.element_queue_stack.take_last();

        // 2. Invoke custom element reactions in queue.
        Bindings::invoke_custom_element_reactions(queue);

        // 3. Decrement document's throw-on-dynamic-markup-insertion counter.
        document->decrement_throw_on_dynamic_markup_insertion_counter({});
    }

    // FIXME: 12. If element has an xmlns attribute in the XMLNS namespace whose value is not exactly the same as the element's namespace, that is a parse error.
    //            Similarly, if element has an xmlns:xlink attribute in the XMLNS namespace whose value is not the XLink Namespace, that is a parse error.

    // FIXME: 13. If element is a resettable element, invoke its reset algorithm. (This initializes the element's value and checkedness based on the element's attributes.)

    // 14. If element is a form-associated element and not a form-associated custom element, the form element pointer is not null, there is no template element on the stack of open elements,
    //     element is either not listed or doesn't have a form attribute, and the intended parent is in the same tree as the element pointed to by the form element pointer,
    //     then associate element with the form element pointed to by the form element pointer and set element's parser inserted flag.
    // FIXME: Check if the element is not a form-associated custom element.
    if (is<FormAssociatedElement>(*element)) {
        auto* form_associated_element = dynamic_cast<FormAssociatedElement*>(element.ptr());
        VERIFY(form_associated_element);

        auto& html_element = form_associated_element->form_associated_element_to_html_element();

        if (m_form_element.ptr()
            && !m_stack_of_open_elements.contains(HTML::TagNames::template_)
            && (!form_associated_element->is_listed() || !html_element.has_attribute(HTML::AttributeNames::form))
            && &intended_parent.root() == &m_form_element->root()) {
            form_associated_element->set_form(m_form_element.ptr());
            form_associated_element->set_parser_inserted({});
        }
    }

    // 15. Return element.
    return element;
}

// https://html.spec.whatwg.org/multipage/parsing.html#insert-a-foreign-element
JS::NonnullGCPtr<DOM::Element> HTMLParser::insert_foreign_element(HTMLToken const& token, Optional<FlyString> const& namespace_, OnlyAddToElementStack only_add_to_element_stack)
{
    // 1. Let the adjusted insertion location be the appropriate place for inserting a node.
    auto adjusted_insertion_location = find_appropriate_place_for_inserting_node();

    // 2. Let element be the result of creating an element for the token in the given namespace,
    //    with the intended parent being the element in which the adjusted insertion location finds itself.
    auto element = create_element_for(token, namespace_, *adjusted_insertion_location.parent);

    // 3. If onlyAddToElementStack is false, then run insert an element at the adjusted insertion location with element.
    if (only_add_to_element_stack == OnlyAddToElementStack::No) {
        insert_an_element_at_the_adjusted_insertion_location(element);
    }

    // 4. Push element onto the stack of open elements so that it is the new current node.
    m_stack_of_open_elements.push(element);

    // 5. Return element.
    return element;
}

JS::NonnullGCPtr<DOM::Element> HTMLParser::insert_html_element(HTMLToken const& token)
{
    return insert_foreign_element(token, Namespace::HTML, OnlyAddToElementStack::No);
}

void HTMLParser::handle_before_head(HTMLToken& token)
{
    if (token.is_character() && token.is_parser_whitespace()) {
        return;
    }

    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::head) {
        auto element = insert_html_element(token);
        m_head_element = JS::make_handle(verify_cast<HTMLHeadElement>(*element));
        m_insertion_mode = InsertionMode::InHead;
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::head, HTML::TagNames::body, HTML::TagNames::html, HTML::TagNames::br)) {
        goto AnythingElse;
    }

    if (token.is_end_tag()) {
        log_parse_error();
        return;
    }

AnythingElse:
    m_head_element = JS::make_handle(verify_cast<HTMLHeadElement>(*insert_html_element(HTMLToken::make_start_tag(HTML::TagNames::head))));
    m_insertion_mode = InsertionMode::InHead;
    process_using_the_rules_for(InsertionMode::InHead, token);
    return;
}

void HTMLParser::insert_comment(HTMLToken& token)
{
    auto adjusted_insertion_location = find_appropriate_place_for_inserting_node();
    adjusted_insertion_location.parent->insert_before(realm().heap().allocate<DOM::Comment>(realm(), document(), token.comment()), adjusted_insertion_location.insert_before_sibling);
}

void HTMLParser::handle_in_head(HTMLToken& token)
{
    if (token.is_parser_whitespace()) {
        insert_character(token.code_point());
        return;
    }

    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::base, HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::link)) {
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();
        token.acknowledge_self_closing_flag_if_set();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::meta) {
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();
        token.acknowledge_self_closing_flag_if_set();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::title) {
        (void)insert_html_element(token);
        m_tokenizer.switch_to({}, HTMLTokenizer::State::RCDATA);
        m_original_insertion_mode = m_insertion_mode;
        m_insertion_mode = InsertionMode::Text;
        return;
    }

    if (token.is_start_tag() && ((token.tag_name() == HTML::TagNames::noscript && m_scripting_enabled) || token.tag_name() == HTML::TagNames::noframes || token.tag_name() == HTML::TagNames::style)) {
        parse_generic_raw_text_element(token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::noscript && !m_scripting_enabled) {
        (void)insert_html_element(token);
        m_insertion_mode = InsertionMode::InHeadNoscript;
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::script) {
        auto adjusted_insertion_location = find_appropriate_place_for_inserting_node();
        auto element = create_element_for(token, Namespace::HTML, *adjusted_insertion_location.parent);
        auto& script_element = verify_cast<HTMLScriptElement>(*element);
        script_element.set_parser_document(Badge<HTMLParser> {}, document());
        script_element.set_force_async(Badge<HTMLParser> {}, false);
        script_element.set_source_line_number({}, token.start_position().line + 1); // FIXME: This +1 is incorrect for script tags whose script does not start on a new line

        if (m_parsing_fragment) {
            script_element.set_already_started(Badge<HTMLParser> {}, true);
        }

        if (m_invoked_via_document_write) {
            TODO();
        }

        adjusted_insertion_location.parent->insert_before(*element, adjusted_insertion_location.insert_before_sibling, false);
        m_stack_of_open_elements.push(element);
        m_tokenizer.switch_to({}, HTMLTokenizer::State::ScriptData);
        m_original_insertion_mode = m_insertion_mode;
        m_insertion_mode = InsertionMode::Text;
        return;
    }
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::head) {
        (void)m_stack_of_open_elements.pop();
        m_insertion_mode = InsertionMode::AfterHead;
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::body, HTML::TagNames::html, HTML::TagNames::br)) {
        goto AnythingElse;
    }

    // -> A start tag whose tag name is "template"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::template_) {
        // Let template start tag be the start tag.
        auto const& template_start_tag = token;

        // Insert a marker at the end of the list of active formatting elements.
        m_list_of_active_formatting_elements.add_marker();

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // Switch the insertion mode to "in template".
        m_insertion_mode = InsertionMode::InTemplate;

        // Push "in template" onto the stack of template insertion modes so that it is the new current template insertion mode.
        m_stack_of_template_insertion_modes.append(InsertionMode::InTemplate);

        // Let the adjusted insertion location be the appropriate place for inserting a node.
        auto adjusted_insertion_location = find_appropriate_place_for_inserting_node();

        // Let intended parent be the element in which the adjusted insertion location finds itself.
        auto& intended_parent = adjusted_insertion_location.parent;

        // Let document be intended parent's node document.
        auto& document = intended_parent->document();

        Optional<Bindings::ShadowRootMode> shadowrootmode = {};
        {
            auto shadowrootmode_attribute_value = template_start_tag.attribute(HTML::AttributeNames::shadowrootmode);
            if (shadowrootmode_attribute_value.has_value()) {
                if (shadowrootmode_attribute_value.value() == "open"sv) {
                    shadowrootmode = Bindings::ShadowRootMode::Open;
                } else if (shadowrootmode_attribute_value.value() == "closed"sv) {
                    shadowrootmode = Bindings::ShadowRootMode::Closed;
                }
            }
        }

        // If any of the following are false:
        // - template start tag's shadowrootmode is not in the none state;
        // - Document's allow declarative shadow roots is true; or
        // - the adjusted current node is not the topmost element in the stack of open elements,
        if (!shadowrootmode.has_value()
            || !document.allow_declarative_shadow_roots()
            || &adjusted_current_node() == &m_stack_of_open_elements.first()) {
            // then insert an HTML element for the token.
            (void)insert_html_element(token);
        }

        // Otherwise:
        else {
            // 1. Let declarative shadow host element be adjusted current node.
            auto& declarative_shadow_host_element = adjusted_current_node();

            // 2. Let template be the result of insert a foreign element for template start tag, with HTML namespace and true.
            auto template_ = insert_foreign_element(template_start_tag, Namespace::HTML, OnlyAddToElementStack::Yes);

            // 3. Let mode be template start tag's shadowrootmode attribute's value.
            auto mode = shadowrootmode.value();

            // 4. Let clonable be true if template start tag has a shadowrootclonable attribute; otherwise false.
            auto clonable = template_start_tag.attribute(HTML::AttributeNames::shadowrootclonable).has_value();

            // 5. Let serializable be true if template start tag has a shadowrootserializable attribute; otherwise false.
            auto serializable = template_start_tag.attribute(HTML::AttributeNames::shadowrootserializable).has_value();

            // 6. Let delegatesFocus be true if template start tag has a shadowrootdelegatesfocus attribute; otherwise false.
            auto delegates_focus = template_start_tag.attribute(HTML::AttributeNames::shadowrootdelegatesfocus).has_value();

            // 7. If declarative shadow host element is a shadow host, then insert an element at the adjusted insertion location with template.
            if (declarative_shadow_host_element.is_shadow_host()) {
                // FIXME: We do manual "insert before" instead of "insert an element at the adjusted insertion location" here
                //        Otherwise, two template elements in a row will cause the second to try to insert into itself.
                //        This might be a spec bug(?)
                adjusted_insertion_location.parent->insert_before(*template_, adjusted_insertion_location.insert_before_sibling);
            }

            // 8. Otherwise:
            else {
                // 1. Attach a shadow root with declarative shadow host element, mode, clonable, serializable, delegatesFocus, and "named".
                //    If an exception is thrown, then catch it, report the exception, insert an element at the adjusted insertion location with template, and return.
                auto result = declarative_shadow_host_element.attach_a_shadow_root(mode, clonable, serializable, delegates_focus, Bindings::SlotAssignmentMode::Named);
                if (result.is_error()) {
                    report_exception(Bindings::dom_exception_to_throw_completion(vm(), result.release_error()), realm());
                    insert_an_element_at_the_adjusted_insertion_location(template_);
                    return;
                }

                // 2. Let shadow be declarative shadow host element's shadow root.
                auto& shadow = *declarative_shadow_host_element.shadow_root();

                // 3. Set shadow's declarative to true.
                shadow.set_declarative(true);

                // 4. Set template's template contents property to shadow.
                verify_cast<HTMLTemplateElement>(*template_).set_template_contents(shadow);

                // 5. Set shadow's available to element internals to true.
                shadow.set_available_to_element_internals(true);
            }
        }

        return;
    }

    // -> An end tag whose tag name is "template"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::template_) {
        // If there is no template element on the stack of open elements, then this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.contains(HTML::TagNames::template_)) {
            log_parse_error();
            return;
        }

        // Otherwise, run these steps:

        // 1. Generate all implied end tags thoroughly.
        generate_all_implied_end_tags_thoroughly();

        // 2. If the current node is not a template element, then this is a parse error.
        if (current_node().local_name() != HTML::TagNames::template_)
            log_parse_error();

        // 3. Pop elements from the stack of open elements until a template element has been popped from the stack.
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::template_);

        // 4. Clear the list of active formatting elements up to the last marker.
        m_list_of_active_formatting_elements.clear_up_to_the_last_marker();

        // 5. Pop the current template insertion mode off the stack of template insertion modes.
        m_stack_of_template_insertion_modes.take_last();

        // 6. Reset the insertion mode appropriately.
        reset_the_insertion_mode_appropriately();
        return;
    }

    if ((token.is_start_tag() && token.tag_name() == HTML::TagNames::head) || token.is_end_tag()) {
        log_parse_error();
        return;
    }

AnythingElse:
    (void)m_stack_of_open_elements.pop();
    m_insertion_mode = InsertionMode::AfterHead;
    process_using_the_rules_for(m_insertion_mode, token);
}

void HTMLParser::handle_in_head_noscript(HTMLToken& token)
{
    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::noscript) {
        (void)m_stack_of_open_elements.pop();
        m_insertion_mode = InsertionMode::InHead;
        return;
    }

    if (token.is_parser_whitespace() || token.is_comment() || (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::link, HTML::TagNames::meta, HTML::TagNames::noframes, HTML::TagNames::style))) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::br) {
        goto AnythingElse;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::head, HTML::TagNames::noscript)) {
        log_parse_error();
        return;
    }

AnythingElse:
    log_parse_error();
    (void)m_stack_of_open_elements.pop();
    m_insertion_mode = InsertionMode::InHead;
    process_using_the_rules_for(m_insertion_mode, token);
}

void HTMLParser::parse_generic_raw_text_element(HTMLToken& token)
{
    (void)insert_html_element(token);
    m_tokenizer.switch_to({}, HTMLTokenizer::State::RAWTEXT);
    m_original_insertion_mode = m_insertion_mode;
    m_insertion_mode = InsertionMode::Text;
}

static bool is_empty_text_node(DOM::Node const* node)
{
    return node && node->is_text() && static_cast<DOM::Text const*>(node)->data().is_empty();
}

DOM::Text* HTMLParser::find_character_insertion_node()
{
    auto adjusted_insertion_location = find_appropriate_place_for_inserting_node();
    if (adjusted_insertion_location.insert_before_sibling) {
        if (is_empty_text_node(adjusted_insertion_location.insert_before_sibling->previous_sibling()))
            return static_cast<DOM::Text*>(adjusted_insertion_location.insert_before_sibling->previous_sibling());
        auto new_text_node = realm().heap().allocate<DOM::Text>(realm(), document(), String {});
        adjusted_insertion_location.parent->insert_before(*new_text_node, *adjusted_insertion_location.insert_before_sibling);
        return new_text_node;
    }
    if (adjusted_insertion_location.parent->is_document())
        return nullptr;
    if (is_empty_text_node(adjusted_insertion_location.parent->last_child()))
        return static_cast<DOM::Text*>(adjusted_insertion_location.parent->last_child());
    auto new_text_node = realm().heap().allocate<DOM::Text>(realm(), document(), String {});
    MUST(adjusted_insertion_location.parent->append_child(*new_text_node));
    return new_text_node;
}

void HTMLParser::flush_character_insertions()
{
    if (m_character_insertion_builder.is_empty())
        return;
    m_character_insertion_node->set_data(MUST(m_character_insertion_builder.to_string()));
    m_character_insertion_builder.clear();
}

void HTMLParser::insert_character(u32 data)
{
    auto node = find_character_insertion_node();
    if (node == m_character_insertion_node.ptr()) {
        m_character_insertion_builder.append(Utf32View { &data, 1 });
        return;
    }
    if (!m_character_insertion_node.ptr()) {
        m_character_insertion_node = JS::make_handle(node);
        m_character_insertion_builder.append(Utf32View { &data, 1 });
        return;
    }
    flush_character_insertions();
    m_character_insertion_node = JS::make_handle(node);
    m_character_insertion_builder.append(Utf32View { &data, 1 });
}

void HTMLParser::handle_after_head(HTMLToken& token)
{
    if (token.is_character() && token.is_parser_whitespace()) {
        insert_character(token.code_point());
        return;
    }

    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::body) {
        (void)insert_html_element(token);
        m_frameset_ok = false;
        m_insertion_mode = InsertionMode::InBody;
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::frameset) {
        (void)insert_html_element(token);
        m_insertion_mode = InsertionMode::InFrameset;
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::base, HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::link, HTML::TagNames::meta, HTML::TagNames::noframes, HTML::TagNames::script, HTML::TagNames::style, HTML::TagNames::template_, HTML::TagNames::title)) {
        log_parse_error();
        m_stack_of_open_elements.push(*m_head_element);
        process_using_the_rules_for(InsertionMode::InHead, token);
        m_stack_of_open_elements.elements().remove_first_matching([&](auto& entry) {
            return entry.ptr() == m_head_element.ptr();
        });
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::template_) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::body, HTML::TagNames::html, HTML::TagNames::br)) {
        goto AnythingElse;
    }

    if ((token.is_start_tag() && token.tag_name() == HTML::TagNames::head) || token.is_end_tag()) {
        log_parse_error();
        return;
    }

AnythingElse:
    (void)insert_html_element(HTMLToken::make_start_tag(HTML::TagNames::body));
    m_insertion_mode = InsertionMode::InBody;
    process_using_the_rules_for(m_insertion_mode, token);
}

void HTMLParser::generate_implied_end_tags(FlyString const& exception)
{
    while (current_node().local_name() != exception && current_node().local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt, HTML::TagNames::li, HTML::TagNames::optgroup, HTML::TagNames::option, HTML::TagNames::p, HTML::TagNames::rb, HTML::TagNames::rp, HTML::TagNames::rt, HTML::TagNames::rtc))
        (void)m_stack_of_open_elements.pop();
}

void HTMLParser::generate_all_implied_end_tags_thoroughly()
{
    while (current_node().local_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::colgroup, HTML::TagNames::dd, HTML::TagNames::dt, HTML::TagNames::li, HTML::TagNames::optgroup, HTML::TagNames::option, HTML::TagNames::p, HTML::TagNames::rb, HTML::TagNames::rp, HTML::TagNames::rt, HTML::TagNames::rtc, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr))
        (void)m_stack_of_open_elements.pop();
}

void HTMLParser::close_a_p_element()
{
    generate_implied_end_tags(HTML::TagNames::p);
    if (current_node().local_name() != HTML::TagNames::p) {
        log_parse_error();
    }
    m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::p);
}

void HTMLParser::handle_after_body(HTMLToken& token)
{
    if (token.is_character() && token.is_parser_whitespace()) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_comment()) {
        auto& insertion_location = m_stack_of_open_elements.first();
        MUST(insertion_location.append_child(realm().heap().allocate<DOM::Comment>(realm(), document(), token.comment())));
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::html) {
        if (m_parsing_fragment) {
            log_parse_error();
            return;
        }
        m_insertion_mode = InsertionMode::AfterAfterBody;
        return;
    }

    if (token.is_end_of_file()) {
        stop_parsing();
        return;
    }

    log_parse_error();
    m_insertion_mode = InsertionMode::InBody;
    process_using_the_rules_for(InsertionMode::InBody, token);
}

void HTMLParser::handle_after_after_body(HTMLToken& token)
{
    if (token.is_comment()) {
        auto comment = realm().heap().allocate<DOM::Comment>(realm(), document(), token.comment());
        MUST(document().append_child(*comment));
        return;
    }

    if (token.is_doctype() || token.is_parser_whitespace() || (token.is_start_tag() && token.tag_name() == HTML::TagNames::html)) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_end_of_file()) {
        stop_parsing();
        return;
    }

    log_parse_error();
    m_insertion_mode = InsertionMode::InBody;
    process_using_the_rules_for(m_insertion_mode, token);
}

// https://html.spec.whatwg.org/multipage/parsing.html#reconstruct-the-active-formatting-elements
void HTMLParser::reconstruct_the_active_formatting_elements()
{
    // 1. If there are no entries in the list of active formatting elements, then there is nothing to reconstruct; stop this algorithm.
    if (m_list_of_active_formatting_elements.is_empty())
        return;

    // 2. If the last (most recently added) entry in the list of active formatting elements is a marker, or if it is an element that is in the stack of open elements,
    //    then there is nothing to reconstruct; stop this algorithm.
    if (m_list_of_active_formatting_elements.entries().last().is_marker())
        return;

    if (m_stack_of_open_elements.contains(*m_list_of_active_formatting_elements.entries().last().element))
        return;

    // 3. Let entry be the last (most recently added) element in the list of active formatting elements.
    size_t index = m_list_of_active_formatting_elements.entries().size() - 1;

    // NOTE: Entry will never be null, but must be a pointer instead of a reference to allow rebinding.
    auto* entry = &m_list_of_active_formatting_elements.entries().at(index);

Rewind:
    // 4. Rewind: If there are no entries before entry in the list of active formatting elements, then jump to the step labeled create.
    if (index == 0)
        goto Create;

    // 5. Let entry be the entry one earlier than entry in the list of active formatting elements.
    --index;
    entry = &m_list_of_active_formatting_elements.entries().at(index);

    // 6. If entry is neither a marker nor an element that is also in the stack of open elements, go to the step labeled rewind.
    if (!entry->is_marker() && !m_stack_of_open_elements.contains(*entry->element))
        goto Rewind;

Advance:
    // 7. Advance: Let entry be the element one later than entry in the list of active formatting elements.
    ++index;
    entry = &m_list_of_active_formatting_elements.entries().at(index);

Create:
    // 8. Create: Insert an HTML element for the token for which the element entry was created, to obtain new element.
    VERIFY(!entry->is_marker());

    // FIXME: Hold on to the real token!
    auto new_element = insert_html_element(HTMLToken::make_start_tag(entry->element->local_name()));

    // 9. Replace the entry for entry in the list with an entry for new element.
    m_list_of_active_formatting_elements.entries().at(index).element = JS::make_handle(new_element);

    // 10. If the entry for new element in the list of active formatting elements is not the last entry in the list, return to the step labeled advance.
    if (index != m_list_of_active_formatting_elements.entries().size() - 1)
        goto Advance;
}

// https://html.spec.whatwg.org/multipage/parsing.html#adoption-agency-algorithm
HTMLParser::AdoptionAgencyAlgorithmOutcome HTMLParser::run_the_adoption_agency_algorithm(HTMLToken& token)
{
    // 1. Let subject be token's tag name.
    auto const& subject = token.tag_name();

    // 2. If the current node is an HTML element whose tag name is subject,
    //    and the current node is not in the list of active formatting elements,
    //    then pop the current node off the stack of open elements, and return.
    if (current_node().local_name() == subject && !m_list_of_active_formatting_elements.contains(current_node())) {
        (void)m_stack_of_open_elements.pop();
        return AdoptionAgencyAlgorithmOutcome::DoNothing;
    }

    // 3. Let outer loop counter be 0.
    size_t outer_loop_counter = 0;

    // 4. While true:
    while (true) {
        // 1. If outer loop counter is greater than or equal to 8, then return.
        if (outer_loop_counter >= 8)
            return AdoptionAgencyAlgorithmOutcome::DoNothing;

        // 2. Increment outer loop counter by 1.
        outer_loop_counter++;

        // 3. Let formatting element be the last element in the list of active formatting elements that:
        //    - is between the end of the list and the last marker in the list, if any, or the start of the list otherwise, and
        //    - has the tag name subject.
        auto* formatting_element = m_list_of_active_formatting_elements.last_element_with_tag_name_before_marker(subject);

        // If there is no such element, then return and instead act as described in the "any other end tag" entry above.
        if (!formatting_element)
            return AdoptionAgencyAlgorithmOutcome::RunAnyOtherEndTagSteps;

        // 4. If formatting element is not in the stack of open elements,
        if (!m_stack_of_open_elements.contains(*formatting_element)) {
            // then this is a parse error;
            log_parse_error();
            // remove the element from the list,
            m_list_of_active_formatting_elements.remove(*formatting_element);
            // and return.
            return AdoptionAgencyAlgorithmOutcome::DoNothing;
        }

        // 5. If formatting element is in the stack of open elements, but the element is not in scope,
        if (!m_stack_of_open_elements.has_in_scope(*formatting_element)) {
            // then this is a parse error;
            log_parse_error();
            // return.
            return AdoptionAgencyAlgorithmOutcome::DoNothing;
        }

        // 6. If formatting element is not the current node,
        if (formatting_element != &current_node()) {
            // this is a parse error. (But do not return.)
            log_parse_error();
        }

        // 7. Let furthest block be the topmost node in the stack of open elements that is lower in the stack than formatting element,
        //    and is an element in the special category. There might not be one.
        JS::GCPtr<DOM::Element> furthest_block = m_stack_of_open_elements.topmost_special_node_below(*formatting_element);

        // 8. If there is no furthest block
        if (!furthest_block) {
            // then the UA must first pop all the nodes from the bottom of the stack of open elements,
            // from the current node up to and including formatting element,
            while (&current_node() != formatting_element)
                (void)m_stack_of_open_elements.pop();
            (void)m_stack_of_open_elements.pop();

            // then remove formatting element from the list of active formatting elements,
            m_list_of_active_formatting_elements.remove(*formatting_element);
            // and finally return.
            return AdoptionAgencyAlgorithmOutcome::DoNothing;
        }

        // 9. Let common ancestor be the element immediately above formatting element in the stack of open elements.
        auto common_ancestor = m_stack_of_open_elements.element_immediately_above(*formatting_element);

        // 10. Let a bookmark note the position of formatting element in the list of active formatting elements
        //     relative to the elements on either side of it in the list.
        auto bookmark = m_list_of_active_formatting_elements.find_index(*formatting_element).value();

        // 11. Let node and last node be furthest block.
        auto node = furthest_block;
        auto last_node = furthest_block;

        // Keep track of this for later
        auto node_above_node = m_stack_of_open_elements.element_immediately_above(*node);

        // 12. Let inner loop counter be 0.
        size_t inner_loop_counter = 0;

        // 13. While true:
        while (true) {
            // 1. Increment inner loop counter by 1.
            inner_loop_counter++;

            // 2. Let node be the element immediately above node in the stack of open elements,
            //    or if node is no longer in the stack of open elements (e.g. because it got removed by this algorithm),
            //    the element that was immediately above node in the stack of open elements before node was removed.
            node = node_above_node;
            VERIFY(node);

            // Keep track of this for later
            node_above_node = m_stack_of_open_elements.element_immediately_above(*node);

            // 3. If node is formatting element, then break.
            if (node.ptr() == formatting_element)
                break;

            // 4. If inner loop counter is greater than 3 and node is in the list of active formatting elements,
            if (inner_loop_counter > 3 && m_list_of_active_formatting_elements.contains(*node)) {
                auto node_index = m_list_of_active_formatting_elements.find_index(*node);
                if (node_index.has_value() && node_index.value() < bookmark)
                    bookmark--;
                // then remove node from the list of active formatting elements.
                m_list_of_active_formatting_elements.remove(*node);
            }

            // 5. If node is not in the list of active formatting elements
            if (!m_list_of_active_formatting_elements.contains(*node)) {
                // then remove node from the stack of open elements and continue.
                m_stack_of_open_elements.remove(*node);
                continue;
            }

            // 6. Create an element for the token for which the element node was created,
            //    in the HTML namespace, with common ancestor as the intended parent;
            // FIXME: hold onto the real token
            auto element = create_element_for(HTMLToken::make_start_tag(node->local_name()), Namespace::HTML, *common_ancestor);
            // replace the entry for node in the list of active formatting elements with an entry for the new element,
            m_list_of_active_formatting_elements.replace(*node, *element);
            // replace the entry for node in the stack of open elements with an entry for the new element,
            m_stack_of_open_elements.replace(*node, element);
            // and let node be the new element.
            node = element;

            // 7. If last node is furthest block,
            if (last_node == furthest_block) {
                // then move the aforementioned bookmark to be immediately after the new node in the list of active formatting elements.
                bookmark = m_list_of_active_formatting_elements.find_index(*node).value() + 1;
            }

            // 8. Append last node to node.
            MUST(node->append_child(*last_node));

            // 9. Set last node to node.
            last_node = node;
        }

        // 14. Insert whatever last node ended up being in the previous step at the appropriate place for inserting a node,
        //     but using common ancestor as the override target.
        auto adjusted_insertion_location = find_appropriate_place_for_inserting_node(common_ancestor);
        adjusted_insertion_location.parent->insert_before(*last_node, adjusted_insertion_location.insert_before_sibling, false);

        // 15. Create an element for the token for which formatting element was created,
        //     in the HTML namespace, with furthest block as the intended parent.
        // FIXME: hold onto the real token
        auto element = create_element_for(HTMLToken::make_start_tag(formatting_element->local_name()), Namespace::HTML, *furthest_block);

        // 16. Take all of the child nodes of furthest block and append them to the element created in the last step.
        for (auto& child : furthest_block->children_as_vector())
            MUST(element->append_child(furthest_block->remove_child(*child).release_value()));

        // 17. Append that new element to furthest block.
        MUST(furthest_block->append_child(*element));

        // 18. Remove formatting element from the list of active formatting elements,
        //     and insert the new element into the list of active formatting elements at the position of the aforementioned bookmark.
        auto formatting_element_index = m_list_of_active_formatting_elements.find_index(*formatting_element);
        if (formatting_element_index.has_value() && formatting_element_index.value() < bookmark)
            bookmark--;
        m_list_of_active_formatting_elements.remove(*formatting_element);
        m_list_of_active_formatting_elements.insert_at(bookmark, *element);

        // 19. Remove formatting element from the stack of open elements, and insert the new element
        //     into the stack of open elements immediately below the position of furthest block in that stack.
        m_stack_of_open_elements.remove(*formatting_element);
        m_stack_of_open_elements.insert_immediately_below(*element, *furthest_block);
    }
}

// https://html.spec.whatwg.org/multipage/parsing.html#special
bool HTMLParser::is_special_tag(FlyString const& tag_name, Optional<FlyString> const& namespace_)
{
    if (namespace_ == Namespace::HTML) {
        return tag_name.is_one_of(
            HTML::TagNames::address,
            HTML::TagNames::applet,
            HTML::TagNames::area,
            HTML::TagNames::article,
            HTML::TagNames::aside,
            HTML::TagNames::base,
            HTML::TagNames::basefont,
            HTML::TagNames::bgsound,
            HTML::TagNames::blockquote,
            HTML::TagNames::body,
            HTML::TagNames::br,
            HTML::TagNames::button,
            HTML::TagNames::caption,
            HTML::TagNames::center,
            HTML::TagNames::col,
            HTML::TagNames::colgroup,
            HTML::TagNames::dd,
            HTML::TagNames::details,
            HTML::TagNames::dir,
            HTML::TagNames::div,
            HTML::TagNames::dl,
            HTML::TagNames::dt,
            HTML::TagNames::embed,
            HTML::TagNames::fieldset,
            HTML::TagNames::figcaption,
            HTML::TagNames::figure,
            HTML::TagNames::footer,
            HTML::TagNames::form,
            HTML::TagNames::frame,
            HTML::TagNames::frameset,
            HTML::TagNames::h1,
            HTML::TagNames::h2,
            HTML::TagNames::h3,
            HTML::TagNames::h4,
            HTML::TagNames::h5,
            HTML::TagNames::h6,
            HTML::TagNames::head,
            HTML::TagNames::header,
            HTML::TagNames::hgroup,
            HTML::TagNames::hr,
            HTML::TagNames::html,
            HTML::TagNames::iframe,
            HTML::TagNames::img,
            HTML::TagNames::input,
            HTML::TagNames::keygen,
            HTML::TagNames::li,
            HTML::TagNames::link,
            HTML::TagNames::listing,
            HTML::TagNames::main,
            HTML::TagNames::marquee,
            HTML::TagNames::menu,
            HTML::TagNames::meta,
            HTML::TagNames::nav,
            HTML::TagNames::noembed,
            HTML::TagNames::noframes,
            HTML::TagNames::noscript,
            HTML::TagNames::object,
            HTML::TagNames::ol,
            HTML::TagNames::p,
            HTML::TagNames::param,
            HTML::TagNames::plaintext,
            HTML::TagNames::pre,
            HTML::TagNames::script,
            HTML::TagNames::section,
            HTML::TagNames::select,
            HTML::TagNames::source,
            HTML::TagNames::style,
            HTML::TagNames::summary,
            HTML::TagNames::table,
            HTML::TagNames::tbody,
            HTML::TagNames::td,
            HTML::TagNames::template_,
            HTML::TagNames::textarea,
            HTML::TagNames::tfoot,
            HTML::TagNames::th,
            HTML::TagNames::thead,
            HTML::TagNames::title,
            HTML::TagNames::tr,
            HTML::TagNames::track,
            HTML::TagNames::ul,
            HTML::TagNames::wbr,
            HTML::TagNames::xmp);
    } else if (namespace_ == Namespace::SVG) {
        return tag_name.is_one_of(
            SVG::TagNames::desc,
            SVG::TagNames::foreignObject,
            SVG::TagNames::title);
    } else if (namespace_ == Namespace::MathML) {
        return tag_name.is_one_of(
            MathML::TagNames::mi,
            MathML::TagNames::mo,
            MathML::TagNames::mn,
            MathML::TagNames::mtext,
            MathML::TagNames::annotation_xml);
    }

    return false;
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody
void HTMLParser::handle_in_body(HTMLToken& token)
{
    if (token.is_character()) {
        // -> A character token that is U+0000 NULL
        if (token.code_point() == 0) {
            // Parse error. Ignore the token.
            log_parse_error();
            return;
        }

        // -> A character token that is one of U+0009 CHARACTER TABULATION, U+000A LINE FEED (LF), U+000C FORM FEED (FF), U+000D CARRIAGE RETURN (CR), or U+0020 SPACE
        if (token.is_parser_whitespace()) {
            // Reconstruct the active formatting elements, if any.
            reconstruct_the_active_formatting_elements();

            // Insert the token's character.
            insert_character(token.code_point());
            return;
        }

        // -> Any other character token

        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert the token's character.
        insert_character(token.code_point());

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;
        return;
    }

    // -> A comment token
    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    // -> A DOCTYPE token
    if (token.is_doctype()) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // -> A start tag whose tag name is "html"`
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        // Parse error.
        log_parse_error();

        // If there is a template element on the stack of open elements, then ignore the token.
        if (m_stack_of_open_elements.contains(HTML::TagNames::template_))
            return;

        // Otherwise, for each attribute on the token, check to see if the attribute is already present on the top element of the stack of open elements.
        // If it is not, add the attribute and its corresponding value to that element.
        token.for_each_attribute([&](auto& attribute) {
            if (!current_node().has_attribute(attribute.local_name))
                MUST(current_node().set_attribute(attribute.local_name, attribute.value));
            return IterationDecision::Continue;
        });
        return;
    }

    // -> A start tag whose tag name is one of: "base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::base, HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::link, HTML::TagNames::meta, HTML::TagNames::noframes, HTML::TagNames::script, HTML::TagNames::style, HTML::TagNames::template_, HTML::TagNames::title)) {
        // Process the token using the rules for the "in head" insertion mode.
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    // -> An end tag whose tag name is "template"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::template_) {
        // Process the token using the rules for the "in head" insertion mode.
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    // -> A start tag whose tag name is "body"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::body) {

        // Parse error.
        log_parse_error();

        // If the stack of open elements has only one node on it, if the second element on the stack of open elements is not a body element,
        // or if there is a template element on the stack of open elements, then ignore the token. (fragment case)
        if (m_stack_of_open_elements.elements().size() == 1
            || m_stack_of_open_elements.elements().at(1)->local_name() != HTML::TagNames::body
            || m_stack_of_open_elements.contains(HTML::TagNames::template_)) {
            VERIFY(m_parsing_fragment);
            return;
        }

        // Otherwise, set the frameset-ok flag to "not ok"; then, for each attribute on the token, check to see if the attribute is already
        // present on the body element (the second element) on the stack of open elements, and if it is not, add the attribute and its
        // corresponding value to that element.
        m_frameset_ok = false;
        auto& body_element = m_stack_of_open_elements.elements().at(1);
        token.for_each_attribute([&](auto& attribute) {
            if (!body_element->has_attribute(attribute.local_name))
                MUST(body_element->set_attribute(attribute.local_name, attribute.value));
            return IterationDecision::Continue;
        });
        return;
    }

    // A start tag whose tag name is "frameset"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::frameset) {
        // Parse error.
        log_parse_error();

        // If the stack of open elements has only one node on it, or if the second element on the stack of open elements is not a body element, then ignore the token. (fragment case)
        if (m_stack_of_open_elements.elements().size() == 1
            || m_stack_of_open_elements.elements().at(1)->local_name() != HTML::TagNames::body) {
            VERIFY(m_parsing_fragment);
            return;
        }

        // If the frameset-ok flag is set to "not ok", ignore the token.
        if (!m_frameset_ok)
            return;

        // FIXME: Otherwise, run the following steps:
        // 1. Remove the second element on the stack of open elements from its parent node, if it has one.
        // 2. Pop all the nodes from the bottom of the stack of open elements, from the current node up to, but not including, the root html element.
        // 3. Insert an HTML element for the token.
        // 4. Switch the insertion mode to "in frameset".
        TODO();
    }

    // -> An end-of-file token
    if (token.is_end_of_file()) {
        // If the stack of template insertion modes is not empty, then process the token using the rules for the "in template" insertion mode.
        if (!m_stack_of_template_insertion_modes.is_empty()) {
            process_using_the_rules_for(InsertionMode::InTemplate, token);
            return;
        }

        // Otherwise, follow these steps:
        // 1. If there is a node in the stack of open elements that is not either a dd element, a dt element, an li element, an optgroup element,
        //    an option element, a p element, an rb element, an rp element, an rt element, an rtc element, a tbody element, a td element, a tfoot
        //    element, a th element, a thead element, a tr element, the body element, or the html element, then this is a parse error.
        for (auto& node : m_stack_of_open_elements.elements()) {
            if (!node->local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt, HTML::TagNames::li, HTML::TagNames::optgroup, HTML::TagNames::option, HTML::TagNames::p, HTML::TagNames::rb, HTML::TagNames::rp, HTML::TagNames::rt, HTML::TagNames::rtc, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr, HTML::TagNames::body, HTML::TagNames::html)) {
                log_parse_error();
                break;
            }
        }

        // 2. Stop parsing.
        stop_parsing();
        return;
    }

    // -> An end tag whose tag name is "body"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::body) {
        // If the stack of open elements does not have a body element in scope, this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_scope(HTML::TagNames::body)) {
            log_parse_error();
            return;
        }

        // Otherwise, if there is a node in the stack of open elements that is not either a dd element, a dt element, an li element, an optgroup element,
        // an option element, a p element, an rb element, an rp element, an rt element, an rtc element, a tbody element, a td element, a tfoot element, a
        // th element, a thead element, a tr element, the body element, or the html element, then this is a parse error.
        for (auto& node : m_stack_of_open_elements.elements()) {
            if (!node->local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt, HTML::TagNames::li, HTML::TagNames::optgroup, HTML::TagNames::option, HTML::TagNames::p, HTML::TagNames::rb, HTML::TagNames::rp, HTML::TagNames::rt, HTML::TagNames::rtc, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr, HTML::TagNames::body, HTML::TagNames::html)) {
                log_parse_error();
                break;
            }
        }

        // Switch the insertion mode to "after body".
        m_insertion_mode = InsertionMode::AfterBody;
        return;
    }

    // -> An end tag whose tag name is "html"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::html) {
        // If the stack of open elements does not have a body element in scope, this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_scope(HTML::TagNames::body)) {
            log_parse_error();
            return;
        }

        // Otherwise, if there is a node in the stack of open elements that is not either a dd element, a dt element, an li element, an optgroup element, an
        // option element, a p element, an rb element, an rp element, an rt element, an rtc element, a tbody element, a td element, a tfoot element, a th element,
        // a thead element, a tr element, the body element, or the html element, then this is a parse error.
        for (auto& node : m_stack_of_open_elements.elements()) {
            if (!node->local_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt, HTML::TagNames::li, HTML::TagNames::optgroup, HTML::TagNames::option, HTML::TagNames::p, HTML::TagNames::rb, HTML::TagNames::rp, HTML::TagNames::rt, HTML::TagNames::rtc, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr, HTML::TagNames::body, HTML::TagNames::html)) {
                log_parse_error();
                break;
            }
        }

        // Switch the insertion mode to "after body".
        m_insertion_mode = InsertionMode::AfterBody;

        // Reprocess the token.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // -> A start tag whose tag name is one of: "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav", "ol", "p", "search", "section", "summary", "ul"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::address, HTML::TagNames::article, HTML::TagNames::aside, HTML::TagNames::blockquote, HTML::TagNames::center, HTML::TagNames::details, HTML::TagNames::dialog, HTML::TagNames::dir, HTML::TagNames::div, HTML::TagNames::dl, HTML::TagNames::fieldset, HTML::TagNames::figcaption, HTML::TagNames::figure, HTML::TagNames::footer, HTML::TagNames::header, HTML::TagNames::hgroup, HTML::TagNames::main, HTML::TagNames::menu, HTML::TagNames::nav, HTML::TagNames::ol, HTML::TagNames::p, HTML::TagNames::section, HTML::TagNames::summary, HTML::TagNames::ul)) {
        // If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is one of: "h1", "h2", "h3", "h4", "h5", "h6"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6)) {
        // If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // If the current node is an HTML element whose tag name is one of "h1", "h2", "h3", "h4", "h5", or "h6", then this is a parse error; pop the current node off the stack of open elements.
        if (current_node().local_name().is_one_of(HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6)) {
            log_parse_error();
            (void)m_stack_of_open_elements.pop();
        }

        // Insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is one of: "pre", "listing"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::pre, HTML::TagNames::listing)) {
        // If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);

        // If the next token is a U+000A LINE FEED (LF) character token,
        // then ignore that token and move on to the next one.
        // (Newlines at the start of pre blocks are ignored as an authoring convenience.)
        auto next_token = m_tokenizer.next_token();
        if (next_token.has_value() && next_token.value().is_character() && next_token.value().code_point() == '\n') {
            // Ignore it.
        } else {
            process_using_the_rules_for(m_insertion_mode, next_token.value());
        }

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;
        return;
    }

    // -> A start tag whose tag name is "form"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::form) {
        // If the form element pointer is not null, and there is no template element on the stack of open elements, then this is a parse error; ignore the token.
        if (m_form_element.ptr() && !m_stack_of_open_elements.contains(HTML::TagNames::template_)) {
            log_parse_error();
            return;
        }

        // Otherwise:
        // If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // Insert an HTML element for the token, and, if there is no template element on the stack of open elements, set the form element pointer to point to the element created.
        auto element = insert_html_element(token);
        if (!m_stack_of_open_elements.contains(HTML::TagNames::template_))
            m_form_element = JS::make_handle(verify_cast<HTMLFormElement>(*element));
        return;
    }

    // -> A start tag whose tag name is "li"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::li) {
        // 1. Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // 2. Initialize node to be the current node (the bottommost node of the stack).
        // 3. Loop: If node is an li element, then run these substeps:
        for (ssize_t i = m_stack_of_open_elements.elements().size() - 1; i >= 0; --i) {
            JS::GCPtr<DOM::Element> node = m_stack_of_open_elements.elements()[i].ptr();
            if (node->local_name() == HTML::TagNames::li) {
                // 1. Generate implied end tags, except for li elements.
                generate_implied_end_tags(HTML::TagNames::li);

                // 2. If the current node is not an li element, then this is a parse error.
                if (current_node().local_name() != HTML::TagNames::li) {
                    log_parse_error();
                }

                // 3. Pop elements from the stack of open elements until an li element has been popped from the stack.
                m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::li);

                // 4. Jump to the step labeled done below.
                break;
            }

            // 4. If node is in the special category, but is not an address, div, or p element, then jump to the step labeled done below.
            if (is_special_tag(node->local_name(), node->namespace_uri()) && !node->local_name().is_one_of(HTML::TagNames::address, HTML::TagNames::div, HTML::TagNames::p))
                break;

            // 5. Otherwise, set node to the previous entry in the stack of open elements and return to the step labeled loop.
        }

        // 6. Done: If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // 7. Finally, insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is one of: "dd", "dt"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt)) {
        // 1. Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // 2. Initialize node to be the current node (the bottommost node of the stack).
        // 3. Loop: If node is a dd element, then run these substeps:
        // 4. If node is a dt element, then run these substeps:
        for (ssize_t i = m_stack_of_open_elements.elements().size() - 1; i >= 0; --i) {
            // 1. Generate implied end tags, except for dd elements.
            JS::GCPtr<DOM::Element> node = m_stack_of_open_elements.elements()[i].ptr();
            if (node->local_name() == HTML::TagNames::dd) {
                generate_implied_end_tags(HTML::TagNames::dd);
                // 2. If the current node is not a dd element, then this is a parse error.
                if (current_node().local_name() != HTML::TagNames::dd) {
                    log_parse_error();
                }

                // 3. Pop elements from the stack of open elements until a dd element has been popped from the stack.
                m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::dd);

                // 4. Jump to the step labeled done below.
                break;
            }

            // 1. Generate implied end tags, except for dt elements.
            if (node->local_name() == HTML::TagNames::dt) {
                // 2. If the current node is not a dt element, then this is a parse error.
                generate_implied_end_tags(HTML::TagNames::dt);
                if (current_node().local_name() != HTML::TagNames::dt) {
                    log_parse_error();
                }
                // 3. Pop elements from the stack of open elements until a dt element has been popped from the stack.
                m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::dt);

                // 4. Jump to the step labeled done below.
                break;
            }

            // 5. If node is in the special category, but is not an address, div, or p element, then jump to the step labeled done below.
            if (is_special_tag(node->local_name(), node->namespace_uri()) && !node->local_name().is_one_of(HTML::TagNames::address, HTML::TagNames::div, HTML::TagNames::p))
                break;

            // 6. Otherwise, set node to the previous entry in the stack of open elements and return to the step labeled loop.
        }

        // 7: Done: If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // 8: Finally, insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is "plaintext"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::plaintext) {
        // If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);

        // Switch the tokenizer to the PLAINTEXT state.
        m_tokenizer.switch_to({}, HTMLTokenizer::State::PLAINTEXT);
        return;
    }

    // -> A start tag whose tag name is "button"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::button) {
        // 1. If the stack of open elements has a button element in scope, then run these substeps:
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::button)) {
            // 1. Parse error.
            log_parse_error();

            // 2. Generate implied end tags.
            generate_implied_end_tags();

            // 3. Pop elements from the stack of open elements until a button element has been popped from the stack.
            m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::button);
        }

        // 2. Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // 3. Insert an HTML element for the token.
        (void)insert_html_element(token);

        // 4. Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;
        return;
    }

    // -> An end tag whose tag name is one of: "address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "listing", "main", "menu", "nav", "ol", "pre", "search", "section", "summary", "ul"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::address, HTML::TagNames::article, HTML::TagNames::aside, HTML::TagNames::blockquote, HTML::TagNames::button, HTML::TagNames::center, HTML::TagNames::details, HTML::TagNames::dialog, HTML::TagNames::dir, HTML::TagNames::div, HTML::TagNames::dl, HTML::TagNames::fieldset, HTML::TagNames::figcaption, HTML::TagNames::figure, HTML::TagNames::footer, HTML::TagNames::header, HTML::TagNames::hgroup, HTML::TagNames::listing, HTML::TagNames::main, HTML::TagNames::menu, HTML::TagNames::nav, HTML::TagNames::ol, HTML::TagNames::pre, HTML::TagNames::section, HTML::TagNames::summary, HTML::TagNames::ul)) {
        // If the stack of open elements does not have an element in scope that is an HTML element with the same tag name as that of the token, then this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_scope(token.tag_name())) {
            log_parse_error();
            return;
        }

        // Otherwise, run these steps:
        // 1. Generate implied end tags.
        generate_implied_end_tags();

        // 2. If the current node is not an HTML element with the same tag name as that of the token, then this is a parse error.
        if (current_node().local_name() != token.tag_name()) {
            log_parse_error();
        }

        // 3. Pop elements from the stack of open elements until an HTML element with the same tag name as the token has been popped from the stack.
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(token.tag_name());
        return;
    }

    // -> An end tag whose tag name is "form"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::form) {
        // If there is no template element on the stack of open elements, then run these substeps:
        if (!m_stack_of_open_elements.contains(HTML::TagNames::template_)) {
            // 1. Let node be the element that the form element pointer is set to, or null if it is not set to an element.
            auto node = m_form_element;

            // 2. Set the form element pointer to null.
            m_form_element = {};

            // 3. If node is null or if the stack of open elements does not have node in scope, then this is a parse error; return and ignore the token.
            if (!node || !m_stack_of_open_elements.has_in_scope(*node)) {
                log_parse_error();
                return;
            }

            // 4. Generate implied end tags.
            generate_implied_end_tags();

            // 5. If the current node is not node, then this is a parse error.
            if (&current_node() != node.ptr()) {
                log_parse_error();
            }

            // 6. Remove node from the stack of open elements.
            m_stack_of_open_elements.elements().remove_first_matching([&](auto& entry) { return entry.ptr() == node.ptr(); });
        }
        // If there is a template element on the stack of open elements, then run these substeps instead:
        else {
            // 1. If the stack of open elements does not have a form element in scope, then this is a parse error; return and ignore the token.
            if (!m_stack_of_open_elements.has_in_scope(HTML::TagNames::form)) {
                log_parse_error();
                return;
            }

            // 2. Generate implied end tags.
            generate_implied_end_tags();

            // 3. If the current node is not a form element, then this is a parse error.
            if (current_node().local_name() != HTML::TagNames::form) {
                log_parse_error();
            }

            // 4. Pop elements from the stack of open elements until a form element has been popped from the stack.
            m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::form);
        }
        return;
    }

    // -> An end tag whose tag name is "p"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::p) {
        // If the stack of open elements does not have a p element in button scope, then this is a parse error; insert an HTML element for a "p" start tag token with no attributes.
        if (!m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p)) {
            log_parse_error();
            (void)insert_html_element(HTMLToken::make_start_tag(HTML::TagNames::p));
        }

        // Close a p element.
        close_a_p_element();
        return;
    }

    // -> An end tag whose tag name is "li"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::li) {
        // If the stack of open elements does not have an li element in list item scope, then this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_list_item_scope(HTML::TagNames::li)) {
            log_parse_error();
            return;
        }

        // Otherwise, run these steps:
        // 1. Generate implied end tags, except for li elements.
        generate_implied_end_tags(HTML::TagNames::li);

        // 2. If the current node is not an li element, then this is a parse error.
        if (current_node().local_name() != HTML::TagNames::li) {
            log_parse_error();
            dbgln("Expected <li> current node, but had <{}>", current_node().local_name());
        }

        // 3. Pop elements from the stack of open elements until an li element has been popped from the stack.
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::li);
        return;
    }

    // -> An end tag whose tag name is one of: "dd", "dt"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::dd, HTML::TagNames::dt)) {
        // If the stack of open elements does not have an element in scope that is an HTML element with the same tag name as that of the token, then this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_scope(token.tag_name())) {
            log_parse_error();
            return;
        }

        // Otherwise, run these steps:
        // 1. Generate implied end tags, except for HTML elements with the same tag name as the token.
        generate_implied_end_tags(token.tag_name());

        // 2. If the current node is not an HTML element with the same tag name as that of the token, then this is a parse error.
        if (current_node().local_name() != token.tag_name()) {
            log_parse_error();
        }

        // 3. Pop elements from the stack of open elements until an HTML element with the same tag name as the token has been popped from the stack.
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(token.tag_name());
        return;
    }

    // 3. An end tag whose tag name is one of: "h1", "h2", "h3", "h4", "h5", "h6"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6)) {
        // If the stack of open elements does not have an element in scope that is an HTML element and whose tag name is one of "h1", "h2", "h3", "h4", "h5", or "h6", then this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_scope(HTML::TagNames::h1)
            && !m_stack_of_open_elements.has_in_scope(HTML::TagNames::h2)
            && !m_stack_of_open_elements.has_in_scope(HTML::TagNames::h3)
            && !m_stack_of_open_elements.has_in_scope(HTML::TagNames::h4)
            && !m_stack_of_open_elements.has_in_scope(HTML::TagNames::h5)
            && !m_stack_of_open_elements.has_in_scope(HTML::TagNames::h6)) {
            log_parse_error();
            return;
        }

        // Otherwise, run these steps:
        // 1. Generate implied end tags.
        generate_implied_end_tags();

        // 2. If the current node is not an HTML element with the same tag name as that of the token, then this is a parse error.
        if (current_node().local_name() != token.tag_name()) {
            log_parse_error();
        }

        // 3. Pop elements from the stack of open elements until an HTML element whose tag name is one of "h1", "h2", "h3", "h4", "h5", or "h6" has been popped from the stack.
        for (;;) {
            auto popped_element = m_stack_of_open_elements.pop();
            if (popped_element->local_name().is_one_of(HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6))
                break;
        }
        return;
    }

    // -> A start tag whose tag name is "a"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::a) {
        // If the list of active formatting elements contains an a element between the end of the list and the last marker on the list (or the start of the list if there
        // is no marker on the list), then this is a parse error; run the adoption agency algorithm for the token, then remove that element from the list of active formatting
        // elements and the stack of open elements if the adoption agency algorithm didn't already remove it (it might not have if the element is not in table scope).
        if (auto* element = m_list_of_active_formatting_elements.last_element_with_tag_name_before_marker(HTML::TagNames::a)) {
            log_parse_error();
            if (run_the_adoption_agency_algorithm(token) == AdoptionAgencyAlgorithmOutcome::RunAnyOtherEndTagSteps)
                goto AnyOtherEndTag;
            m_list_of_active_formatting_elements.remove(*element);
            m_stack_of_open_elements.elements().remove_first_matching([&](auto& entry) {
                return entry.ptr() == element;
            });
        }

        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token. Push onto the list of active formatting elements that element.
        auto element = insert_html_element(token);
        m_list_of_active_formatting_elements.add(*element);
        return;
    }

    // -> A start tag whose tag name is one of: "b", "big", "code", "em", "font", "i", "s", "small", "strike", "strong", "tt", "u"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::b, HTML::TagNames::big, HTML::TagNames::code, HTML::TagNames::em, HTML::TagNames::font, HTML::TagNames::i, HTML::TagNames::s, HTML::TagNames::small, HTML::TagNames::strike, HTML::TagNames::strong, HTML::TagNames::tt, HTML::TagNames::u)) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token. Push onto the list of active formatting elements that element.
        auto element = insert_html_element(token);
        m_list_of_active_formatting_elements.add(*element);
        return;
    }

    // -> A start tag whose tag name is "nobr"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::nobr) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // If the stack of open elements has a nobr element in scope, then this is a parse error; run the adoption agency algorithm for the token, then once again reconstruct the active formatting elements, if any.
        if (m_stack_of_open_elements.has_in_scope(HTML::TagNames::nobr)) {
            log_parse_error();
            run_the_adoption_agency_algorithm(token);
            reconstruct_the_active_formatting_elements();
        }

        // Insert an HTML element for the token. Push onto the list of active formatting elements that element.
        auto element = insert_html_element(token);
        m_list_of_active_formatting_elements.add(*element);
        return;
    }

    // -> An end tag whose tag name is one of: "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::a, HTML::TagNames::b, HTML::TagNames::big, HTML::TagNames::code, HTML::TagNames::em, HTML::TagNames::font, HTML::TagNames::i, HTML::TagNames::nobr, HTML::TagNames::s, HTML::TagNames::small, HTML::TagNames::strike, HTML::TagNames::strong, HTML::TagNames::tt, HTML::TagNames::u)) {
        // Run the adoption agency algorithm for the token.
        if (run_the_adoption_agency_algorithm(token) == AdoptionAgencyAlgorithmOutcome::RunAnyOtherEndTagSteps)
            goto AnyOtherEndTag;
        return;
    }

    // -> A start tag whose tag name is one of: "applet", "marquee", "object"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::applet, HTML::TagNames::marquee, HTML::TagNames::object)) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);

        // Insert a marker at the end of the list of active formatting elements.
        m_list_of_active_formatting_elements.add_marker();

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;
        return;
    }

    // -> An end tag token whose tag name is one of: "applet", "marquee", "object"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::applet, HTML::TagNames::marquee, HTML::TagNames::object)) {
        // If the stack of open elements does not have an element in scope that is an HTML element with the same tag name as that of the token, then this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_scope(token.tag_name())) {
            log_parse_error();
            return;
        }

        // Otherwise, run these steps:
        // 1. Generate implied end tags.
        generate_implied_end_tags();

        // 2. If the current node is not an HTML element with the same tag name as that of the token, then this is a parse error.
        if (current_node().local_name() != token.tag_name()) {
            log_parse_error();
        }

        // 3. Pop elements from the stack of open elements until an HTML element with the same tag name as the token has been popped from the stack.
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(token.tag_name());

        // 4. Clear the list of active formatting elements up to the last marker.
        m_list_of_active_formatting_elements.clear_up_to_the_last_marker();
        return;
    }

    // -> A start tag whose tag name is "table"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::table) {

        // If the Document is not set to quirks mode, and the stack of open elements has a p element in button scope, then close a p element.
        if (!document().in_quirks_mode()) {
            if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
                close_a_p_element();
        }

        // Insert an HTML element for the token.
        (void)insert_html_element(token);

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // Switch the insertion mode to "in table".`
        m_insertion_mode = InsertionMode::InTable;
        return;
    }

    // -> An end tag whose tag name is "br"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::br) {
        // Parse error. Drop the attributes from the token, and act as described in the next entry; i.e. act as if this was a "br" start tag token with no attributes, rather than the end tag token that it actually is.
        log_parse_error();
        token.drop_attributes();
        goto BRStartTag;
    }

    // -> A start tag whose tag name is one of: "area", "br", "embed", "img", "keygen", "wbr"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::area, HTML::TagNames::br, HTML::TagNames::embed, HTML::TagNames::img, HTML::TagNames::keygen, HTML::TagNames::wbr)) {
    BRStartTag:
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token. Immediately pop the current node off the stack of open elements.
        (void)insert_html_element(token);

        // Acknowledge the token's self-closing flag, if it is set.
        (void)m_stack_of_open_elements.pop();

        // Acknowledge the token's self-closing flag, if it is set.
        token.acknowledge_self_closing_flag_if_set();

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;
        return;
    }

    // -> A start tag whose tag name is "input"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::input) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token. Immediately pop the current node off the stack of open elements.
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();

        // Acknowledge the token's self-closing flag, if it is set.
        token.acknowledge_self_closing_flag_if_set();

        // If the token does not have an attribute with the name "type", or if it does, but that attribute's value is not an ASCII case-insensitive match for the string "hidden", then: set the frameset-ok flag to "not ok".
        auto type_attribute = token.attribute(HTML::AttributeNames::type);
        if (!type_attribute.has_value() || !type_attribute->equals_ignoring_ascii_case("hidden"sv)) {
            m_frameset_ok = false;
        }
        return;
    }

    // -> A start tag whose tag name is one of: "param", "source", "track"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::param, HTML::TagNames::source, HTML::TagNames::track)) {
        // Insert an HTML element for the token. Immediately pop the current node off the stack of open elements.
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();

        // Acknowledge the token's self-closing flag, if it is set.
        token.acknowledge_self_closing_flag_if_set();
        return;
    }

    // -> A start tag whose tag name is "hr"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::hr) {
        // If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p))
            close_a_p_element();

        // Insert an HTML element for the token. Immediately pop the current node off the stack of open elements.
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();

        // Acknowledge the token's self-closing flag, if it is set.
        token.acknowledge_self_closing_flag_if_set();

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;
        return;
    }

    // -> A start tag whose tag name is "image"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::image) {
        // Parse error. Change the token's tag name to HTML::TagNames::img and reprocess it. (Don't ask.)
        log_parse_error();
        token.set_tag_name("img"_fly_string);
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // -> A start tag whose tag name is "textarea"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::textarea) {
        // 1. Insert an HTML element for the token.
        (void)insert_html_element(token);

        // FIXME: 2. If the next token is a U+000A LINE FEED (LF) character token, then ignore that token and move on to the next one. (Newlines at the start of textarea elements are ignored as an authoring convenience.)

        // 3. Switch the tokenizer to the RCDATA state.
        m_tokenizer.switch_to({}, HTMLTokenizer::State::RCDATA);

        // If the next token is a U+000A LINE FEED (LF) character token,
        // then ignore that token and move on to the next one.
        // (Newlines at the start of pre blocks are ignored as an authoring convenience.)
        auto next_token = m_tokenizer.next_token();

        // 4. Let the original insertion mode be the current insertion mode.
        m_original_insertion_mode = m_insertion_mode;

        // 5. Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // 6. Switch the insertion mode to "text".
        m_insertion_mode = InsertionMode::Text;

        // FIXME: This step is not in the spec.
        if (next_token.has_value() && next_token.value().is_character() && next_token.value().code_point() == '\n') {
            // Ignore it.
        } else {
            process_using_the_rules_for(m_insertion_mode, next_token.value());
        }
        return;
    }

    // -> A start tag whose tag name is "xmp"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::xmp) {
        // If the stack of open elements has a p element in button scope, then close a p element.
        if (m_stack_of_open_elements.has_in_button_scope(HTML::TagNames::p)) {
            close_a_p_element();
        }

        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // Follow the generic raw text element parsing algorithm.
        parse_generic_raw_text_element(token);
        return;
    }

    // -> A start tag whose tag name is "iframe"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::iframe) {
        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // Follow the generic raw text element parsing algorithm.
        parse_generic_raw_text_element(token);
        return;
    }

    // -> A start tag whose tag name is "noembed"
    // -> A start tag whose tag name is "noscript", if the scripting flag is enabled
    if (token.is_start_tag() && ((token.tag_name() == HTML::TagNames::noembed) || (token.tag_name() == HTML::TagNames::noscript && m_scripting_enabled))) {
        // Follow the generic raw text element parsing algorithm.
        parse_generic_raw_text_element(token);
        return;
    }

    // -> A start tag whose tag name is "select"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::select) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);

        // Set the frameset-ok flag to "not ok".
        m_frameset_ok = false;

        // If the insertion mode is one of "in table", "in caption", "in table body", "in row", or "in cell", then switch the insertion mode to "in select in table". Otherwise, switch the insertion mode to "in select".
        switch (m_insertion_mode) {
        case InsertionMode::InTable:
        case InsertionMode::InCaption:
        case InsertionMode::InTableBody:
        case InsertionMode::InRow:
        case InsertionMode::InCell:
            m_insertion_mode = InsertionMode::InSelectInTable;
            break;
        default:
            m_insertion_mode = InsertionMode::InSelect;
            break;
        }
        return;
    }

    // -> A start tag whose tag name is one of: "optgroup", "option"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::optgroup, HTML::TagNames::option)) {
        // If the current node is an option element, then pop the current node off the stack of open elements.
        if (current_node().local_name() == HTML::TagNames::option)
            (void)m_stack_of_open_elements.pop();

        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is one of: "rb", "rtc"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::rb, HTML::TagNames::rtc)) {
        // If the stack of open elements has a ruby element in scope, then generate implied end tags. If the current node is not now a ruby element, this is a parse error.
        if (m_stack_of_open_elements.has_in_scope(HTML::TagNames::ruby))
            generate_implied_end_tags();
        if (current_node().local_name() != HTML::TagNames::ruby)
            log_parse_error();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is one of: "rp", "rt"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::rp, HTML::TagNames::rt)) {
        // If the stack of open elements has a ruby element in scope, then generate implied end tags, except for rtc elements. If the current node is not now a rtc element or a ruby element, this is a parse error.
        if (m_stack_of_open_elements.has_in_scope(HTML::TagNames::ruby))
            generate_implied_end_tags(HTML::TagNames::rtc);
        if (current_node().local_name() != HTML::TagNames::rtc || current_node().local_name() != HTML::TagNames::ruby)
            log_parse_error();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is "math"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::math) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Adjust MathML attributes for the token. (This fixes the case of MathML attributes that are not all lowercase.)
        adjust_mathml_attributes(token);

        // Adjust foreign attributes for the token. (This fixes the use of namespaced attributes, in particular XLink.)
        adjust_foreign_attributes(token);

        // Insert a foreign element for the token, with MathML namespace and false.
        (void)insert_foreign_element(token, Namespace::MathML, OnlyAddToElementStack::No);

        // If the token has its self-closing flag set, pop the current node off the stack of open elements and acknowledge the token's self-closing flag.
        if (token.is_self_closing()) {
            (void)m_stack_of_open_elements.pop();
            token.acknowledge_self_closing_flag_if_set();
        }
        return;
    }

    // -> A start tag whose tag name is "svg"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::svg) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Adjust SVG attributes for the token. (This fixes the case of SVG attributes that are not all lowercase.)
        adjust_svg_attributes(token);

        // Adjust foreign attributes for the token. (This fixes the use of namespaced attributes, in particular XLink in SVG.)
        adjust_foreign_attributes(token);

        // FIXME: We are not setting the 'onlyAddToElementStack' flag here.
        // Insert a foreign element for the token, with SVG namespace and false.
        (void)insert_foreign_element(token, Namespace::SVG, OnlyAddToElementStack::No);

        // If the token has its self-closing flag set, pop the current node off the stack of open elements and acknowledge the token's self-closing flag.
        if (token.is_self_closing()) {
            (void)m_stack_of_open_elements.pop();
            token.acknowledge_self_closing_flag_if_set();
        }
        return;
    }

    // -> A start tag whose tag name is one of: "caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr"
    if ((token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::frame, HTML::TagNames::head, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr))) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // -> Any other start tag
    if (token.is_start_tag()) {
        // Reconstruct the active formatting elements, if any.
        reconstruct_the_active_formatting_elements();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);
        return;
    }

    // -> Any other end tag
    if (token.is_end_tag()) {
    AnyOtherEndTag:
        // 1. Initialize node to be the current node (the bottommost node of the stack).
        JS::GCPtr<DOM::Element> node;

        // 2. Loop: If node is an HTML element with the same tag name as the token, then:
        for (ssize_t i = m_stack_of_open_elements.elements().size() - 1; i >= 0; --i) {
            node = m_stack_of_open_elements.elements()[i].ptr();
            if (node->local_name() == token.tag_name()) {
                // 1. Generate implied end tags, except for HTML elements with the same tag name as the token.
                generate_implied_end_tags(token.tag_name());

                // 2. If node is not the current node, then this is a parse error.
                if (node.ptr() != &current_node()) {
                    log_parse_error();
                }

                // 3. Pop all the nodes from the current node up to node, including node, then stop these steps.
                while (&current_node() != node.ptr()) {
                    (void)m_stack_of_open_elements.pop();
                }
                (void)m_stack_of_open_elements.pop();
                break;
            }

            // 3. Otherwise, if node is in the special category, then this is a parse error; ignore the token, and return.
            if (is_special_tag(node->local_name(), node->namespace_uri())) {
                log_parse_error();
                return;
            }

            // 4. Set node to the previous entry in the stack of open elements.
            // 5. Return to the step labeled loop.
        }
        return;
    }
}

void HTMLParser::adjust_mathml_attributes(HTMLToken& token)
{
    token.adjust_attribute_name("definitionurl"_fly_string, "definitionURL"_fly_string);
}

void HTMLParser::adjust_svg_tag_names(HTMLToken& token)
{
    struct TagNameAdjustment {
        FlyString from;
        FlyString to;
    };

    static TagNameAdjustment adjustments[] = {
        { "altglyph"_fly_string, "altGlyph"_fly_string },
        { "altglyphdef"_fly_string, "altGlyphDef"_fly_string },
        { "altglyphitem"_fly_string, "altGlyphItem"_fly_string },
        { "animatecolor"_fly_string, "animateColor"_fly_string },
        { "animatemotion"_fly_string, "animateMotion"_fly_string },
        { "animatetransform"_fly_string, "animateTransform"_fly_string },
        { "clippath"_fly_string, "clipPath"_fly_string },
        { "feblend"_fly_string, "feBlend"_fly_string },
        { "fecolormatrix"_fly_string, "feColorMatrix"_fly_string },
        { "fecomponenttransfer"_fly_string, "feComponentTransfer"_fly_string },
        { "fecomposite"_fly_string, "feComposite"_fly_string },
        { "feconvolvematrix"_fly_string, "feConvolveMatrix"_fly_string },
        { "fediffuselighting"_fly_string, "feDiffuseLighting"_fly_string },
        { "fedisplacementmap"_fly_string, "feDisplacementMap"_fly_string },
        { "fedistantlight"_fly_string, "feDistantLight"_fly_string },
        { "fedropshadow"_fly_string, "feDropShadow"_fly_string },
        { "feflood"_fly_string, "feFlood"_fly_string },
        { "fefunca"_fly_string, "feFuncA"_fly_string },
        { "fefuncb"_fly_string, "feFuncB"_fly_string },
        { "fefuncg"_fly_string, "feFuncG"_fly_string },
        { "fefuncr"_fly_string, "feFuncR"_fly_string },
        { "fegaussianblur"_fly_string, "feGaussianBlur"_fly_string },
        { "feimage"_fly_string, "feImage"_fly_string },
        { "femerge"_fly_string, "feMerge"_fly_string },
        { "femergenode"_fly_string, "feMergeNode"_fly_string },
        { "femorphology"_fly_string, "feMorphology"_fly_string },
        { "feoffset"_fly_string, "feOffset"_fly_string },
        { "fepointlight"_fly_string, "fePointLight"_fly_string },
        { "fespecularlighting"_fly_string, "feSpecularLighting"_fly_string },
        { "fespotlight"_fly_string, "feSpotlight"_fly_string },
        { "foreignobject"_fly_string, "foreignObject"_fly_string },
        { "glyphref"_fly_string, "glyphRef"_fly_string },
        { "lineargradient"_fly_string, "linearGradient"_fly_string },
        { "radialgradient"_fly_string, "radialGradient"_fly_string },
        { "textpath"_fly_string, "textPath"_fly_string },
    };

    for (auto const& adjustment : adjustments) {
        token.adjust_tag_name(adjustment.from, adjustment.to);
    }
}

void HTMLParser::adjust_svg_attributes(HTMLToken& token)
{
    struct AttributeAdjustment {
        FlyString from;
        FlyString to;
    };

    static AttributeAdjustment adjustments[] = {
        { "attributename"_fly_string, "attributeName"_fly_string },
        { "attributetype"_fly_string, "attributeType"_fly_string },
        { "basefrequency"_fly_string, "baseFrequency"_fly_string },
        { "baseprofile"_fly_string, "baseProfile"_fly_string },
        { "calcmode"_fly_string, "calcMode"_fly_string },
        { "clippathunits"_fly_string, "clipPathUnits"_fly_string },
        { "diffuseconstant"_fly_string, "diffuseConstant"_fly_string },
        { "edgemode"_fly_string, "edgeMode"_fly_string },
        { "filterunits"_fly_string, "filterUnits"_fly_string },
        { "glyphref"_fly_string, "glyphRef"_fly_string },
        { "gradienttransform"_fly_string, "gradientTransform"_fly_string },
        { "gradientunits"_fly_string, "gradientUnits"_fly_string },
        { "kernelmatrix"_fly_string, "kernelMatrix"_fly_string },
        { "kernelunitlength"_fly_string, "kernelUnitLength"_fly_string },
        { "keypoints"_fly_string, "keyPoints"_fly_string },
        { "keysplines"_fly_string, "keySplines"_fly_string },
        { "keytimes"_fly_string, "keyTimes"_fly_string },
        { "lengthadjust"_fly_string, "lengthAdjust"_fly_string },
        { "limitingconeangle"_fly_string, "limitingConeAngle"_fly_string },
        { "markerheight"_fly_string, "markerHeight"_fly_string },
        { "markerunits"_fly_string, "markerUnits"_fly_string },
        { "markerwidth"_fly_string, "markerWidth"_fly_string },
        { "maskcontentunits"_fly_string, "maskContentUnits"_fly_string },
        { "maskunits"_fly_string, "maskUnits"_fly_string },
        { "numoctaves"_fly_string, "numOctaves"_fly_string },
        { "pathlength"_fly_string, "pathLength"_fly_string },
        { "patterncontentunits"_fly_string, "patternContentUnits"_fly_string },
        { "patterntransform"_fly_string, "patternTransform"_fly_string },
        { "patternunits"_fly_string, "patternUnits"_fly_string },
        { "pointsatx"_fly_string, "pointsAtX"_fly_string },
        { "pointsaty"_fly_string, "pointsAtY"_fly_string },
        { "pointsatz"_fly_string, "pointsAtZ"_fly_string },
        { "preservealpha"_fly_string, "preserveAlpha"_fly_string },
        { "preserveaspectratio"_fly_string, "preserveAspectRatio"_fly_string },
        { "primitiveunits"_fly_string, "primitiveUnits"_fly_string },
        { "refx"_fly_string, "refX"_fly_string },
        { "refy"_fly_string, "refY"_fly_string },
        { "repeatcount"_fly_string, "repeatCount"_fly_string },
        { "repeatdur"_fly_string, "repeatDur"_fly_string },
        { "requiredextensions"_fly_string, "requiredExtensions"_fly_string },
        { "requiredfeatures"_fly_string, "requiredFeatures"_fly_string },
        { "specularconstant"_fly_string, "specularConstant"_fly_string },
        { "specularexponent"_fly_string, "specularExponent"_fly_string },
        { "spreadmethod"_fly_string, "spreadMethod"_fly_string },
        { "startoffset"_fly_string, "startOffset"_fly_string },
        { "stddeviation"_fly_string, "stdDeviation"_fly_string },
        { "stitchtiles"_fly_string, "stitchTiles"_fly_string },
        { "surfacescale"_fly_string, "surfaceScale"_fly_string },
        { "systemlanguage"_fly_string, "systemLanguage"_fly_string },
        { "tablevalues"_fly_string, "tableValues"_fly_string },
        { "targetx"_fly_string, "targetX"_fly_string },
        { "targety"_fly_string, "targetY"_fly_string },
        { "textlength"_fly_string, "textLength"_fly_string },
        { "viewbox"_fly_string, "viewBox"_fly_string },
        { "viewtarget"_fly_string, "viewTarget"_fly_string },
        { "xchannelselector"_fly_string, "xChannelSelector"_fly_string },
        { "ychannelselector"_fly_string, "yChannelSelector"_fly_string },
        { "zoomandpan"_fly_string, "zoomAndPan"_fly_string },
    };

    for (auto const& adjustment : adjustments) {
        token.adjust_attribute_name(adjustment.from, adjustment.to);
    }
}

// https://html.spec.whatwg.org/multipage/parsing.html#adjust-foreign-attributes
void HTMLParser::adjust_foreign_attributes(HTMLToken& token)
{
    struct ForeignAttributeAdjustment {
        FlyString attribute_name;
        Optional<FlyString> prefix;
        FlyString local_name;
        FlyString namespace_;
    };

    static ForeignAttributeAdjustment adjustments[] = {
        { "xlink:actuate"_fly_string, "xlink"_fly_string, "actuate"_fly_string, Namespace::XLink },
        { "xlink:arcrole"_fly_string, "xlink"_fly_string, "arcrole"_fly_string, Namespace::XLink },
        { "xlink:href"_fly_string, "xlink"_fly_string, "href"_fly_string, Namespace::XLink },
        { "xlink:role"_fly_string, "xlink"_fly_string, "role"_fly_string, Namespace::XLink },
        { "xlink:show"_fly_string, "xlink"_fly_string, "show"_fly_string, Namespace::XLink },
        { "xlink:title"_fly_string, "xlink"_fly_string, "title"_fly_string, Namespace::XLink },
        { "xlink:type"_fly_string, "xlink"_fly_string, "type"_fly_string, Namespace::XLink },
        { "xml:lang"_fly_string, "xml"_fly_string, "lang"_fly_string, Namespace::XML },
        { "xml:space"_fly_string, "xml"_fly_string, "space"_fly_string, Namespace::XML },
        { "xmlns"_fly_string, {}, "xmlns"_fly_string, Namespace::XMLNS },
        { "xmlns:xlink"_fly_string, "xmlns"_fly_string, "xlink"_fly_string, Namespace::XMLNS },
    };

    for (auto const& adjustment : adjustments) {
        token.adjust_foreign_attribute(adjustment.attribute_name, adjustment.prefix, adjustment.local_name, adjustment.namespace_);
    }
}

void HTMLParser::increment_script_nesting_level()
{
    ++m_script_nesting_level;
}

void HTMLParser::decrement_script_nesting_level()
{
    VERIFY(m_script_nesting_level);
    --m_script_nesting_level;
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-incdata
void HTMLParser::handle_text(HTMLToken& token)
{
    if (token.is_character()) {
        insert_character(token.code_point());
        return;
    }
    if (token.is_end_of_file()) {
        log_parse_error();
        if (current_node().local_name() == HTML::TagNames::script)
            verify_cast<HTMLScriptElement>(current_node()).set_already_started(Badge<HTMLParser> {}, true);
        (void)m_stack_of_open_elements.pop();
        m_insertion_mode = m_original_insertion_mode;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // -> An end tag whose tag name is "script"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::script) {
        // FIXME: If the active speculative HTML parser is null and the JavaScript execution context stack is empty, then perform a microtask checkpoint.

        // Non-standard: Make sure the <script> element has up-to-date text content before preparing the script.
        flush_character_insertions();

        // If the active speculative HTML parser is null and the JavaScript execution context stack is empty, then perform a microtask checkpoint.
        // FIXME: If the active speculative HTML parser is null
        auto& vm = main_thread_event_loop().vm();
        if (vm.execution_context_stack().is_empty())
            perform_a_microtask_checkpoint();

        // Let script be the current node (which will be a script element).
        JS::NonnullGCPtr<HTMLScriptElement> script = verify_cast<HTMLScriptElement>(current_node());

        // Pop the current node off the stack of open elements.
        (void)m_stack_of_open_elements.pop();

        // Switch the insertion mode to the original insertion mode.
        m_insertion_mode = m_original_insertion_mode;

        // Let the old insertion point have the same value as the current insertion point.
        m_tokenizer.store_insertion_point();

        // Let the insertion point be just before the next input character.
        m_tokenizer.update_insertion_point();

        // Increment the parser's script nesting level by one.
        increment_script_nesting_level();

        // If the active speculative HTML parser is null, then prepare the script element script.
        // This might cause some script to execute, which might cause new characters to be inserted into the tokenizer,
        // and might cause the tokenizer to output more tokens, resulting in a reentrant invocation of the parser.
        // FIXME: Check if active speculative HTML parser is null.
        script->prepare_script(Badge<HTMLParser> {});

        // Decrement the parser's script nesting level by one.
        decrement_script_nesting_level();

        // If the parser's script nesting level is zero, then set the parser pause flag to false.
        if (script_nesting_level() == 0)
            m_parser_pause_flag = false;

        // Let the insertion point have the value of the old insertion point.
        m_tokenizer.restore_insertion_point();

        // At this stage, if the pending parsing-blocking script is not null, then:
        if (document().pending_parsing_blocking_script()) {
            // -> If the script nesting level is not zero:
            if (script_nesting_level() != 0) {
                // Set the parser pause flag to true,
                m_parser_pause_flag = true;
                // and abort the processing of any nested invocations of the tokenizer, yielding control back to the caller.
                // (Tokenization will resume when the caller returns to the "outer" tree construction stage.)
                return;
            }

            // Otherwise:
            else {
                // While the pending parsing-blocking script is not null:
                while (document().pending_parsing_blocking_script()) {
                    // 1. Let the script be the pending parsing-blocking script.
                    // 2. Set the pending parsing-blocking script to null.
                    auto the_script = document().take_pending_parsing_blocking_script({});

                    // FIXME: 3. Start the speculative HTML parser for this instance of the HTML parser.

                    // 4. Block the tokenizer for this instance of the HTML parser, such that the event loop will not run tasks that invoke the tokenizer.
                    m_tokenizer.set_blocked(true);

                    // 5. If the parser's Document has a style sheet that is blocking scripts
                    //    or the script's ready to be parser-executed is false:
                    if (m_document->has_a_style_sheet_that_is_blocking_scripts() || the_script->is_ready_to_be_parser_executed() == false) {
                        // spin the event loop until the parser's Document has no style sheet that is blocking scripts
                        // and the script's ready to be parser-executed becomes true.
                        main_thread_event_loop().spin_until([&] {
                            return !m_document->has_a_style_sheet_that_is_blocking_scripts() && the_script->is_ready_to_be_parser_executed();
                        });
                    }

                    // 6. If this parser has been aborted in the meantime, return.
                    if (m_aborted)
                        return;

                    // FIXME: 7. Stop the speculative HTML parser for this instance of the HTML parser.

                    // 8. Unblock the tokenizer for this instance of the HTML parser, such that tasks that invoke the tokenizer can again be run.
                    m_tokenizer.set_blocked(false);

                    // 9. Let the insertion point be just before the next input character.
                    m_tokenizer.update_insertion_point();

                    // 10. Increment the parser's script nesting level by one (it should be zero before this step, so this sets it to one).
                    VERIFY(script_nesting_level() == 0);
                    increment_script_nesting_level();

                    // 11. Execute the script element the script.
                    the_script->execute_script();

                    // 12. Decrement the parser's script nesting level by one.
                    decrement_script_nesting_level();

                    // If the parser's script nesting level is zero (which it always should be at this point), then set the parser pause flag to false.
                    VERIFY(script_nesting_level() == 0);
                    m_parser_pause_flag = false;

                    // 13. Let the insertion point be undefined again.
                    m_tokenizer.undefine_insertion_point();
                }
            }
        }

        return;
    }

    if (token.is_end_tag()) {
        (void)m_stack_of_open_elements.pop();
        m_insertion_mode = m_original_insertion_mode;
        return;
    }
    TODO();
}

void HTMLParser::clear_the_stack_back_to_a_table_context()
{
    while (!current_node().local_name().is_one_of(HTML::TagNames::table, HTML::TagNames::template_, HTML::TagNames::html))
        (void)m_stack_of_open_elements.pop();

    if (current_node().local_name() == HTML::TagNames::html)
        VERIFY(m_parsing_fragment);
}

void HTMLParser::clear_the_stack_back_to_a_table_row_context()
{
    while (!current_node().local_name().is_one_of(HTML::TagNames::tr, HTML::TagNames::template_, HTML::TagNames::html))
        (void)m_stack_of_open_elements.pop();

    if (current_node().local_name() == HTML::TagNames::html)
        VERIFY(m_parsing_fragment);
}

void HTMLParser::clear_the_stack_back_to_a_table_body_context()
{
    while (!current_node().local_name().is_one_of(HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::template_, HTML::TagNames::html))
        (void)m_stack_of_open_elements.pop();

    if (current_node().local_name() == HTML::TagNames::html)
        VERIFY(m_parsing_fragment);
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intr
void HTMLParser::handle_in_row(HTMLToken& token)
{
    // A start tag whose tag name is one of: "th", "td"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::th, HTML::TagNames::td)) {
        // Clear the stack back to a table row context.
        clear_the_stack_back_to_a_table_row_context();

        // Insert an HTML element for the token, then switch the insertion mode to "in cell".
        (void)insert_html_element(token);
        m_insertion_mode = InsertionMode::InCell;

        // Insert a marker at the end of the list of active formatting elements.
        m_list_of_active_formatting_elements.add_marker();
        return;
    }

    // An end tag whose tag name is "tr"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::tr) {
        // If the stack of open elements does not have a tr element in table scope, this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::tr)) {
            log_parse_error();
            return;
        }

        // Otherwise:
        // Clear the stack back to a table row context.
        clear_the_stack_back_to_a_table_row_context();

        // Pop the current node (which will be a tr element) from the stack of open elements.
        (void)m_stack_of_open_elements.pop();

        // Switch the insertion mode to "in table body".
        m_insertion_mode = InsertionMode::InTableBody;
        return;
    }

    // A start tag whose tag name is one of: "caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"
    // An end tag whose tag name is "table"
    if ((token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::tr))
        || (token.is_end_tag() && token.tag_name() == HTML::TagNames::table)) {

        // If the stack of open elements does not have a tr element in table scope, this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::tr)) {
            log_parse_error();
            return;
        }

        // Otherwise:
        // Clear the stack back to a table row context.
        clear_the_stack_back_to_a_table_row_context();

        // Pop the current node (which will be a tr element) from the stack of open elements.
        (void)m_stack_of_open_elements.pop();

        // Switch the insertion mode to "in table body".
        m_insertion_mode = InsertionMode::InTableBody;

        // Reprocess the token.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // An end tag whose tag name is one of: "tbody", "tfoot", "thead"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead)) {
        // If the stack of open elements does not have an element in table scope that is an HTML element with the same tag name as the token, this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_table_scope(token.tag_name())) {
            log_parse_error();
            return;
        }

        // If the stack of open elements does not have a tr element in table scope, ignore the token.
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::tr)) {
            return;
        }

        // Otherwise:
        // Clear the stack back to a table row context.
        clear_the_stack_back_to_a_table_row_context();

        // Pop the current node (which will be a tr element) from the stack of open elements.
        (void)m_stack_of_open_elements.pop();

        // Switch the insertion mode to "in table body".
        m_insertion_mode = InsertionMode::InTableBody;

        // Reprocess the token.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // An end tag whose tag name is one of: "body", "caption", "col", "colgroup", "html", "td", "th"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::body, HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::html, HTML::TagNames::td, HTML::TagNames::th)) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // Anything else:
    // Process the token using the rules for the "in table" insertion mode.
    process_using_the_rules_for(InsertionMode::InTable, token);
}

void HTMLParser::close_the_cell()
{
    generate_implied_end_tags();
    if (!current_node().local_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th)) {
        log_parse_error();
    }
    while (!current_node().local_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th))
        (void)m_stack_of_open_elements.pop();
    (void)m_stack_of_open_elements.pop();
    m_list_of_active_formatting_elements.clear_up_to_the_last_marker();
    m_insertion_mode = InsertionMode::InRow;
}

void HTMLParser::handle_in_cell(HTMLToken& token)
{
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th)) {
        if (!m_stack_of_open_elements.has_in_table_scope(token.tag_name())) {
            log_parse_error();
            return;
        }
        generate_implied_end_tags();

        if (current_node().local_name() != token.tag_name()) {
            log_parse_error();
        }

        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(token.tag_name());

        m_list_of_active_formatting_elements.clear_up_to_the_last_marker();

        m_insertion_mode = InsertionMode::InRow;
        return;
    }
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr)) {
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::td) && !m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::th)) {
            VERIFY(m_parsing_fragment);
            log_parse_error();
            return;
        }
        close_the_cell();
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::body, HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::html)) {
        log_parse_error();
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::table, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::tr)) {
        if (!m_stack_of_open_elements.has_in_table_scope(token.tag_name())) {
            log_parse_error();
            return;
        }
        close_the_cell();
        // Reprocess the token.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    process_using_the_rules_for(InsertionMode::InBody, token);
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intabletext
void HTMLParser::handle_in_table_text(HTMLToken& token)
{
    if (token.is_character()) {
        // A character token that is U+0000 NULL
        if (token.code_point() == 0) {
            // Parse error. Ignore the token.
            log_parse_error();
            return;
        }
        // Any other character token
        // Append the character token to the pending table character tokens list.
        m_pending_table_character_tokens.append(move(token));
        return;
    }

    // Anything else

    // If any of the tokens in the pending table character tokens list
    // are character tokens that are not ASCII whitespace, then this is a parse error:
    // reprocess the character tokens in the pending table character tokens list using
    // the rules given in the "anything else" entry in the "in table" insertion mode.
    if (any_of(m_pending_table_character_tokens, [](auto const& token) { return !token.is_parser_whitespace(); })) {
        log_parse_error();
        for (auto& pending_token : m_pending_table_character_tokens) {
            m_foster_parenting = true;
            process_using_the_rules_for(InsertionMode::InBody, pending_token);
            m_foster_parenting = false;
        }
    } else {
        // Otherwise, insert the characters given by the pending table character tokens list.
        for (auto& pending_token : m_pending_table_character_tokens) {
            insert_character(pending_token.code_point());
        }
    }

    // Switch the insertion mode to the original insertion mode and reprocess the token.
    m_insertion_mode = m_original_insertion_mode;
    process_using_the_rules_for(m_insertion_mode, token);
}

void HTMLParser::handle_in_table_body(HTMLToken& token)
{
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::tr) {
        clear_the_stack_back_to_a_table_body_context();
        (void)insert_html_element(token);
        m_insertion_mode = InsertionMode::InRow;
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::th, HTML::TagNames::td)) {
        log_parse_error();
        clear_the_stack_back_to_a_table_body_context();
        (void)insert_html_element(HTMLToken::make_start_tag(HTML::TagNames::tr));
        m_insertion_mode = InsertionMode::InRow;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead)) {
        if (!m_stack_of_open_elements.has_in_table_scope(token.tag_name())) {
            log_parse_error();
            return;
        }
        clear_the_stack_back_to_a_table_body_context();
        (void)m_stack_of_open_elements.pop();
        m_insertion_mode = InsertionMode::InTable;
        return;
    }

    if ((token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead))
        || (token.is_end_tag() && token.tag_name() == HTML::TagNames::table)) {

        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::tbody)
            && !m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::thead)
            && !m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::tfoot)) {
            log_parse_error();
            return;
        }

        clear_the_stack_back_to_a_table_body_context();
        (void)m_stack_of_open_elements.pop();
        m_insertion_mode = InsertionMode::InTable;
        process_using_the_rules_for(InsertionMode::InTable, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::body, HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::html, HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::tr)) {
        log_parse_error();
        return;
    }

    process_using_the_rules_for(InsertionMode::InTable, token);
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intable
void HTMLParser::handle_in_table(HTMLToken& token)
{
    // A character token, if the current node is table, tbody, template, tfoot, thead, or tr element
    if (token.is_character() && current_node().local_name().is_one_of(HTML::TagNames::table, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::tr)) {
        // Let the pending table character tokens be an empty list of tokens.
        m_pending_table_character_tokens.clear();

        // Let the original insertion mode be the current insertion mode.
        m_original_insertion_mode = m_insertion_mode;

        // Switch the insertion mode to "in table text" and reprocess the token.
        m_insertion_mode = InsertionMode::InTableText;
        process_using_the_rules_for(InsertionMode::InTableText, token);
        return;
    }

    // A comment token
    if (token.is_comment()) {
        // Insert a comment.
        insert_comment(token);
        return;
    }
    // A DOCTYPE token
    if (token.is_doctype()) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // A start tag whose tag name is "caption"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::caption) {
        // Clear the stack back to a table context.
        clear_the_stack_back_to_a_table_context();

        // Insert a marker at the end of the list of active formatting elements.
        m_list_of_active_formatting_elements.add_marker();

        // Insert an HTML element for the token, then switch the insertion mode to "in caption".
        (void)insert_html_element(token);
        m_insertion_mode = InsertionMode::InCaption;
        return;
    }

    // A start tag whose tag name is "colgroup"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::colgroup) {
        // Clear the stack back to a table context.
        clear_the_stack_back_to_a_table_context();

        // Insert an HTML element for the token, then switch the insertion mode to "in column group".
        (void)insert_html_element(token);
        m_insertion_mode = InsertionMode::InColumnGroup;
        return;
    }

    // A start tag whose tag name is "col"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::col) {
        // Clear the stack back to a table context.
        clear_the_stack_back_to_a_table_context();

        // Insert an HTML element for a "colgroup" start tag token with no attributes, then switch the insertion mode to "in column group".
        (void)insert_html_element(HTMLToken::make_start_tag(HTML::TagNames::colgroup));
        m_insertion_mode = InsertionMode::InColumnGroup;

        // Reprocess the current token.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // A start tag whose tag name is one of: "tbody", "tfoot", "thead"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead)) {
        // Clear the stack back to a table context.
        clear_the_stack_back_to_a_table_context();

        // Insert an HTML element for the token, then switch the insertion mode to "in table body".
        (void)insert_html_element(token);
        m_insertion_mode = InsertionMode::InTableBody;
        return;
    }

    // A start tag whose tag name is one of: "td", "th", "tr"
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::tr)) {
        // Clear the stack back to a table context.
        clear_the_stack_back_to_a_table_context();

        // Insert an HTML element for a "tbody" start tag token with no attributes, then switch the insertion mode to "in table body".
        (void)insert_html_element(HTMLToken::make_start_tag(HTML::TagNames::tbody));
        m_insertion_mode = InsertionMode::InTableBody;

        // Reprocess the current token.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // A start tag whose tag name is "table"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::table) {
        // Parse error.
        log_parse_error();

        // If the stack of open elements does not have a table element in table scope, ignore the token.
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::table))
            return;

        // Otherwise:
        // Pop elements from this stack until a table element has been popped from the stack.
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::table);

        // Reset the insertion mode appropriately.
        reset_the_insertion_mode_appropriately();

        // Reprocess the token.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // An end tag whose tag name is "table"
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::table) {
        // If the stack of open elements does not have a table element in table scope, this is a parse error; ignore the token.
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::table)) {
            log_parse_error();
            return;
        }

        // Otherwise:
        // Pop elements from this stack until a table element has been popped from the stack.
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::table);

        // Reset the insertion mode appropriately.
        reset_the_insertion_mode_appropriately();
        return;
    }

    // An end tag whose tag name is one of: "body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"
    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::body, HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::html, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr)) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // A start tag whose tag name is one of: "style", "script", "template"
    // An end tag whose tag name is "template"
    if ((token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::style, HTML::TagNames::script, HTML::TagNames::template_))
        || (token.is_end_tag() && token.tag_name() == HTML::TagNames::template_)) {
        // Process the token using the rules for the "in head" insertion mode.
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    // A start tag whose tag name is "input"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::input) {
        // If the token does not have an attribute with the name "type",
        // or if it does, but that attribute's value is not an ASCII case-insensitive match for the string "hidden",
        // then: act as described in the "anything else" entry below.
        auto type_attribute = token.attribute(HTML::AttributeNames::type);
        if (!type_attribute.has_value() || !type_attribute->equals_ignoring_ascii_case("hidden"sv)) {
            goto AnythingElse;
        }

        // Otherwise:
        // Parse error.
        log_parse_error();

        // Insert an HTML element for the token.
        (void)insert_html_element(token);

        // Pop that input element off the stack of open elements.
        (void)m_stack_of_open_elements.pop();

        // Acknowledge the token's self-closing flag, if it is set.
        token.acknowledge_self_closing_flag_if_set();
        return;
    }

    // A start tag whose tag name is "form"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::form) {
        // Parse error.
        log_parse_error();

        // If there is a template element on the stack of open elements,
        // or if the form element pointer is not null, ignore the token.
        if (m_form_element.ptr() || m_stack_of_open_elements.contains(HTML::TagNames::template_)) {
            return;
        }

        // Otherwise:
        // Insert an HTML element for the token, and set the form element pointer to point to the element created.
        m_form_element = JS::make_handle(verify_cast<HTMLFormElement>(*insert_html_element(token)));

        // Pop that form element off the stack of open elements.
        (void)m_stack_of_open_elements.pop();
        return;
    }

    // An end-of-file token
    if (token.is_end_of_file()) {
        // Process the token using the rules for the "in body" insertion mode.
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

AnythingElse:
    // Anything else

    // Parse error.
    log_parse_error();

    // Enable foster parenting, process the token using the rules for the "in body" insertion mode, and then disable foster parenting.
    m_foster_parenting = true;
    process_using_the_rules_for(InsertionMode::InBody, token);
    m_foster_parenting = false;
}

void HTMLParser::handle_in_select_in_table(HTMLToken& token)
{
    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::table, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::tr, HTML::TagNames::td, HTML::TagNames::th)) {
        log_parse_error();
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::select);
        reset_the_insertion_mode_appropriately();
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::table, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead, HTML::TagNames::tr, HTML::TagNames::td, HTML::TagNames::th)) {
        log_parse_error();

        if (!m_stack_of_open_elements.has_in_table_scope(token.tag_name()))
            return;

        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::select);
        reset_the_insertion_mode_appropriately();
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    process_using_the_rules_for(InsertionMode::InSelect, token);
}

void HTMLParser::handle_in_select(HTMLToken& token)
{
    if (token.is_character()) {
        if (token.code_point() == 0) {
            log_parse_error();
            return;
        }
        insert_character(token.code_point());
        return;
    }

    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::option) {
        if (current_node().local_name() == HTML::TagNames::option) {
            (void)m_stack_of_open_elements.pop();
        }
        (void)insert_html_element(token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::optgroup) {
        if (current_node().local_name() == HTML::TagNames::option) {
            (void)m_stack_of_open_elements.pop();
        }
        if (current_node().local_name() == HTML::TagNames::optgroup) {
            (void)m_stack_of_open_elements.pop();
        }
        (void)insert_html_element(token);
        return;
    }

    // -> A start tag whose tag name is "hr"
    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::hr) {
        // If the current node is an option element, pop that node from the stack of open elements.
        if (current_node().local_name() == HTML::TagNames::option) {
            (void)m_stack_of_open_elements.pop();
        }
        // If the current node is an optgroup element, pop that node from the stack of open elements.
        if (current_node().local_name() == HTML::TagNames::optgroup) {
            (void)m_stack_of_open_elements.pop();
        }
        // Insert an HTML element for the token. Immediately pop the current node off the stack of open elements.
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();
        // Acknowledge the token's self-closing flag, if it is set.
        token.acknowledge_self_closing_flag_if_set();
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::optgroup) {
        if (current_node().local_name() == HTML::TagNames::option && node_before_current_node().local_name() == HTML::TagNames::optgroup)
            (void)m_stack_of_open_elements.pop();

        if (current_node().local_name() == HTML::TagNames::optgroup) {
            (void)m_stack_of_open_elements.pop();
        } else {
            log_parse_error();
            return;
        }
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::option) {
        if (current_node().local_name() == HTML::TagNames::option) {
            (void)m_stack_of_open_elements.pop();
        } else {
            log_parse_error();
            return;
        }
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::select) {
        if (!m_stack_of_open_elements.has_in_select_scope(HTML::TagNames::select)) {
            VERIFY(m_parsing_fragment);
            log_parse_error();
            return;
        }
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::select);
        reset_the_insertion_mode_appropriately();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::select) {
        log_parse_error();

        if (!m_stack_of_open_elements.has_in_select_scope(HTML::TagNames::select)) {
            VERIFY(m_parsing_fragment);
            return;
        }

        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::select);
        reset_the_insertion_mode_appropriately();
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::input, HTML::TagNames::keygen, HTML::TagNames::textarea)) {
        log_parse_error();

        if (!m_stack_of_open_elements.has_in_select_scope(HTML::TagNames::select)) {
            VERIFY(m_parsing_fragment);
            return;
        }

        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::select);
        reset_the_insertion_mode_appropriately();
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::script, HTML::TagNames::template_)) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::template_) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_of_file()) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    log_parse_error();
}

void HTMLParser::handle_in_caption(HTMLToken& token)
{
    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::caption) {
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::caption)) {
            VERIFY(m_parsing_fragment);
            log_parse_error();
            return;
        }

        generate_implied_end_tags();

        if (current_node().local_name() != HTML::TagNames::caption)
            log_parse_error();

        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::caption);
        m_list_of_active_formatting_elements.clear_up_to_the_last_marker();

        m_insertion_mode = InsertionMode::InTable;
        return;
    }

    if ((token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr))
        || (token.is_end_tag() && token.tag_name() == HTML::TagNames::table)) {
        if (!m_stack_of_open_elements.has_in_table_scope(HTML::TagNames::caption)) {
            VERIFY(m_parsing_fragment);
            log_parse_error();
            return;
        }

        generate_implied_end_tags();

        if (current_node().local_name() != HTML::TagNames::caption)
            log_parse_error();

        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::caption);
        m_list_of_active_formatting_elements.clear_up_to_the_last_marker();

        m_insertion_mode = InsertionMode::InTable;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::body, HTML::TagNames::col, HTML::TagNames::colgroup, HTML::TagNames::html, HTML::TagNames::tbody, HTML::TagNames::td, HTML::TagNames::tfoot, HTML::TagNames::th, HTML::TagNames::thead, HTML::TagNames::tr)) {
        log_parse_error();
        return;
    }

    process_using_the_rules_for(InsertionMode::InBody, token);
}

void HTMLParser::handle_in_column_group(HTMLToken& token)
{
    if (token.is_character() && token.is_parser_whitespace()) {
        insert_character(token.code_point());
        return;
    }

    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::col) {
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();
        token.acknowledge_self_closing_flag_if_set();
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::colgroup) {
        if (current_node().local_name() != HTML::TagNames::colgroup) {
            log_parse_error();
            return;
        }

        (void)m_stack_of_open_elements.pop();
        m_insertion_mode = InsertionMode::InTable;
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::col) {
        log_parse_error();
        return;
    }

    if ((token.is_start_tag() || token.is_end_tag()) && token.tag_name() == HTML::TagNames::template_) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_of_file()) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (current_node().local_name() != HTML::TagNames::colgroup) {
        log_parse_error();
        return;
    }

    (void)m_stack_of_open_elements.pop();
    m_insertion_mode = InsertionMode::InTable;
    process_using_the_rules_for(m_insertion_mode, token);
}

void HTMLParser::handle_in_template(HTMLToken& token)
{
    if (token.is_character() || token.is_comment() || token.is_doctype()) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::base, HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::link, HTML::TagNames::meta, HTML::TagNames::noframes, HTML::TagNames::script, HTML::TagNames::style, HTML::TagNames::template_, HTML::TagNames::title)) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::template_) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::caption, HTML::TagNames::colgroup, HTML::TagNames::tbody, HTML::TagNames::tfoot, HTML::TagNames::thead)) {
        m_stack_of_template_insertion_modes.take_last();
        m_stack_of_template_insertion_modes.append(InsertionMode::InTable);
        m_insertion_mode = InsertionMode::InTable;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::col) {
        m_stack_of_template_insertion_modes.take_last();
        m_stack_of_template_insertion_modes.append(InsertionMode::InColumnGroup);
        m_insertion_mode = InsertionMode::InColumnGroup;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::tr) {
        m_stack_of_template_insertion_modes.take_last();
        m_stack_of_template_insertion_modes.append(InsertionMode::InTableBody);
        m_insertion_mode = InsertionMode::InTableBody;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th)) {
        m_stack_of_template_insertion_modes.take_last();
        m_stack_of_template_insertion_modes.append(InsertionMode::InRow);
        m_insertion_mode = InsertionMode::InRow;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_start_tag()) {
        m_stack_of_template_insertion_modes.take_last();
        m_stack_of_template_insertion_modes.append(InsertionMode::InBody);
        m_insertion_mode = InsertionMode::InBody;
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    if (token.is_end_tag()) {
        log_parse_error();
        return;
    }

    if (token.is_end_of_file()) {
        if (!m_stack_of_open_elements.contains(HTML::TagNames::template_)) {
            VERIFY(m_parsing_fragment);
            stop_parsing();
            return;
        }

        log_parse_error();
        m_stack_of_open_elements.pop_until_an_element_with_tag_name_has_been_popped(HTML::TagNames::template_);
        m_list_of_active_formatting_elements.clear_up_to_the_last_marker();
        m_stack_of_template_insertion_modes.take_last();
        reset_the_insertion_mode_appropriately();
        process_using_the_rules_for(m_insertion_mode, token);
    }
}

void HTMLParser::handle_in_frameset(HTMLToken& token)
{
    if (token.is_character() && token.is_parser_whitespace()) {
        insert_character(token.code_point());
        return;
    }

    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::frameset) {
        (void)insert_html_element(token);
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::frameset) {
        // FIXME: If the current node is the root html element, then this is a parse error; ignore the token. (fragment case)

        (void)m_stack_of_open_elements.pop();

        if (!m_parsing_fragment && current_node().local_name() != HTML::TagNames::frameset) {
            m_insertion_mode = InsertionMode::AfterFrameset;
        }
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::frame) {
        (void)insert_html_element(token);
        (void)m_stack_of_open_elements.pop();
        token.acknowledge_self_closing_flag_if_set();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::noframes) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_of_file()) {
        // FIXME: If the current node is not the root html element, then this is a parse error.

        stop_parsing();
        return;
    }

    log_parse_error();
}

void HTMLParser::handle_after_frameset(HTMLToken& token)
{
    if (token.is_character() && token.is_parser_whitespace()) {
        insert_character(token.code_point());
        return;
    }

    if (token.is_comment()) {
        insert_comment(token);
        return;
    }

    if (token.is_doctype()) {
        log_parse_error();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::html) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_end_tag() && token.tag_name() == HTML::TagNames::html) {
        m_insertion_mode = InsertionMode::AfterAfterFrameset;
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::noframes) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    if (token.is_end_of_file()) {
        stop_parsing();
        return;
    }

    log_parse_error();
}

void HTMLParser::handle_after_after_frameset(HTMLToken& token)
{
    if (token.is_comment()) {
        auto comment = document().heap().allocate<DOM::Comment>(document().realm(), document(), token.comment());
        MUST(document().append_child(comment));
        return;
    }

    if (token.is_doctype() || token.is_parser_whitespace() || (token.is_start_tag() && token.tag_name() == HTML::TagNames::html)) {
        process_using_the_rules_for(InsertionMode::InBody, token);
        return;
    }

    if (token.is_end_of_file()) {
        stop_parsing();
        return;
    }

    if (token.is_start_tag() && token.tag_name() == HTML::TagNames::noframes) {
        process_using_the_rules_for(InsertionMode::InHead, token);
        return;
    }

    log_parse_error();
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inforeign
void HTMLParser::process_using_the_rules_for_foreign_content(HTMLToken& token)
{
    if (token.is_character()) {
        // -> A character token that is U+0000 NULL
        if (token.code_point() == 0) {
            // Parse error. Insert a U+FFFD REPLACEMENT CHARACTER character.
            log_parse_error();
            insert_character(0xFFFD);
            return;
        }

        // -> A character token that is one of U+0009 CHARACTER TABULATION, U+000A LINE FEED (LF), U+000C FORM FEED (FF), U+000D CARRIAGE RETURN (CR), or U+0020 SPAC
        if (token.is_parser_whitespace()) {
            insert_character(token.code_point());
            return;
        }

        // -> Any other character token
        insert_character(token.code_point());
        m_frameset_ok = false;
        return;
    }

    // -> A comment token
    if (token.is_comment()) {
        // Insert a comment.
        insert_comment(token);
        return;
    }

    // -> A DOCTYPE token
    if (token.is_doctype()) {
        // Parse error. Ignore the token.
        log_parse_error();
        return;
    }

    // -> A start tag whose tag name is one of: "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var"
    // -> A start tag whose tag name is "font", if the token has any attributes named "color", "face", or "size"
    // -> An end tag whose tag name is "br", "p"
    if ((token.is_start_tag() && token.tag_name().is_one_of(HTML::TagNames::b, HTML::TagNames::big, HTML::TagNames::blockquote, HTML::TagNames::body, HTML::TagNames::br, HTML::TagNames::center, HTML::TagNames::code, HTML::TagNames::dd, HTML::TagNames::div, HTML::TagNames::dl, HTML::TagNames::dt, HTML::TagNames::em, HTML::TagNames::embed, HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6, HTML::TagNames::head, HTML::TagNames::hr, HTML::TagNames::i, HTML::TagNames::img, HTML::TagNames::li, HTML::TagNames::listing, HTML::TagNames::menu, HTML::TagNames::meta, HTML::TagNames::nobr, HTML::TagNames::ol, HTML::TagNames::p, HTML::TagNames::pre, HTML::TagNames::ruby, HTML::TagNames::s, HTML::TagNames::small, HTML::TagNames::span, HTML::TagNames::strong, HTML::TagNames::strike, HTML::TagNames::sub, HTML::TagNames::sup, HTML::TagNames::table, HTML::TagNames::tt, HTML::TagNames::u, HTML::TagNames::ul, HTML::TagNames::var))
        || (token.is_start_tag() && token.tag_name() == HTML::TagNames::font && (token.has_attribute(HTML::AttributeNames::color) || token.has_attribute(HTML::AttributeNames::face) || token.has_attribute(HTML::AttributeNames::size)))
        || (token.is_end_tag() && token.tag_name().is_one_of(HTML::TagNames::br, HTML::TagNames::p))) {
        // Parse error.
        log_parse_error();

        // While the current node is not a MathML text integration point, an HTML integration point, or an element in the HTML namespace, pop elements from the stack of open elements.
        while (!is_mathml_text_integration_point(current_node())
            && !is_html_integration_point(current_node())
            && current_node().namespace_uri() != Namespace::HTML) {
            (void)m_stack_of_open_elements.pop();
        }

        // Reprocess the token according to the rules given in the section corresponding to the current insertion mode in HTML content.
        process_using_the_rules_for(m_insertion_mode, token);
        return;
    }

    // Any other start tag
    if (token.is_start_tag()) {
        // If the adjusted current node is an element in the MathML namespace, adjust MathML attributes for the token. (This fixes the case of MathML attributes that are not all lowercase.)
        if (adjusted_current_node().namespace_uri() == Namespace::MathML) {
            adjust_mathml_attributes(token);
        }
        // If the adjusted current node is an element in the SVG namespace, and the token's tag name is one of the ones in the first column of the
        // following table, change the tag name to the name given in the corresponding cell in the second column. (This fixes the case of SVG
        // elements that are not all lowercase.)
        else if (adjusted_current_node().namespace_uri() == Namespace::SVG) {
            adjust_svg_tag_names(token);
            // If the adjusted current node is an element in the SVG namespace, adjust SVG attributes for the token. (This fixes the case of SVG attributes that are not all lowercase.)
            adjust_svg_attributes(token);
        }

        // Adjust foreign attributes for the token. (This fixes the use of namespaced attributes, in particular XLink in SVG.)
        adjust_foreign_attributes(token);

        // Insert a foreign element for the token, with adjusted current node's namespace and false.
        (void)insert_foreign_element(token, adjusted_current_node().namespace_uri(), OnlyAddToElementStack::No);

        // If the token has its self-closing flag set, then run the appropriate steps from the following list:
        if (token.is_self_closing()) {

            // -> If the token's tag name is "script", and the new current node is in the SVG namespace
            if (token.tag_name() == SVG::TagNames::script && current_node().namespace_uri() == Namespace::SVG) {
                auto& script_element = verify_cast<SVG::SVGScriptElement>(current_node());
                script_element.set_source_line_number({}, token.start_position().line + 1); // FIXME: This +1 is incorrect for script tags whose script does not start on a new line

                // Acknowledge the token's self-closing flag, and then act as described in the steps for a "script" end tag below.
                token.acknowledge_self_closing_flag_if_set();
                goto ScriptEndTag;
            }
            // -> Otherwise
            else {
                // Pop the current node off the stack of open elements and acknowledge the token's self-closing flag.
                (void)m_stack_of_open_elements.pop();
                token.acknowledge_self_closing_flag_if_set();
            }
        }

        return;
    }

    // -> An end tag whose tag name is "script", if the current node is an SVG script element
    if (token.is_end_tag() && current_node().namespace_uri() == Namespace::SVG && current_node().tag_name() == SVG::TagNames::script) {
    ScriptEndTag:
        // Pop the current node off the stack of open elements.
        auto& script_element = verify_cast<SVG::SVGScriptElement>(*m_stack_of_open_elements.pop());
        // Let the old insertion point have the same value as the current insertion point.
        m_tokenizer.store_insertion_point();
        // Let the insertion point be just before the next input character.
        m_tokenizer.update_insertion_point();
        // Increment the parser's script nesting level by one.
        increment_script_nesting_level();
        // Set the parser pause flag to true.
        m_parser_pause_flag = true;

        // Non-standard: Make sure the <script> element has up-to-date text content before processing the script.
        flush_character_insertions();

        // If the active speculative HTML parser is null and the user agent supports SVG, then Process the SVG script element according to the SVG rules. [SVG]
        // FIXME: If the active speculative HTML parser is null
        script_element.process_the_script_element();

        // Decrement the parser's script nesting level by one.
        decrement_script_nesting_level();
        // If the parser's script nesting level is zero, then set the parser pause flag to false.
        if (script_nesting_level() == 0)
            m_parser_pause_flag = false;

        // Let the insertion point have the value of the old insertion point.
        m_tokenizer.restore_insertion_point();
        return;
    }

    // -> Any other end tag
    if (token.is_end_tag()) {
        // 1. Initialize node to be the current node (the bottommost node of the stack).
        JS::GCPtr<DOM::Element> node = current_node();

        // 2. If node's tag name, converted to ASCII lowercase, is not the same as the tag name of the token, then this is a parse error.
        if (node->tag_name().equals_ignoring_ascii_case(token.tag_name()))
            log_parse_error();

        // 3. Loop: If node is the topmost element in the stack of open elements, then return. (fragment case)
        for (ssize_t i = m_stack_of_open_elements.elements().size() - 1; i >= 0; --i) {
            if (node.ptr() == &m_stack_of_open_elements.first()) {
                VERIFY(m_parsing_fragment);
                return;
            }

            // 4. If node's tag name, converted to ASCII lowercase, is the same as the tag name of the token, pop elements from the stack
            // of open elements until node has been popped from the stack, and then return.
            if (node->tag_name().equals_ignoring_ascii_case(token.tag_name())) {
                while (&current_node() != node.ptr())
                    (void)m_stack_of_open_elements.pop();
                (void)m_stack_of_open_elements.pop();
                return;
            }

            // 5. Set node to the previous entry in the stack of open elements.
            node = m_stack_of_open_elements.elements().at(i - 1).ptr();

            // 6. If node is not an element in the HTML namespace, return to the step labeled loop.
            if (node->namespace_uri() != Namespace::HTML)
                continue;

            // 7. Otherwise, process the token according to the rules given in the section corresponding to the current insertion mode in HTML content.
            process_using_the_rules_for(m_insertion_mode, token);
            return;
        }
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/parsing.html#reset-the-insertion-mode-appropriately
void HTMLParser::reset_the_insertion_mode_appropriately()
{
    for (ssize_t i = m_stack_of_open_elements.elements().size() - 1; i >= 0; --i) {
        bool last = i == 0;
        // NOTE: When parsing fragments, we substitute the context element for the root of the stack of open elements.
        JS::GCPtr<DOM::Element> node;
        if (last && m_parsing_fragment) {
            node = m_context_element.ptr();
        } else {
            node = m_stack_of_open_elements.elements().at(i).ptr();
        }

        if (node->local_name() == HTML::TagNames::select) {
            if (!last) {
                for (ssize_t j = i; j > 0; --j) {
                    auto& ancestor = m_stack_of_open_elements.elements().at(j - 1);

                    if (is<HTMLTemplateElement>(*ancestor))
                        break;

                    if (is<HTMLTableElement>(*ancestor)) {
                        m_insertion_mode = InsertionMode::InSelectInTable;
                        return;
                    }
                }
            }

            m_insertion_mode = InsertionMode::InSelect;
            return;
        }

        if (!last && node->local_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th)) {
            m_insertion_mode = InsertionMode::InCell;
            return;
        }

        if (node->local_name() == HTML::TagNames::tr) {
            m_insertion_mode = InsertionMode::InRow;
            return;
        }

        if (node->local_name().is_one_of(HTML::TagNames::tbody, HTML::TagNames::thead, HTML::TagNames::tfoot)) {
            m_insertion_mode = InsertionMode::InTableBody;
            return;
        }

        if (node->local_name() == HTML::TagNames::caption) {
            m_insertion_mode = InsertionMode::InCaption;
            return;
        }

        if (node->local_name() == HTML::TagNames::colgroup) {
            m_insertion_mode = InsertionMode::InColumnGroup;
            return;
        }

        if (node->local_name() == HTML::TagNames::table) {
            m_insertion_mode = InsertionMode::InTable;
            return;
        }

        if (node->local_name() == HTML::TagNames::template_) {
            m_insertion_mode = m_stack_of_template_insertion_modes.last();
            return;
        }

        if (!last && node->local_name() == HTML::TagNames::head) {
            m_insertion_mode = InsertionMode::InHead;
            return;
        }

        if (node->local_name() == HTML::TagNames::body) {
            m_insertion_mode = InsertionMode::InBody;
            return;
        }

        if (node->local_name() == HTML::TagNames::frameset) {
            VERIFY(m_parsing_fragment);
            m_insertion_mode = InsertionMode::InFrameset;
            return;
        }

        if (node->local_name() == HTML::TagNames::html) {
            if (!m_head_element) {
                VERIFY(m_parsing_fragment);
                m_insertion_mode = InsertionMode::BeforeHead;
                return;
            }

            m_insertion_mode = InsertionMode::AfterHead;
            return;
        }
    }

    VERIFY(m_parsing_fragment);
    m_insertion_mode = InsertionMode::InBody;
}

char const* HTMLParser::insertion_mode_name() const
{
    switch (m_insertion_mode) {
#define __ENUMERATE_INSERTION_MODE(mode) \
    case InsertionMode::mode:            \
        return #mode;
        ENUMERATE_INSERTION_MODES
#undef __ENUMERATE_INSERTION_MODE
    }
    VERIFY_NOT_REACHED();
}

DOM::Document& HTMLParser::document()
{
    return *m_document;
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-html-fragments
Vector<JS::Handle<DOM::Node>> HTMLParser::parse_html_fragment(DOM::Element& context_element, StringView markup, AllowDeclarativeShadowRoots allow_declarative_shadow_roots)
{
    // 1. Create a new Document node, and mark it as being an HTML document.
    auto temp_document = DOM::Document::create(context_element.realm());
    temp_document->set_document_type(DOM::Document::Type::HTML);

    temp_document->set_is_temporary_document_for_fragment_parsing({});

    // 2. If the node document of the context element is in quirks mode, then let the Document be in quirks mode.
    //    Otherwise, the node document of the context element is in limited-quirks mode, then let the Document be in limited-quirks mode.
    //    Otherwise, leave the Document in no-quirks mode.
    temp_document->set_quirks_mode(context_element.document().mode());

    // 3. If allowDeclarativeShadowRoots is true, then set Document's allow declarative shadow roots to true.
    if (allow_declarative_shadow_roots == AllowDeclarativeShadowRoots::Yes)
        temp_document->set_allow_declarative_shadow_roots(true);

    // 4. Create a new HTML parser, and associate it with the just created Document node.
    auto parser = HTMLParser::create(*temp_document, markup, "utf-8"sv);
    parser->m_context_element = JS::make_handle(context_element);
    parser->m_parsing_fragment = true;

    // 5. Set the state of the HTML parser's tokenization stage as follows, switching on the context element:
    // - title
    // - textarea
    if (context_element.local_name().is_one_of(HTML::TagNames::title, HTML::TagNames::textarea)) {
        // Switch the tokenizer to the RCDATA state.
        parser->m_tokenizer.switch_to({}, HTMLTokenizer::State::RCDATA);
    }
    // - style
    // - xmp
    // - iframe
    // - noembed
    // - noframes
    else if (context_element.local_name().is_one_of(HTML::TagNames::style, HTML::TagNames::xmp, HTML::TagNames::iframe, HTML::TagNames::noembed, HTML::TagNames::noframes)) {
        // Switch the tokenizer to the RAWTEXT state.
        parser->m_tokenizer.switch_to({}, HTMLTokenizer::State::RAWTEXT);
    }
    // - script
    else if (context_element.local_name().is_one_of(HTML::TagNames::script)) {
        // Switch the tokenizer to the script data state.
        parser->m_tokenizer.switch_to({}, HTMLTokenizer::State::ScriptData);
    }
    // - noscript
    else if (context_element.local_name().is_one_of(HTML::TagNames::noscript)) {
        // If the scripting flag is enabled, switch the tokenizer to the RAWTEXT state. Otherwise, leave the tokenizer in the data state.
        if (context_element.document().is_scripting_enabled())
            parser->m_tokenizer.switch_to({}, HTMLTokenizer::State::RAWTEXT);
    }
    // - plaintext
    else if (context_element.local_name().is_one_of(HTML::TagNames::plaintext)) {
        // Switch the tokenizer to the PLAINTEXT state.
        parser->m_tokenizer.switch_to({}, HTMLTokenizer::State::PLAINTEXT);
    }
    // Any other element
    else {
        // Leave the tokenizer in the data state.
    }

    // 6. Let root be a new html element with no attributes.
    auto root = create_element(context_element.document(), HTML::TagNames::html, Namespace::HTML).release_value_but_fixme_should_propagate_errors();

    // 7. Append the element root to the Document node created above.
    MUST(temp_document->append_child(root));

    // 8. Set up the parser's stack of open elements so that it contains just the single element root.
    parser->m_stack_of_open_elements.push(root);

    // 9. If the context element is a template element,
    if (context_element.local_name() == HTML::TagNames::template_) {
        // push "in template" onto the stack of template insertion modes so that it is the new current template insertion mode.
        parser->m_stack_of_template_insertion_modes.append(InsertionMode::InTemplate);
    }

    // FIXME: 10. Create a start tag token whose name is the local name of context and whose attributes are the attributes of context.
    //           Let this start tag token be the start tag token of the context node, e.g. for the purposes of determining if it is an HTML integration point.

    // 11. Reset the parser's insertion mode appropriately.
    parser->reset_the_insertion_mode_appropriately();

    // 12. Set the parser's form element pointer to the nearest node to the context element that is a form element
    //     (going straight up the ancestor chain, and including the element itself, if it is a form element), if any.
    //     (If there is no such form element, the form element pointer keeps its initial value, null.)
    parser->m_form_element = context_element.first_ancestor_of_type<HTMLFormElement>();

    // 13. Place the input into the input stream for the HTML parser just created. The encoding confidence is irrelevant.
    // 14. Start the parser and let it run until it has consumed all the characters just inserted into the input stream.
    parser->run(context_element.document().url());

    // 15. Return the child nodes of root, in tree order.
    Vector<JS::Handle<DOM::Node>> children;
    while (JS::GCPtr<DOM::Node> child = root->first_child()) {
        MUST(root->remove_child(*child));
        context_element.document().adopt_node(*child);
        children.append(JS::make_handle(*child));
    }
    return children;
}

JS::NonnullGCPtr<HTMLParser> HTMLParser::create_for_scripting(DOM::Document& document)
{
    return document.heap().allocate_without_realm<HTMLParser>(document);
}

JS::NonnullGCPtr<HTMLParser> HTMLParser::create_with_uncertain_encoding(DOM::Document& document, ByteBuffer const& input)
{
    if (document.has_encoding())
        return document.heap().allocate_without_realm<HTMLParser>(document, input, document.encoding().value().to_byte_string());
    auto encoding = run_encoding_sniffing_algorithm(document, input);
    dbgln_if(HTML_PARSER_DEBUG, "The encoding sniffing algorithm returned encoding '{}'", encoding);
    return document.heap().allocate_without_realm<HTMLParser>(document, input, encoding);
}

JS::NonnullGCPtr<HTMLParser> HTMLParser::create(DOM::Document& document, StringView input, StringView encoding)
{
    return document.heap().allocate_without_realm<HTMLParser>(document, input, encoding);
}

enum class AttributeMode {
    No,
    Yes,
};

static String escape_string(StringView string, AttributeMode attribute_mode)
{
    // https://html.spec.whatwg.org/multipage/parsing.html#escapingString
    StringBuilder builder;
    for (auto code_point : Utf8View { string }) {
        // 1. Replace any occurrence of the "&" character by the string "&amp;".
        if (code_point == '&')
            builder.append("&amp;"sv);
        // 2. Replace any occurrences of the U+00A0 NO-BREAK SPACE character by the string "&nbsp;".
        else if (code_point == 0xA0)
            builder.append("&nbsp;"sv);
        // 3. If the algorithm was invoked in the attribute mode, replace any occurrences of the """ character by the string "&quot;".
        else if (code_point == '"' && attribute_mode == AttributeMode::Yes)
            builder.append("&quot;"sv);
        // 4. If the algorithm was not invoked in the attribute mode, replace any occurrences of the "<" character by the string "&lt;", and any occurrences of the ">" character by the string "&gt;".
        else if (code_point == '<' && attribute_mode == AttributeMode::No)
            builder.append("&lt;"sv);
        else if (code_point == '>' && attribute_mode == AttributeMode::No)
            builder.append("&gt;"sv);
        else
            builder.append_code_point(code_point);
    }
    return builder.to_string_without_validation();
}

// https://html.spec.whatwg.org/multipage/parsing.html#html-fragment-serialisation-algorithm
String HTMLParser::serialize_html_fragment(DOM::Node const& node, SerializableShadowRoots serializable_shadow_roots, Vector<JS::Handle<DOM::ShadowRoot>> const& shadow_roots, DOM::FragmentSerializationMode fragment_serialization_mode)
{
    // NOTE: Steps in this function are jumbled a bit to accommodate the Element.outerHTML API.
    //       When called with FragmentSerializationMode::Outer, we will serialize the element itself,
    //       not just its children.

    // 2. Let s be a string, and initialize it to the empty string.
    StringBuilder builder;

    auto serialize_element = [&](DOM::Element const& element) {
        // If current node is an element in the HTML namespace, the MathML namespace, or the SVG namespace, then let tagname be current node's local name.
        // Otherwise, let tagname be current node's qualified name.
        FlyString tag_name;

        if (element.namespace_uri().has_value() && element.namespace_uri()->is_one_of(Namespace::HTML, Namespace::MathML, Namespace::SVG))
            tag_name = element.local_name();
        else
            tag_name = element.qualified_name();

        // Append a U+003C LESS-THAN SIGN character (<), followed by tagname.
        builder.append('<');
        builder.append(tag_name);

        // If current node's is value is not null, and the element does not have an is attribute in its attribute list,
        // then append the string " is="",
        // followed by current node's is value escaped as described below in attribute mode,
        // followed by a U+0022 QUOTATION MARK character (").
        if (element.is_value().has_value() && !element.has_attribute(AttributeNames::is)) {
            builder.append(" is=\""sv);
            builder.append(escape_string(element.is_value().value(), AttributeMode::Yes));
            builder.append('"');
        }

        // For each attribute that the element has,
        // append a U+0020 SPACE character,
        // the attribute's serialized name as described below,
        // a U+003D EQUALS SIGN character (=),
        // a U+0022 QUOTATION MARK character ("),
        // the attribute's value, escaped as described below in attribute mode,
        // and a second U+0022 QUOTATION MARK character (").
        element.for_each_attribute([&](auto const& attribute) {
            builder.append(' ');

            // An attribute's serialized name for the purposes of the previous paragraph must be determined as follows:

            // NOTE: As far as I can tell, these steps are equivalent to just using the qualified name.
            //
            // -> If the attribute has no namespace:
            //         The attribute's serialized name is the attribute's local name.
            // -> If the attribute is in the XML namespace:
            //         The attribute's serialized name is the string "xml:" followed by the attribute's local name.
            // -> If the attribute is in the XMLNS namespace and the attribute's local name is xmlns:
            //         The attribute's serialized name is the string "xmlns".
            // -> If the attribute is in the XMLNS namespace and the attribute's local name is not xmlns:
            //         The attribute's serialized name is the string "xmlns:" followed by the attribute's local name.
            // -> If the attribute is in the XLink namespace:
            //         The attribute's serialized name is the string "xlink:" followed by the attribute's local name.
            // -> If the attribute is in some other namespace:
            //         The attribute's serialized name is the attribute's qualified name.
            builder.append(attribute.name());

            builder.append("=\""sv);
            builder.append(escape_string(attribute.value(), AttributeMode::Yes));
            builder.append('"');
        });

        // Append a U+003E GREATER-THAN SIGN character (>).
        builder.append('>');

        // If current node serializes as void, then continue on to the next child node at this point.
        if (element.serializes_as_void())
            return IterationDecision::Continue;

        // Append the value of running the HTML fragment serialization algorithm with current node,
        // serializableShadowRoots, and shadowRoots (thus recursing into this algorithm for that node),
        // followed by a U+003C LESS-THAN SIGN character (<),
        // a U+002F SOLIDUS character (/),
        // tagname again,
        // and finally a U+003E GREATER-THAN SIGN character (>).
        builder.append(serialize_html_fragment(element, serializable_shadow_roots, shadow_roots));
        builder.append("</"sv);
        builder.append(tag_name);
        builder.append('>');

        return IterationDecision::Continue;
    };

    if (fragment_serialization_mode == DOM::FragmentSerializationMode::Outer) {
        serialize_element(verify_cast<DOM::Element>(node));
        return builder.to_string_without_validation();
    }

    // The algorithm takes as input a DOM Element, Document, or DocumentFragment referred to as the node.
    VERIFY(node.is_element() || node.is_document() || node.is_document_fragment());
    JS::NonnullGCPtr<DOM::Node const> actual_node = node;

    if (is<DOM::Element>(node)) {
        auto const& element = verify_cast<DOM::Element>(node);

        // 1. If the node serializes as void, then return the empty string.
        //    (NOTE: serializes as void is defined only on elements in the spec)
        if (element.serializes_as_void())
            return String {};

        // 3. If the node is a template element, then let the node instead be the template element's template contents (a DocumentFragment node).
        //    (NOTE: This is out of order of the spec to avoid another dynamic cast. The second step just creates a string builder, so it shouldn't matter)
        if (is<HTML::HTMLTemplateElement>(element))
            actual_node = verify_cast<HTML::HTMLTemplateElement>(element).content();

        // 4. If current node is a shadow host, then:
        if (element.is_shadow_host()) {
            // 1. Let shadow be current node's shadow root.
            auto shadow = element.shadow_root();

            // 2. If one of the following is true:
            //    - serializableShadowRoots is true and shadow's serializable is true; or
            //    - shadowRoots contains shadow,
            if ((serializable_shadow_roots == SerializableShadowRoots::Yes && shadow->serializable())
                || shadow_roots.find_first_index_if([&](auto& entry) { return entry == shadow; }).has_value()) {
                // then:
                // 1. Append "<template shadowrootmode="".
                builder.append("<template shadowrootmode=\""sv);

                // 2. If shadow's mode is "open", then append "open". Otherwise, append "closed".
                builder.append(shadow->mode() == Bindings::ShadowRootMode::Open ? "open"sv : "closed"sv);

                // 3. Append """.
                builder.append('"');

                // 4. If shadow's delegates focus is set, then append " shadowrootdelegatesfocus=""".
                if (shadow->delegates_focus())
                    builder.append(" shadowrootdelegatesfocus=\"\""sv);

                // 5. If shadow's serializable is set, then append " shadowrootserializable=""".
                if (shadow->serializable())
                    builder.append(" shadowrootserializable=\"\""sv);

                // 6. If shadow's clonable is set, then append " shadowrootclonable=""".
                if (shadow->clonable())
                    builder.append(" shadowrootclonable=\"\""sv);

                // 7. Append ">".
                builder.append('>');

                // 8. Append the value of running the HTML fragment serialization algorithm with shadow,
                //    serializableShadowRoots, and shadowRoots (thus recursing into this algorithm for that element).
                builder.append(serialize_html_fragment(*shadow, serializable_shadow_roots, shadow_roots));

                // 9. Append "</template>".
                builder.append("</template>"sv);
            }
        }
    }

    // 5. For each child node of the node, in tree order, run the following steps:
    actual_node->for_each_child([&](DOM::Node& current_node) {
        // 1. Let current node be the child node being processed.

        // 2. Append the appropriate string from the following list to s:

        if (is<DOM::Element>(current_node)) {
            // -> If current node is an Element
            auto& element = verify_cast<DOM::Element>(current_node);
            serialize_element(element);
            return IterationDecision::Continue;
        }

        if (is<DOM::Text>(current_node)) {
            // -> If current node is a Text node
            auto& text_node = verify_cast<DOM::Text>(current_node);
            auto* parent = current_node.parent();

            if (is<DOM::Element>(parent)) {
                auto& parent_element = verify_cast<DOM::Element>(*parent);

                // If the parent of current node is a style, script, xmp, iframe, noembed, noframes, or plaintext element,
                // or if the parent of current node is a noscript element and scripting is enabled for the node, then append the value of current node's data IDL attribute literally.
                if (parent_element.local_name().is_one_of(HTML::TagNames::style, HTML::TagNames::script, HTML::TagNames::xmp, HTML::TagNames::iframe, HTML::TagNames::noembed, HTML::TagNames::noframes, HTML::TagNames::plaintext)
                    || (parent_element.local_name() == HTML::TagNames::noscript && !parent_element.is_scripting_disabled())) {
                    builder.append(text_node.data());
                    return IterationDecision::Continue;
                }
            }

            // Otherwise, append the value of current node's data IDL attribute, escaped as described below.
            builder.append(escape_string(text_node.data(), AttributeMode::No));
        }

        if (is<DOM::Comment>(current_node)) {
            // -> If current node is a Comment
            auto& comment_node = verify_cast<DOM::Comment>(current_node);

            // Append the literal string "<!--" (U+003C LESS-THAN SIGN, U+0021 EXCLAMATION MARK, U+002D HYPHEN-MINUS, U+002D HYPHEN-MINUS),
            // followed by the value of current node's data IDL attribute, followed by the literal string "-->" (U+002D HYPHEN-MINUS, U+002D HYPHEN-MINUS, U+003E GREATER-THAN SIGN).
            builder.append("<!--"sv);
            builder.append(comment_node.data());
            builder.append("-->"sv);
            return IterationDecision::Continue;
        }

        if (is<DOM::ProcessingInstruction>(current_node)) {
            // -> If current node is a ProcessingInstruction
            auto& processing_instruction_node = verify_cast<DOM::ProcessingInstruction>(current_node);

            // Append the literal string "<?" (U+003C LESS-THAN SIGN, U+003F QUESTION MARK), followed by the value of current node's target IDL attribute,
            // followed by a single U+0020 SPACE character, followed by the value of current node's data IDL attribute, followed by a single U+003E GREATER-THAN SIGN character (>).
            builder.append("<?"sv);
            builder.append(processing_instruction_node.target());
            builder.append(' ');
            builder.append(processing_instruction_node.data());
            builder.append('>');
            return IterationDecision::Continue;
        }

        if (is<DOM::DocumentType>(current_node)) {
            // -> If current node is a DocumentType
            auto& document_type_node = verify_cast<DOM::DocumentType>(current_node);

            // Append the literal string "<!DOCTYPE" (U+003C LESS-THAN SIGN, U+0021 EXCLAMATION MARK, U+0044 LATIN CAPITAL LETTER D, U+004F LATIN CAPITAL LETTER O,
            // U+0043 LATIN CAPITAL LETTER C, U+0054 LATIN CAPITAL LETTER T, U+0059 LATIN CAPITAL LETTER Y, U+0050 LATIN CAPITAL LETTER P, U+0045 LATIN CAPITAL LETTER E),
            // followed by a space (U+0020 SPACE), followed by the value of current node's name IDL attribute, followed by the literal string ">" (U+003E GREATER-THAN SIGN).
            builder.append("<!DOCTYPE "sv);
            builder.append(document_type_node.name());
            builder.append('>');
            return IterationDecision::Continue;
        }

        return IterationDecision::Continue;
    });

    // 6. Return s.
    return MUST(builder.to_string());
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#current-dimension-value
static RefPtr<CSS::StyleValue> parse_current_dimension_value(float value, Utf8View input, Utf8View::Iterator position)
{
    // 1. If position is past the end of input, then return value as a length.
    if (position == input.end())
        return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(value)));

    // 2. If the code point at position within input is U+0025 (%), then return value as a percentage.
    if (*position == '%')
        return CSS::PercentageStyleValue::create(CSS::Percentage(value));

    // 3. Return value as a length.
    return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(value)));
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-dimension-values
RefPtr<CSS::StyleValue> parse_dimension_value(StringView string)
{
    // 1. Let input be the string being parsed.
    auto input = Utf8View(string);
    if (!input.validate())
        return nullptr;

    // 2. Let position be a position variable for input, initially pointing at the start of input.
    auto position = input.begin();

    // 3. Skip ASCII whitespace within input given position.
    while (position != input.end() && Infra::is_ascii_whitespace(*position))
        ++position;

    // 4. If position is past the end of input or the code point at position within input is not an ASCII digit,
    //    then return failure.
    if (position == input.end() || !is_ascii_digit(*position))
        return nullptr;

    // 5. Collect a sequence of code points that are ASCII digits from input given position,
    //    and interpret the resulting sequence as a base-ten integer. Let value be that number.
    StringBuilder number_string;
    while (position != input.end() && is_ascii_digit(*position)) {
        number_string.append(*position);
        ++position;
    }
    auto integer_value = number_string.string_view().to_number<int>();

    // 6. If position is past the end of input, then return value as a length.
    if (position == input.end())
        return CSS::LengthStyleValue::create(CSS::Length::make_px(*integer_value));

    float value = *integer_value;

    // 7. If the code point at position within input is U+002E (.), then:
    if (*position == '.') {
        // 1. Advance position by 1.
        ++position;

        // 2. If position is past the end of input or the code point at position within input is not an ASCII digit,
        //    then return the current dimension value with value, input, and position.
        if (position == input.end() || !is_ascii_digit(*position))
            return parse_current_dimension_value(value, input, position);

        // 3. Let divisor have the value 1.
        float divisor = 1;

        // 4. While true:
        while (true) {
            // 1. Multiply divisor by ten.
            divisor *= 10;

            // 2. Add the value of the code point at position within input,
            //    interpreted as a base-ten digit (0..9) and divided by divisor, to value.
            value += (*position - '0') / divisor;

            // 3. Advance position by 1.
            ++position;

            // 4. If position is past the end of input, then return value as a length.
            if (position == input.end())
                return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(value)));

            // 5. If the code point at position within input is not an ASCII digit, then break.
            if (!is_ascii_digit(*position))
                break;
        }
    }

    // 8. Return the current dimension value with value, input, and position.
    return parse_current_dimension_value(value, input, position);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-non-zero-dimension-values
RefPtr<CSS::StyleValue> parse_nonzero_dimension_value(StringView string)
{
    // 1. Let input be the string being parsed.
    // 2. Let value be the result of parsing input using the rules for parsing dimension values.
    auto value = parse_dimension_value(string);

    // 3. If value is an error, return an error.
    if (!value)
        return nullptr;

    // 4. If value is zero, return an error.
    if (value->is_length() && value->as_length().length().raw_value() == 0)
        return nullptr;
    if (value->is_percentage() && value->as_percentage().percentage().value() == 0)
        return nullptr;

    // 5. If value is a percentage, return value as a percentage.
    // 6. Return value as a length.
    return value;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-a-legacy-colour-value
Optional<Color> parse_legacy_color_value(StringView string)
{
    // 1. Let input be the string being parsed
    ByteString input = MUST(ByteString::from_utf8(string));

    // 2. If input is the empty string, then return an error.
    if (input.is_empty())
        return {};

    // 3. Strip leading and trailing ASCII whitespace from input.
    input = input.trim(Infra::ASCII_WHITESPACE);

    // 4. If input is an ASCII case-insensitive match for the string "transparent", then return an error.
    if (Infra::is_ascii_case_insensitive_match(input, "transparent"sv))
        return {};

    // 5. If input is an ASCII case-insensitive match for one of the named colors, then return the simple color corresponding to that keyword. [CSSCOLOR]
    if (auto const color = Color::from_named_css_color_string(input); color.has_value())
        return color;

    auto hex_nibble_to_u8 = [](char nibble) -> u8 {
        if (nibble >= '0' && nibble <= '9')
            return nibble - '0';
        if (nibble >= 'a' && nibble <= 'f')
            return nibble - 'a' + 10;
        return nibble - 'A' + 10;
    };

    // 6. If input's code point length is four, and the first character in input is U+0023 (#), and the last three characters of input are all ASCII hex digits, then:
    if (input.length() == 4 && input[0] == '#' && is_ascii_hex_digit(input[1]) && is_ascii_hex_digit(input[2]) && is_ascii_hex_digit(input[3])) {
        // 1. Let result be a simple color.
        Color result;
        result.set_alpha(0xFF);

        // 2. Interpret the second character of input as a hexadecimal digit; let the red component of result be the resulting number multiplied by 17.
        result.set_red(hex_nibble_to_u8(input[1]) * 17);

        // 3. Interpret the third character of input as a hexadecimal digit; let the green component of result be the resulting number multiplied by 17.
        result.set_green(hex_nibble_to_u8(input[2]) * 17);

        // 4. Interpret the fourth character of input as a hexadecimal digit; let the blue component of result be the resulting number multiplied by 17.
        result.set_blue(hex_nibble_to_u8(input[3]) * 17);

        // 5. Return result.
        return result;
    }

    // 7. Replace any code points greater than U+FFFF in input (i.e., any characters that are not in the basic multilingual plane) with the two-character string "00".
    auto replace_non_basic_multilingual_code_points = [](StringView string) -> ByteString {
        StringBuilder builder;
        for (auto code_point : Utf8View { string }) {
            if (code_point > 0xFFFF)
                builder.append("00"sv);
            else
                builder.append_code_point(code_point);
        }
        return builder.to_byte_string();
    };
    input = replace_non_basic_multilingual_code_points(input);

    // 8. If input's code point length is greater than 128, truncate input, leaving only the first 128 characters.
    if (input.length() > 128)
        input = input.substring(0, 128);

    // 9. If the first character in input is a U+0023 NUMBER SIGN character (#), remove it.
    if (input[0] == '#')
        input = input.substring(1);

    // 10. Replace any character in input that is not an ASCII hex digit with the character U+0030 DIGIT ZERO (0).
    auto replace_non_ascii_hex = [](StringView string) -> ByteString {
        StringBuilder builder;
        for (auto code_point : Utf8View { string }) {
            if (is_ascii_hex_digit(code_point))
                builder.append_code_point(code_point);
            else
                builder.append_code_point('0');
        }
        return builder.to_byte_string();
    };
    input = replace_non_ascii_hex(input);

    // 11. While input's code point length is zero or not a multiple of three, append a U+0030 DIGIT ZERO (0) character to input.
    StringBuilder builder;
    builder.append(input);
    while (builder.length() == 0 || (builder.length() % 3 != 0))
        builder.append_code_point('0');
    input = builder.to_byte_string();

    // 12. Split input into three strings of equal code point length, to obtain three components. Let length be the code point length that all of those components have (one third the code point length of input).
    auto length = input.length() / 3;
    auto first_component = input.substring_view(0, length);
    auto second_component = input.substring_view(length, length);
    auto third_component = input.substring_view(length * 2, length);

    // 13. If length is greater than 8, then remove the leading length-8 characters in each component, and let length be 8.
    if (length > 8) {
        first_component = first_component.substring_view(length - 8);
        second_component = second_component.substring_view(length - 8);
        third_component = third_component.substring_view(length - 8);
        length = 8;
    }

    // 14. While length is greater than two and the first character in each component is a U+0030 DIGIT ZERO (0) character, remove that character and reduce length by one.
    while (length > 2 && first_component[0] == '0' && second_component[0] == '0' && third_component[0] == '0') {
        --length;
        first_component = first_component.substring_view(1);
        second_component = second_component.substring_view(1);
        third_component = third_component.substring_view(1);
    }

    // 15. If length is still greater than two, truncate each component, leaving only the first two characters in each.
    if (length > 2) {
        first_component = first_component.substring_view(0, 2);
        second_component = second_component.substring_view(0, 2);
        third_component = third_component.substring_view(0, 2);
    }

    auto to_hex = [&](StringView string) -> u8 {
        auto nib1 = hex_nibble_to_u8(string[0]);
        auto nib2 = hex_nibble_to_u8(string[1]);
        return nib1 << 4 | nib2;
    };

    // 16. Let result be a simple color.
    Color result;
    result.set_alpha(0xFF);

    // 17. Interpret the first component as a hexadecimal number; let the red component of result be the resulting number.
    result.set_red(to_hex(first_component));

    // 18. Interpret the second component as a hexadecimal number; let the green component of result be the resulting number.
    result.set_green(to_hex(second_component));

    // 19. Interpret the third component as a hexadecimal number; let the blue component of result be the resulting number.
    result.set_blue(to_hex(third_component));

    // 20. Return result.
    return result;
}

JS::Realm& HTMLParser::realm()
{
    return m_document->realm();
}

// https://html.spec.whatwg.org/multipage/parsing.html#abort-a-parser
void HTMLParser::abort()
{
    // 1. Throw away any pending content in the input stream, and discard any future content that would have been added to it.
    m_tokenizer.abort();

    // FIXME: 2. Stop the speculative HTML parser for this HTML parser.

    // 3. Update the current document readiness to "interactive".
    m_document->update_readiness(DocumentReadyState::Interactive);

    // 4. Pop all the nodes off the stack of open elements.
    while (!m_stack_of_open_elements.is_empty())
        m_stack_of_open_elements.pop();

    // 5. Update the current document readiness to "complete".
    m_document->update_readiness(DocumentReadyState::Complete);

    m_aborted = true;
}

// https://html.spec.whatwg.org/multipage/parsing.html#insert-an-element-at-the-adjusted-insertion-location
void HTMLParser::insert_an_element_at_the_adjusted_insertion_location(JS::NonnullGCPtr<DOM::Element> element)
{
    // 1. Let the adjusted insertion location be the appropriate place for inserting a node.
    auto adjusted_insertion_location = find_appropriate_place_for_inserting_node();

    // 2. If it is not possible to insert element at the adjusted insertion location, abort these steps.
    if (!adjusted_insertion_location.parent)
        return;

    // 3. If the parser was not created as part of the HTML fragment parsing algorithm,
    //    then push a new element queue onto element's relevant agent's custom element reactions stack.
    if (!m_parsing_fragment) {
        auto& custom_data = verify_cast<Bindings::WebEngineCustomData>(*relevant_agent(*element).custom_data());
        custom_data.custom_element_reactions_stack.element_queue_stack.append({});
    }

    // 4. Insert element at the adjusted insertion location.
    adjusted_insertion_location.parent->insert_before(element, adjusted_insertion_location.insert_before_sibling);

    // 5. If the parser was not created as part of the HTML fragment parsing algorithm,
    //    then pop the element queue from element's relevant agent's custom element reactions stack, and invoke custom element reactions in that queue.
    if (!m_parsing_fragment) {
        auto& custom_data = verify_cast<Bindings::WebEngineCustomData>(*relevant_agent(*element).custom_data());
        auto queue = custom_data.custom_element_reactions_stack.element_queue_stack.take_last();
        Bindings::invoke_custom_element_reactions(queue);
    }
}

}
