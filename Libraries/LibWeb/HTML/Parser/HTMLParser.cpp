/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Parser/SpeculativeHTMLParser.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTMLTokenizerRustFFI.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/SVG/SVGScriptElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLParser);
GC_DEFINE_ALLOCATOR(HTMLParserEndState);

static DOM::Node& node_from_html_parser_ffi(size_t);
static HTMLParser& parser_from_html_parser_ffi(void*);
static RustFfiHtmlNamespace namespace_to_html_parser_ffi(Optional<FlyString> const&);
static RustFfiHtmlAttributeNamespace attribute_namespace_to_html_parser_ffi(Optional<FlyString> const&);
static RustFfiHtmlQuirksMode quirks_mode_to_html_parser_ffi(DOM::QuirksMode);

extern "C" void ladybird_html_parser_log_parse_error(void*, u8 const*, size_t);
extern "C" void ladybird_html_parser_stop_parsing(void*);
extern "C" bool ladybird_html_parser_parse_errors_enabled();
extern "C" void ladybird_html_parser_visit_node(void*, size_t);
extern "C" size_t ladybird_html_parser_document_node(void*);
extern "C" size_t ladybird_html_parser_document_html_element(void*);
extern "C" void ladybird_html_parser_set_document_quirks_mode(void*, RustFfiHtmlQuirksMode);
extern "C" size_t ladybird_html_parser_create_document_type(void*, u8 const*, size_t, u8 const*, size_t, u8 const*, size_t);
extern "C" size_t ladybird_html_parser_create_comment(void*, u8 const*, size_t);
extern "C" void ladybird_html_parser_insert_text(size_t, size_t, u8 const*, size_t);
extern "C" void ladybird_html_parser_add_missing_attribute(size_t, u8 const*, size_t, u8 const*, size_t);
extern "C" void ladybird_html_parser_remove_node(size_t);
extern "C" void ladybird_html_parser_handle_element_popped(size_t);
extern "C" void ladybird_html_parser_prepare_svg_script(void*, size_t, size_t);
extern "C" void ladybird_html_parser_set_script_source_line(void*, size_t, size_t);
extern "C" void ladybird_html_parser_mark_script_already_started(void*, size_t);
extern "C" size_t ladybird_html_parser_parent_node(size_t);
extern "C" size_t ladybird_html_parser_create_element(void*, size_t, RustFfiHtmlNamespace, u8 const*, size_t, u8 const*, size_t, RustFfiHtmlParserAttribute const*, size_t, bool, size_t, bool);
extern "C" void ladybird_html_parser_append_child(size_t, size_t);
extern "C" void ladybird_html_parser_insert_node(size_t, size_t, size_t, bool);
extern "C" void ladybird_html_parser_move_all_children(size_t, size_t);
extern "C" size_t ladybird_html_parser_template_content(size_t);
extern "C" size_t ladybird_html_parser_attach_declarative_shadow_root(size_t, RustFfiHtmlShadowRootMode, RustFfiHtmlSlotAssignmentMode, bool, bool, bool, bool);
extern "C" void ladybird_html_parser_set_template_content(size_t, size_t);
extern "C" bool ladybird_html_parser_allows_declarative_shadow_roots(size_t);

HTMLParser::HTMLParser(DOM::Document& document, ParserScriptingMode scripting_mode, StringView input, StringView encoding)
    : m_tokenizer(input, encoding)
    , m_scripting_mode(scripting_mode)
    , m_document(document)
{
    m_rust_parser = rust_html_parser_create();
    m_document->set_parser({}, *this);
    auto standardized_encoding = TextCodec::get_standardized_encoding(encoding);
    VERIFY(standardized_encoding.has_value());
    m_document->set_encoding(MUST(String::from_utf8(standardized_encoding.value())));
}

HTMLParser::HTMLParser(DOM::Document& document, ParserScriptingMode scripting_mode, ScriptCreatedParser script_created)
    : m_scripting_mode(scripting_mode)
    , m_script_created(script_created == ScriptCreatedParser::Yes)
    , m_document(document)
{
    m_rust_parser = rust_html_parser_create();
    m_document->set_parser({}, *this);
}

HTMLParser::~HTMLParser() = default;

void HTMLParser::finalize()
{
    Base::finalize();
    if (m_rust_parser) {
        rust_html_parser_destroy(m_rust_parser);
        m_rust_parser = nullptr;
    }
}

void HTMLParser::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_form_element);
    visitor.visit(m_context_element);
    visitor.visit(m_active_speculative_html_parser);

    rust_html_parser_visit_edges(m_rust_parser, &visitor);
}

void HTMLParser::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
}

void HTMLParser::run(HTMLTokenizer::StopAtInsertionPoint stop_at_insertion_point)
{
    m_stop_parsing = false;

    for (;;) {
        if (m_parser_pause_flag)
            break;

        auto result = rust_html_parser_run_document(
            m_rust_parser,
            m_tokenizer.ffi_handle({}),
            this,
            m_scripting_mode != ParserScriptingMode::Disabled,
            stop_at_insertion_point == HTMLTokenizer::StopAtInsertionPoint::Yes);
        if (result == RustFfiHtmlParserRunResult::Ok)
            break;

        if (result == RustFfiHtmlParserRunResult::ExecuteScript) {
            auto script = rust_html_parser_take_pending_script(m_rust_parser);
            VERIFY(script);
            process_script_end_tag_from_rust_parser(as<HTMLScriptElement>(node_from_html_parser_ffi(script)));
            continue;
        }

        if (result == RustFfiHtmlParserRunResult::ExecuteSvgScript) {
            auto script = rust_html_parser_take_pending_svg_script(m_rust_parser);
            VERIFY(script);
            if (process_svg_script_end_tag_from_rust_parser(as<SVG::SVGScriptElement>(node_from_html_parser_ffi(script))))
                break;
            continue;
        }

        VERIFY_NOT_REACHED();
    }

    m_tokenizer.parser_did_run({});
}

void HTMLParser::run(URL::URL const& url, HTMLTokenizer::StopAtInsertionPoint stop_at_insertion_point)
{
    m_document->set_url(url);
    m_document->set_source(m_tokenizer.source());
    run_until_completion(stop_at_insertion_point);
}

void HTMLParser::pop_all_open_elements()
{
    rust_html_parser_pop_all_open_elements(m_rust_parser);
}

void HTMLParser::configure_element_created_by_rust_parser(DOM::Element& element)
{
    if (element.local_name() == HTML::TagNames::link && element.namespace_uri() == Namespace::HTML) {
        // AD-HOC: Let <link> elements know which document they were originally parsed for.
        //         This is used for the render-blocking logic.
        auto& link_element = as<HTMLLinkElement>(element);
        link_element.set_parser_document({}, document());
        link_element.set_was_enabled_when_created_by_parser({}, !element.has_attribute(HTML::AttributeNames::disabled));
        return;
    }

    if (element.local_name() != HTML::TagNames::script || element.namespace_uri() != Namespace::HTML)
        return;

    auto& script_element = as<HTMLScriptElement>(element);
    if (m_scripting_mode != ParserScriptingMode::Fragment)
        script_element.set_parser_document(Badge<HTMLParser> {}, document());
    script_element.set_force_async(Badge<HTMLParser> {}, false);
    if (m_scripting_mode == ParserScriptingMode::Inert)
        script_element.set_already_started(Badge<HTMLParser> {}, true);
}

GC::Ref<DOM::Element> HTMLParser::create_element_for_rust_parser(HTMLToken const& token, Optional<FlyString> const& namespace_, DOM::Node& intended_parent, bool had_duplicate_attribute, GC::Ptr<HTMLFormElement> form_element, bool has_template_element_on_stack)
{
    auto element = create_element_for(token, namespace_, intended_parent);
    configure_element_created_by_rust_parser(element);

    // AD-HOC: See AD-HOC comment on Element.m_had_duplicate_attribute_during_tokenization about why this is done.
    if (had_duplicate_attribute)
        element->set_had_duplicate_attribute_during_tokenization({});

    if (form_element && !has_template_element_on_stack) {
        auto* html_element = as_if<HTML::HTMLElement>(*element);
        if (html_element && html_element->is_form_associated_element() && !html_element->is_form_associated_custom_element()) {
            if ((!html_element->is_listed() || !html_element->has_attribute(HTML::AttributeNames::form))
                && &intended_parent.root() == &form_element->root()) {
                html_element->set_form(form_element.ptr());
                html_element->set_parser_inserted({});
            }
        }
    }

    return element;
}

bool HTMLParser::process_script_end_tag_from_rust_parser(HTMLScriptElement& script)
{
    // If the active speculative HTML parser is null and the JavaScript execution context stack is empty, then perform a microtask checkpoint.
    // The active speculative HTML parser is null here; start/stop are paired around the spin_until below.
    auto& vm = main_thread_event_loop().vm();
    if (!vm.has_running_execution_context())
        perform_a_microtask_checkpoint();

    // Let the old insertion point have the same value as the current insertion point.
    m_tokenizer.store_old_insertion_point();

    // Let the insertion point be just before the next input character.
    m_tokenizer.update_insertion_point();

    // Increment the parser's script nesting level by one.
    increment_script_nesting_level();

    // https://w3c.github.io/trusted-types/dist/spec/#setting-slot-values-from-parser
    // Set script’s script text value to its child text content.
    script.set_string_text(script.child_text_content());

    // If the active speculative HTML parser is null, then prepare the script element script.
    // This might cause some script to execute, which might cause new characters to be inserted into the tokenizer,
    // and might cause the tokenizer to output more tokens, resulting in a reentrant invocation of the parser.
    // The active speculative HTML parser is null here (see above).
    script.prepare_script(Badge<HTMLParser> {});

    // Decrement the parser's script nesting level by one.
    decrement_script_nesting_level();

    // If the parser's script nesting level is zero, then set the parser pause flag to false.
    if (script_nesting_level() == 0)
        m_parser_pause_flag = false;

    // Let the insertion point have the value of the old insertion point.
    m_tokenizer.restore_old_insertion_point();

    // At this stage, if the pending parsing-blocking script is not null, then:
    if (document().pending_parsing_blocking_script()) {
        // -> If the script nesting level is not zero:
        if (script_nesting_level() != 0) {
            // Set the parser pause flag to true,
            m_parser_pause_flag = true;
            // and abort the processing of any nested invocations of the tokenizer, yielding control back to the caller.
            // (Tokenization will resume when the caller returns to the "outer" tree construction stage.)
            return true;
        }

        // -> Otherwise:
        // The spec's "While the pending parsing-blocking script is not null" loop and the contained "spin the event
        // loop" step are implemented asynchronously: pause the parser, schedule a resume check, and yield back to
        // the caller. The remaining steps (4-13) run from resume_after_parser_blocking_script when the script is
        // ready.

        // 3. Start the speculative HTML parser for this instance of the HTML parser.
        start_the_speculative_html_parser();

        m_parser_pause_flag = true;
        schedule_resume_check();
    }

    return m_parser_pause_flag;
}

void HTMLParser::prepare_svg_script_for_rust_parser(SVG::SVGScriptElement& script, size_t source_line_number)
{
    // AD-HOC: For SVG script elements, set the parser-inserted flag before the element is inserted into the DOM.
    // Otherwise inserted()/attribute_changed() would invoke process_the_script_element() with the flag still unset
    // and bypass the parser-blocking fetch handling.
    //
    // https://html.spec.whatwg.org/multipage/parsing.html#scripting-mode
    // The Fragment scripting mode treats parser-inserted scripts as if they were not parser-inserted, allowing, for
    // example, executing scripts when applying a fragment created by createContextualFragment().
    if (m_scripting_mode != ParserScriptingMode::Fragment)
        script.set_parser_inserted({});
    script.set_source_line_number({}, source_line_number);
}

void HTMLParser::set_script_source_line_from_rust_parser(DOM::Element& element, size_t source_line_number)
{
    if (auto* html_script_element = as_if<HTML::HTMLScriptElement>(element)) {
        html_script_element->set_source_line_number({}, source_line_number);
        return;
    }
    if (auto* svg_script_element = as_if<SVG::SVGScriptElement>(element))
        svg_script_element->set_source_line_number({}, source_line_number);
}

void HTMLParser::mark_script_already_started_from_rust_parser(HTMLScriptElement& script)
{
    script.set_already_started(Badge<HTMLParser> {}, true);
}

void HTMLParser::stop_parsing_from_rust_parser()
{
    stop_parsing();
}

bool HTMLParser::process_svg_script_end_tag_from_rust_parser(SVG::SVGScriptElement& script)
{
    // Let the old insertion point have the same value as the current insertion point.
    m_tokenizer.store_old_insertion_point();

    // Let the insertion point be just before the next input character.
    m_tokenizer.update_insertion_point();

    // Increment the parser's script nesting level by one.
    increment_script_nesting_level();

    // Set the parser pause flag to true.
    m_parser_pause_flag = true;

    // If the active speculative HTML parser is null and the user agent supports SVG, then Process the SVG script element according to the SVG rules. [SVG]
    // The active speculative HTML parser is null here.
    script.process_the_script_element();

    // Decrement the parser's script nesting level by one.
    decrement_script_nesting_level();

    // If the parser's script nesting level is zero, then set the parser pause flag to false.
    if (script_nesting_level() == 0)
        m_parser_pause_flag = false;

    // Let the insertion point have the value of the old insertion point.
    m_tokenizer.restore_old_insertion_point();

    // If the SVG script registered itself as a pending parsing-blocking script (external fetch in flight),
    // pause the parser and schedule a resume check. The parser will resume from
    // resume_after_parser_blocking_script when the fetch completes.
    if (document().pending_parsing_blocking_svg_script()) {
        m_parser_pause_flag = true;
        schedule_resume_check();
    }

    return m_parser_pause_flag;
}

void HTMLParser::run_until_completion(HTMLTokenizer::StopAtInsertionPoint stop_at_insertion_point)
{
    m_post_parse_action = [this] { the_end(*m_document, this); };
    run(stop_at_insertion_point);
    if (!m_parser_pause_flag)
        invoke_post_parse_action();
}

// https://html.spec.whatwg.org/multipage/parsing.html#the-end
void HTMLParser::the_end(GC::Ref<DOM::Document> document, GC::Ptr<HTMLParser> parser)
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

    // 1. If the active speculative HTML parser is not null, then stop the speculative HTML parser and return.
    if (parser && parser->m_active_speculative_html_parser) {
        parser->stop_the_speculative_html_parser();
        return;
    }

    // 2. Set the insertion point to undefined.
    if (parser)
        parser->m_tokenizer.undefine_insertion_point();

    // 3. Update the current document readiness to "interactive".
    document->update_readiness(HTML::DocumentReadyState::Interactive);

    // 4. Pop all the nodes off the stack of open elements.
    if (parser)
        parser->pop_all_open_elements();

    // AD-HOC: Skip remaining steps when there's no browsing context.
    // This happens when parsing HTML via DOMParser or similar mechanisms.
    // Note: This diverges from the spec, which expects more steps to follow.
    if (!document->browsing_context()) {
        // Parsed via DOMParser, no need to wait for load events.
        document->update_readiness(HTML::DocumentReadyState::Complete);
        return;
    }

    // Steps 5-11 are handled by the HTMLParserEndState state machine.
    auto state = HTMLParserEndState::create(document, parser);
    document->set_html_parser_end_state(state);
    state->schedule_progress_check();
}

static constexpr int THE_END_TIMEOUT_MS = 15000;

// Perform a microtask checkpoint matching spin_until's pre-check semantics: pending microtasks (e.g. image load-event
// delayer creation from update_the_image_data step 8) must be drained before checking parser progress. The empty-queue
// fast path avoids the save/clear/restore of the execution context stack and notify_about_rejected_promises when there
// is nothing to drain.
static void perform_pre_progress_microtask_checkpoint()
{
    auto& event_loop = main_thread_event_loop();
    if (event_loop.microtask_queue_empty())
        return;
    auto& vm = event_loop.vm();
    vm.save_execution_context_stack();
    vm.clear_execution_context_stack();
    event_loop.perform_a_microtask_checkpoint();
    vm.restore_execution_context_stack();
}

GC::Ref<HTMLParserEndState> HTMLParserEndState::create(GC::Ref<DOM::Document> document, GC::Ptr<HTMLParser> parser)
{
    return document->heap().allocate<HTMLParserEndState>(document, parser);
}

HTMLParserEndState::HTMLParserEndState(GC::Ref<DOM::Document> document, GC::Ptr<HTMLParser> parser)
    : m_document(document)
    , m_parser(parser)
    , m_timeout(Platform::Timer::create_single_shot(heap(), THE_END_TIMEOUT_MS, GC::create_function(heap(), [this] {
        if (m_phase != Phase::Completed)
            dbgln("HTMLParserEndState: timed out in phase {}", to_underlying(m_phase));
    })))
{
    m_timeout->start();
}

void HTMLParserEndState::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_parser);
    visitor.visit(m_timeout);
}

void HTMLParserEndState::schedule_progress_check()
{
    if (m_phase == Phase::Completed)
        return;
    if (m_check_pending)
        return;
    m_check_pending = true;
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this] {
        perform_pre_progress_microtask_checkpoint();
        check_progress();
        m_check_pending = false;
    }));
}

void HTMLParserEndState::check_progress()
{
    // AD-HOC: Bail out if the document is no longer fully active (e.g. navigated away from).
    if (!m_document->is_fully_active()) {
        complete();
        return;
    }

    switch (m_phase) {
    case Phase::WaitingForDeferredScripts:
        // 5. While the list of scripts that will execute when the document has finished parsing is not empty:
        while (!m_document->scripts_to_execute_when_parsing_has_finished().is_empty()) {
            auto& first_script = *m_document->scripts_to_execute_when_parsing_has_finished().first();

            // 1. Spin the event loop until the first script in the list of scripts that will execute when the document has finished parsing
            //    has its "ready to be parser-executed" flag set and the parser's Document has no style sheet that is blocking scripts.
            if (!first_script.is_ready_to_be_parser_executed() || m_document->has_a_style_sheet_that_is_blocking_scripts())
                return;

            // 2. Execute the first script in the list of scripts that will execute when the document has finished parsing.
            first_script.execute_script();

            // 3. Remove the first script element from the list of scripts that will execute when the document has finished parsing (i.e. shift out the first entry in the list).
            (void)m_document->scripts_to_execute_when_parsing_has_finished().take_first();
        }

        advance_to_asap_scripts_phase();
        [[fallthrough]];

    case Phase::WaitingForASAPScripts:
        // 7. Spin the event loop until the set of scripts that will execute as soon as possible and the list of scripts
        //    that will execute in order as soon as possible are empty.
        if (!m_document->scripts_to_execute_as_soon_as_possible().is_empty()
            || !m_document->scripts_to_execute_in_order_as_soon_as_possible().is_empty())
            return;

        m_phase = Phase::WaitingForLoadEventDelay;
        [[fallthrough]];

    case Phase::WaitingForLoadEventDelay:
        // 8. Spin the event loop until there is nothing that delays the load event in the Document.
        if (m_document->anything_is_delaying_the_load_event())
            return;

        m_phase = Phase::Completed;
        [[fallthrough]];

    case Phase::Completed:
        complete();
        return;
    }
}

void HTMLParserEndState::advance_to_asap_scripts_phase()
{
    // AD-HOC: We need to scroll to the fragment on page load somewhere.
    // But a script that ran in step 5 above may have scrolled the page already,
    // so only do this if there is an actual fragment to avoid resetting the scroll position unexpectedly.
    // Spec bug: https://github.com/whatwg/html/issues/10914
    auto indicated_part = m_document->determine_the_indicated_part();
    if (indicated_part.has<DOM::Element*>() && indicated_part.get<DOM::Element*>() != nullptr) {
        m_document->scroll_to_the_fragment();
    }

    // 6. Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following substeps:
    queue_global_task(HTML::Task::Source::DOMManipulation, *m_document, GC::create_function(m_document->heap(), [document = m_document] {
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

    m_phase = Phase::WaitingForASAPScripts;
}

void HTMLParserEndState::complete()
{
    m_phase = Phase::Completed;
    m_timeout->stop();
    m_document->set_html_parser_end_state(nullptr);

    // 9. Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following steps:
    queue_global_task(HTML::Task::Source::DOMManipulation, *m_document, GC::create_function(m_document->heap(), [document = m_document, parser = m_parser] {
        // 1. Update the current document readiness to "complete".
        document->update_readiness(HTML::DocumentReadyState::Complete);

        // AD-HOC: We need to wait until the document ready state is complete before detaching the parser, otherwise the DOM complete time will not be set correctly.
        if (parser)
            document->detach_parser();

        // 2. If the Document object's browsing context is null, then abort these steps.
        if (!document->browsing_context())
            return;

        // 3. Let window be the Document's relevant global object.
        auto& window = as<Window>(relevant_global_object(*document));

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

        // 10. Set the Document's page showing to true.
        document->set_page_showing(true);

        // 11. Fire a page transition event named pageshow at window with false.
        window.fire_a_page_transition_event(HTML::EventNames::pageshow, false);

        // 12. Completely finish loading the Document.
        document->completely_finish_loading();

        // FIXME: 13. Queue the navigation timing entry for the Document.
    }));

    // FIXME: 10. If the Document's print when loaded flag is set, then run the printing steps.

    // 11. The Document is now ready for post-load tasks.
    m_document->set_ready_for_post_load_tasks(true);
}
// https://html.spec.whatwg.org/multipage/parsing.html#create-an-element-for-the-token
GC::Ref<DOM::Element> HTMLParser::create_element_for(HTMLToken const& token, Optional<FlyString> const& namespace_, DOM::Node& intended_parent)
{
    // 1. If the active speculative HTML parser is not null, then return the result of creating a speculative mock element given namespace, token's tag name, and token's attributes.
    // The active speculative HTML parser runs synchronously to completion, so it is null whenever the real
    // parser invokes this algorithm. The speculative parser produces mock elements via its own path.

    // 2. Otherwise, optionally create a speculative mock element given namespace, token's tag name, and token's attributes.
    // We deliberately skip step 2 — the active speculative parser already issues these fetches, so doing it
    // again here would be redundant.

    // 3. Let document be intendedParent's node document.
    GC::Ref<DOM::Document> document = intended_parent.document();

    // 4. Let localName be token's tag name.
    auto const& local_name = token.tag_name();

    // 5. Let is be the value of the "is" attribute in token, if such an attribute exists; otherwise null.
    auto is_value = token.attribute(AttributeNames::is);

    // 6. Let registry be the result of looking up a custom element registry given intendedParent.
    auto registry = look_up_a_custom_element_registry(intended_parent);

    // 7. Let definition be the result of looking up a custom element definition given registry, namespace, localName,
    //    and is.
    auto definition = look_up_a_custom_element_definition(registry, namespace_, local_name, is_value);

    // 8. Let willExecuteScript be true if definition is non-null and the parser was not created as part of the HTML
    //    fragment parsing algorithm; otherwise false.
    bool will_execute_script = definition && !m_parsing_fragment;

    // 9. If willExecuteScript is true:
    if (will_execute_script) {
        // 1. Increment document's throw-on-dynamic-markup-insertion counter.
        document->increment_throw_on_dynamic_markup_insertion_counter({});

        // 2. If the JavaScript execution context stack is empty, then perform a microtask checkpoint.
        auto& vm = main_thread_event_loop().vm();
        if (!vm.has_running_execution_context())
            perform_a_microtask_checkpoint();

        // 3. Push a new element queue onto document's relevant agent's custom element reactions stack.
        relevant_similar_origin_window_agent(document).custom_element_reactions_stack.element_queue_stack.append({});
    }

    // 10. Let element be the result of creating an element given document, localName, namespace, null, is,
    //     willExecuteScript, and registry.
    auto element = create_element(*document, local_name, namespace_, {}, is_value, will_execute_script, registry).release_value_but_fixme_should_propagate_errors();

    // AD-HOC: See AD-HOC comment on Element.m_had_duplicate_attribute_during_tokenization about why this is done.
    if (token.had_duplicate_attribute()) {
        element->set_had_duplicate_attribute_during_tokenization({});
    }

    // AD-HOC: Let <link> elements know which document they were originally parsed for.
    //         This is used for the render-blocking logic.
    if (local_name == HTML::TagNames::link && namespace_ == Namespace::HTML) {
        auto& link_element = as<HTMLLinkElement>(*element);
        link_element.set_parser_document({}, document);
        link_element.set_was_enabled_when_created_by_parser({}, !token.has_attribute(HTML::AttributeNames::disabled));
    }

    // 11. Append each attribute in the given token to element.
    token.for_each_attribute([&](auto const& attribute) {
        DOM::QualifiedName qualified_name { attribute.local_name, attribute.prefix, attribute.namespace_ };
        auto dom_attribute = realm().create<DOM::Attr>(*document, move(qualified_name), attribute.value, element);
        element->append_attribute(dom_attribute);
        return IterationDecision::Continue;
    });

    // AD-HOC: The muted attribute on media elements is only set if the muted content attribute is present when the element is first created.
    if (element->is_html_media_element() && namespace_ == Namespace::HTML) {
        // https://html.spec.whatwg.org/multipage/media.html#user-interface:attr-media-muted
        // When a media element is created, if the element has a muted content attribute specified, then the muted IDL
        // attribute should be set to true; otherwise, the user agents may set the value to the user's preferred value.
        if (element->has_attribute(HTML::AttributeNames::muted)) {
            auto& media_element = as<HTMLMediaElement>(*element);
            media_element.set_muted(true);
        }
    }

    // 12. If willExecuteScript is true:
    if (will_execute_script) {
        // 1. Let queue be the result of popping from document's relevant agent's custom element reactions stack.
        //    (This will be the same element queue as was pushed above.)
        auto queue = relevant_similar_origin_window_agent(document).custom_element_reactions_stack.element_queue_stack.take_last();

        // 2. Invoke custom element reactions in queue.
        Bindings::invoke_custom_element_reactions(queue);

        // 3. Decrement document's throw-on-dynamic-markup-insertion counter.
        document->decrement_throw_on_dynamic_markup_insertion_counter({});
    }

    // FIXME: 13. If element has an xmlns attribute in the XMLNS namespace whose value is not exactly the same as the element's namespace, that is a parse error.
    //            Similarly, if element has an xmlns:xlink attribute in the XMLNS namespace whose value is not the XLink Namespace, that is a parse error.

    if (auto* html_element = as_if<HTML::HTMLElement>(*element)) {
        if (html_element->is_form_associated_element() && !html_element->is_form_associated_custom_element()) {
            // 14. If element is a resettable element and not a form-associated custom element, then invoke its reset algorithm.
            //     (This initializes the element's value and checkedness based on the element's attributes.)
            if (html_element->is_resettable())
                html_element->reset_algorithm();
        }
    }

    // 16. Return element.
    return element;
}

void HTMLParser::schedule_resume_check()
{
    if (m_resume_check_pending)
        return;
    if (!m_parser_pause_flag)
        return;
    m_resume_check_pending = true;
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this] {
        m_resume_check_pending = false;
        perform_pre_progress_microtask_checkpoint();
        resume_after_parser_blocking_script();
    }));
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-incdata
// Async equivalent of "spin the event loop until ... ready to be parser-executed" from the per-iteration block of the
// "text" insertion mode (steps 4-13). Driven by schedule_resume_check.
void HTMLParser::resume_after_parser_blocking_script()
{
    if (!m_parser_pause_flag)
        return;
    if (m_aborted || m_stop_parsing)
        return;

    auto pending = document().pending_parsing_blocking_script();
    auto pending_svg = document().pending_parsing_blocking_svg_script();
    bool ready = false;
    if (pending)
        ready = pending->is_ready_to_be_parser_executed();
    else if (pending_svg)
        ready = pending_svg->is_ready_to_be_parser_executed();
    else
        return;

    // 5. If the parser's Document has a style sheet that is blocking scripts or the script's ready to be
    //    parser-executed is false: spin the event loop until the parser's Document has no style sheet that is blocking
    //    scripts and the script's ready to be parser-executed becomes true.
    // The async equivalent: return without taking the script; schedule_resume_check re-fires this method when the
    // relevant state changes.
    if (m_document->has_a_style_sheet_that_is_blocking_scripts())
        return;
    if (!ready)
        return;

    // 3. Start the speculative HTML parser for this instance of the HTML parser.
    // (Done at the pause point in the corresponding insertion-mode handler, so that speculation runs during the wait.)

    // 4. Block the tokenizer for this instance of the HTML parser, such that the event loop will not run tasks that
    //    invoke the tokenizer.
    // (No-op: pausing is expressed by returning from run() and m_parser_pause_flag, not a tokenizer-level block flag.)

    // 6. If this parser has been aborted in the meantime, return.
    if (m_aborted)
        return;

    // 7. Stop the speculative HTML parser for this instance of the HTML parser.
    stop_the_speculative_html_parser();

    // 8. Unblock the tokenizer for this instance of the HTML parser, such that tasks that invoke the tokenizer can
    //    again be run. (No-op, see step 4.)

    // 9. Let the insertion point be just before the next input character.
    m_tokenizer.update_insertion_point();

    // 10. Increment the parser's script nesting level by one (it should be zero before this step, so this sets it to
    //     one).
    VERIFY(script_nesting_level() == 0);
    increment_script_nesting_level();

    // Step 8 unblocked the tokenizer above. Our async "spin the event loop" implementation uses the parser pause flag
    // to yield while waiting for the pending script, so clear it before executing the script. This allows
    // document.write() calls made by the script to synchronously re-enter the parser up to the insertion point.
    m_parser_pause_flag = false;

    // 1. Let the script be the pending parsing-blocking script.
    // 2. Set the pending parsing-blocking script to null.
    // 11. Execute the script element the script.
    if (pending)
        document().take_pending_parsing_blocking_script({})->execute_script();
    else
        document().take_pending_parsing_blocking_svg_script({})->execute_pending_parser_blocking_script({});

    // 12. Decrement the parser's script nesting level by one.
    decrement_script_nesting_level();

    // If the parser's script nesting level is zero (which it always should be at this point), then set the parser pause
    // flag to false.
    VERIFY(script_nesting_level() == 0);
    m_parser_pause_flag = false;

    // 13. Let the insertion point be undefined again.
    m_tokenizer.undefine_insertion_point();

    // The spec's loop would handle the next pending parsing-blocking script before continuing normal tokenization.
    // In this async implementation, pause again and resume when that next script is ready.
    if (document().has_pending_parsing_blocking_script()) {
        m_parser_pause_flag = true;
        schedule_resume_check();
        return;
    }

    // The spec's "While the pending parsing-blocking script is not null" iteration is realized by run() pausing again
    // on the next </script> end tag if the executed script set up a new pending blocking script (e.g. via
    // document.write).
    run();

    if (m_parser_pause_flag)
        return;

    invoke_post_parse_action();
}

void HTMLParser::invoke_post_parse_action()
{
    if (auto action = exchange(m_post_parse_action, nullptr))
        action();
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

DOM::Document& HTMLParser::document()
{
    return *m_document;
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-html-fragments
WebIDL::ExceptionOr<Vector<GC::Root<DOM::Node>>> HTMLParser::parse_html_fragment(DOM::Element& context_element, StringView markup, AllowDeclarativeShadowRoots allow_declarative_shadow_roots, ParserScriptingMode scripting_mode)
{
    // 1. Assert: scriptingMode is either Inert or Fragment.
    VERIFY(scripting_mode == HTML::ParserScriptingMode::Inert || scripting_mode == HTML::ParserScriptingMode::Fragment);

    // 2. Let document be a Document node whose type is "html".
    auto temp_document = DOM::Document::create_for_fragment_parsing(context_element.realm());
    temp_document->set_document_type(DOM::Document::Type::HTML);

    // AD-HOC: We set the about base URL of the document to the same as the context element's document.
    //         This is required for Document::parse_url() to work inside iframe srcdoc documents.
    //         Spec issue: https://github.com/whatwg/html/issues/12210
    temp_document->set_about_base_url(context_element.document().about_base_url());

    // 3. Let contextDocument be context's node document.
    auto& context_document = context_element.document();

    // 4. If contextDocument is in quirks mode, then set document's mode to "quirks".
    if (context_document.in_quirks_mode()) {
        temp_document->set_quirks_mode(DOM::QuirksMode::Yes);
    }
    // 5. Otherwise, if context's node document is in limited-quirks mode, then set document's mode to "limited-quirks".
    else if (context_element.document().in_limited_quirks_mode()) {
        temp_document->set_quirks_mode(DOM::QuirksMode::Limited);
    }

    // 6. If allowDeclarativeShadowRoots is true, then set document's allow declarative shadow roots to true.
    if (allow_declarative_shadow_roots == AllowDeclarativeShadowRoots::Yes)
        temp_document->set_allow_declarative_shadow_roots(true);

    // 7. Create a new HTML parser, and associate it with document.
    // 8. If contextDocument's scripting is disabled, then set scriptingMode to Disabled.
    // 9. Set the parser's scripting mode to scriptingMode.
    if (context_element.document().is_scripting_disabled())
        scripting_mode = HTML::ParserScriptingMode::Disabled;

    auto parser = HTMLParser::create(*temp_document, markup, scripting_mode, "utf-8"sv);
    parser->m_context_element = context_element;
    parser->m_parsing_fragment = true;

    // 10. Set the state of the HTML parser's tokenization stage as follows, switching on the context element:
    bool const context_element_is_html = context_element.namespace_uri() == Namespace::HTML;
    // - title
    // - textarea
    if (context_element_is_html
        && context_element.local_name().is_one_of(HTML::TagNames::title, HTML::TagNames::textarea)) {
        // Switch the tokenizer to the RCDATA state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::RCDATA);
    }
    // - style
    // - xmp
    // - iframe
    // - noembed
    // - noframes
    else if (context_element_is_html
        && context_element.local_name().is_one_of(HTML::TagNames::style, HTML::TagNames::xmp, HTML::TagNames::iframe, HTML::TagNames::noembed, HTML::TagNames::noframes)) {
        // Switch the tokenizer to the RAWTEXT state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::RAWTEXT);
    }
    // - script
    else if (context_element_is_html && context_element.local_name().is_one_of(HTML::TagNames::script)) {
        // Switch the tokenizer to the script data state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::ScriptData);
    }
    // - noscript
    else if (context_element_is_html && context_element.local_name().is_one_of(HTML::TagNames::noscript)) {
        // If scripting mode is not Disabled, switch the tokenizer to the RAWTEXT state. Otherwise, leave the tokenizer in the data state.
        if (scripting_mode != HTML::ParserScriptingMode::Disabled)
            parser->m_tokenizer.switch_to(HTMLTokenizer::State::RAWTEXT);
    }
    // - plaintext
    else if (context_element_is_html && context_element.local_name().is_one_of(HTML::TagNames::plaintext)) {
        // Switch the tokenizer to the PLAINTEXT state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::PLAINTEXT);
    }
    // Any other element
    else {
        // Leave the tokenizer in the data state.
    }

    // 11. Let root be the result of creating an element given document, "html", the HTML namespace, null, null, false,
    //    and context's custom element registry.
    auto root = MUST(create_element(context_element.document(), HTML::TagNames::html, Namespace::HTML, {}, {}, false, context_element.custom_element_registry()));

    // 12. Append root to document.
    MUST(temp_document->append_child(root));

    // 17. Set the HTML parser's form element pointer to the nearest node to context that is a form element
    //     (going straight up the ancestor chain, and including the element itself, if it is a form element), if any.
    //     (If there is no such form element, the form element pointer keeps its initial value, null.)
    parser->m_form_element = as_if<HTMLFormElement>(context_element);
    if (!parser->m_form_element)
        parser->m_form_element = context_element.first_ancestor_of_type<HTMLFormElement>();

    auto context_local_name = context_element.local_name().bytes_as_string_view();
    auto context_namespace = context_element.namespace_uri();
    auto context_namespace_ffi = namespace_to_html_parser_ffi(context_namespace);
    StringView context_namespace_uri;
    if (context_namespace_ffi == RustFfiHtmlNamespace::Other && context_namespace.has_value())
        context_namespace_uri = context_namespace->bytes_as_string_view();
    Vector<RustFfiHtmlParserAttribute> context_attributes;
    if (auto attributes = context_element.attributes()) {
        context_attributes.ensure_capacity(attributes->length());
        for (size_t i = 0; i < attributes->length(); ++i) {
            auto const* attribute = attributes->item(i);
            auto local_name = attribute->local_name().bytes_as_string_view();
            auto value = attribute->value().bytes_as_string_view();
            auto prefix = attribute->prefix().map([](auto const& prefix) { return prefix.bytes_as_string_view(); });
            context_attributes.unchecked_append({
                reinterpret_cast<u8 const*>(local_name.characters_without_null_termination()),
                local_name.length(),
                prefix.has_value() ? reinterpret_cast<u8 const*>(prefix->characters_without_null_termination()) : nullptr,
                prefix.has_value() ? prefix->length() : 0,
                attribute_namespace_to_html_parser_ffi(attribute->namespace_uri()),
                reinterpret_cast<u8 const*>(value.characters_without_null_termination()),
                value.length(),
            });
        }
    }
    rust_html_parser_begin_fragment(
        parser->m_rust_parser,
        reinterpret_cast<size_t>(root.ptr()),
        reinterpret_cast<size_t>(&context_element),
        context_namespace_ffi,
        reinterpret_cast<u8 const*>(context_namespace_uri.characters_without_null_termination()),
        context_namespace_uri.length(),
        reinterpret_cast<u8 const*>(context_local_name.characters_without_null_termination()),
        context_local_name.length(),
        context_attributes.data(),
        context_attributes.size(),
        quirks_mode_to_html_parser_ffi(temp_document->mode()),
        parser->m_form_element ? reinterpret_cast<size_t>(parser->m_form_element.ptr()) : 0);

    // 18. Place the input into the input stream for the HTML parser just created. The encoding confidence is irrelevant.
    // 19. Start the HTML parser and let it run until it has consumed all the characters just inserted into the input stream.
    parser->run(context_element.document().url());

    // 20. Return root's children, in tree order.
    Vector<GC::Root<DOM::Node>> children;
    while (GC::Ptr<DOM::Node> child = root->first_child()) {
        MUST(root->remove_child(*child));
        context_element.document().adopt_node(*child);
        children.append(GC::make_root(*child));
    }
    return children;
}

GC::Ref<HTMLParser> HTMLParser::create_for_scripting(DOM::Document& document)
{
    auto scripting_mode = document.is_scripting_enabled() ? ParserScriptingMode::Normal : ParserScriptingMode::Disabled;
    return document.realm().create<HTMLParser>(document, scripting_mode, ScriptCreatedParser::Yes);
}

GC::Ref<HTMLParser> HTMLParser::create_with_open_input_stream(DOM::Document& document)
{
    auto scripting_mode = document.is_scripting_enabled() ? ParserScriptingMode::Normal : ParserScriptingMode::Disabled;
    return document.realm().create<HTMLParser>(document, scripting_mode, ScriptCreatedParser::No);
}

GC::Ref<HTMLParser> HTMLParser::create_with_uncertain_encoding(DOM::Document& document, ByteBuffer const& input, Optional<MimeSniff::MimeType> maybe_mime_type)
{
    auto scripting_mode = document.is_scripting_enabled() ? ParserScriptingMode::Normal : ParserScriptingMode::Disabled;
    if (document.has_encoding())
        return document.realm().create<HTMLParser>(document, scripting_mode, input, document.encoding().value().to_byte_string());
    auto encoding = run_encoding_sniffing_algorithm(document, input, maybe_mime_type);
    dbgln_if(HTML_PARSER_DEBUG, "The encoding sniffing algorithm returned encoding '{}'", encoding);
    return document.realm().create<HTMLParser>(document, scripting_mode, input, encoding);
}

GC::Ref<HTMLParser> HTMLParser::create(DOM::Document& document, StringView input, ParserScriptingMode scripting_mode, StringView encoding)
{
    return document.realm().create<HTMLParser>(document, scripting_mode, input, encoding);
}

enum class AttributeMode {
    No,
    Yes,
};

template<OneOf<Utf8View, Utf16View> ViewType>
static String escape_string(ViewType const& string, AttributeMode attribute_mode)
{
    // https://html.spec.whatwg.org/multipage/parsing.html#escapingString
    StringBuilder builder;
    for (auto code_point : string) {
        // 1. Replace any occurrence of the "&" character by the string "&amp;".
        if (code_point == '&')
            builder.append("&amp;"sv);
        // 2. Replace any occurrences of the U+00A0 NO-BREAK SPACE character by the string "&nbsp;".
        else if (code_point == 0xA0)
            builder.append("&nbsp;"sv);
        // 3. Replace any occurrences of the "<" character by the string "&lt;".
        else if (code_point == '<')
            builder.append("&lt;"sv);
        // 4. Replace any occurrences of the ">" character by the string "&gt;".
        else if (code_point == '>')
            builder.append("&gt;"sv);
        // 5. If the algorithm was invoked in the attribute mode, then replace any occurrences of the """ character by the string "&quot;".
        else if (code_point == '"' && attribute_mode == AttributeMode::Yes)
            builder.append("&quot;"sv);
        else
            builder.append_code_point(code_point);
    }
    return builder.to_string_without_validation();
}

// https://html.spec.whatwg.org/multipage/parsing.html#html-fragment-serialisation-algorithm
String HTMLParser::serialize_html_fragment(DOM::Node const& node, SerializableShadowRoots serializable_shadow_roots, Vector<GC::Root<DOM::ShadowRoot>> const& shadow_roots, DOM::FragmentSerializationMode fragment_serialization_mode)
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
            builder.append(escape_string(element.is_value().value().code_points(), AttributeMode::Yes));
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
            // -> If the attribute has no namespace:
            if (!attribute.namespace_uri().has_value()) {
                // The attribute's serialized name is the attribute's local name.
                builder.append(attribute.local_name());
            }
            // -> If the attribute is in the XML namespace:
            else if (attribute.namespace_uri() == Namespace::XML) {
                // The attribute's serialized name is the string "xml:" followed by the attribute's local name.
                builder.append("xml:"sv);
                builder.append(attribute.local_name());
            }
            // -> If the attribute is in the XMLNS namespace and the attribute's local name is xmlns:
            else if (attribute.namespace_uri() == Namespace::XMLNS && attribute.local_name() == "xmlns") {
                // The attribute's serialized name is the string "xmlns".
                builder.append("xmlns"sv);
            }
            // -> If the attribute is in the XMLNS namespace and the attribute's local name is not xmlns:
            else if (attribute.namespace_uri() == Namespace::XMLNS) {
                // The attribute's serialized name is the string "xmlns:" followed by the attribute's local name.
                builder.append("xmlns:"sv);
                builder.append(attribute.local_name());
            }
            // -> If the attribute is in the XLink namespace:
            else if (attribute.namespace_uri() == Namespace::XLink) {
                // The attribute's serialized name is the string "xlink:" followed by the attribute's local name.
                builder.append("xlink:"sv);
                builder.append(attribute.local_name());
            }
            // -> If the attribute is in some other namespace:
            else {
                // The attribute's serialized name is the attribute's qualified name.
                builder.append(attribute.name());
            }

            builder.append("=\""sv);
            builder.append(escape_string(attribute.value().code_points(), AttributeMode::Yes));
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
        serialize_element(as<DOM::Element>(node));
        return builder.to_string_without_validation();
    }

    // The algorithm takes as input a DOM Element, Document, or DocumentFragment referred to as the node.
    VERIFY(node.is_element() || node.is_document() || node.is_document_fragment());
    GC::Ref<DOM::Node const> actual_node = node;

    if (is<DOM::Element>(node)) {
        auto const& element = as<DOM::Element>(node);

        // 1. If the node serializes as void, then return the empty string.
        //    (NOTE: serializes as void is defined only on elements in the spec)
        if (element.serializes_as_void())
            return String {};

        // 3. If the node is a template element, then let the node instead be the template element's template contents (a DocumentFragment node).
        //    (NOTE: This is out of order of the spec to avoid another dynamic cast. The second step just creates a string builder, so it shouldn't matter)
        if (is<HTML::HTMLTemplateElement>(element))
            actual_node = as<HTML::HTMLTemplateElement>(element).content();

        // 4. If current node is a shadow host, then:
        if (element.is_shadow_host()) {
            // 1. Let shadow be current node's shadow root.
            auto shadow = element.shadow_root();

            // 2. If one of the following is true:
            //    - serializableShadowRoots is true and shadow's serializable is true; or
            //    - shadowRoots contains shadow,
            if ((serializable_shadow_roots == SerializableShadowRoots::Yes && shadow->serializable())
                || shadow_roots.contains([&](auto& entry) { return entry == shadow; })) {
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

                // 6. If shadow's slot assignment is "manual", then append " shadowrootslotassignment="manual"".
                if (shadow->slot_assignment() == Bindings::SlotAssignmentMode::Manual)
                    builder.append(" shadowrootslotassignment=\"manual\""sv);

                // 7. If shadow's clonable is set, then append " shadowrootclonable=""".
                if (shadow->clonable())
                    builder.append(" shadowrootclonable=\"\""sv);

                // 7. Let shouldAppendRegistryAttribute be the result of running these steps:
                auto should_append_registry_attribute = [&] {
                    // 1. Let documentRegistry be shadow's node document's custom element registry.
                    auto document_registry = shadow->document().custom_element_registry();

                    // 2. Let shadowRegistry be shadow's custom element registry.
                    auto shadow_registry = shadow->custom_element_registry();

                    // 3. If documentRegistry is null and shadowRegistry is null, then return false.
                    if (!document_registry && !shadow_registry)
                        return false;

                    // 4. If documentRegistry is a global custom element registry and shadowRegistry is a global custom
                    //    element registry, then return false.
                    if (is_a_global_custom_element_registry(document_registry) && is_a_global_custom_element_registry(shadow_registry))
                        return false;

                    // 5. Return true.
                    return true;
                }();

                // 8. If shouldAppendRegistryAttribute is true, then append " shadowrootcustomelementregistry=""".
                if (should_append_registry_attribute)
                    builder.append(" shadowrootcustomelementregistry=\"\""sv);

                // 9. Append ">".
                builder.append('>');

                // 10. Append the value of running the HTML fragment serialization algorithm with shadow,
                //    serializableShadowRoots, and shadowRoots (thus recursing into this algorithm for that element).
                builder.append(serialize_html_fragment(*shadow, serializable_shadow_roots, shadow_roots));

                // 11. Append "</template>".
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
            auto& element = as<DOM::Element>(current_node);
            serialize_element(element);
            return IterationDecision::Continue;
        }

        if (is<DOM::Text>(current_node)) {
            // -> If current node is a Text node
            auto& text_node = as<DOM::Text>(current_node);
            auto* parent = current_node.parent();

            if (is<DOM::Element>(parent)) {
                auto& parent_element = as<DOM::Element>(*parent);

                // If the parent of current node is a style, script, xmp, iframe, noembed, noframes, or plaintext element,
                // or if the parent of current node is a noscript element and scripting is enabled for the node, then append the value of current node's data IDL attribute literally.
                if (parent_element.local_name().is_one_of(HTML::TagNames::style, HTML::TagNames::script, HTML::TagNames::xmp, HTML::TagNames::iframe, HTML::TagNames::noembed, HTML::TagNames::noframes, HTML::TagNames::plaintext)
                    || (parent_element.local_name() == HTML::TagNames::noscript && !parent_element.is_scripting_disabled())) {
                    builder.append(text_node.data());
                    return IterationDecision::Continue;
                }
            }

            // Otherwise, append the value of current node's data IDL attribute, escaped as described below.
            builder.append(escape_string(text_node.data().utf16_view(), AttributeMode::No));
        }

        if (is<DOM::Comment>(current_node)) {
            // -> If current node is a Comment
            auto& comment_node = as<DOM::Comment>(current_node);

            // Append the literal string "<!--" (U+003C LESS-THAN SIGN, U+0021 EXCLAMATION MARK, U+002D HYPHEN-MINUS, U+002D HYPHEN-MINUS),
            // followed by the value of current node's data IDL attribute, followed by the literal string "-->" (U+002D HYPHEN-MINUS, U+002D HYPHEN-MINUS, U+003E GREATER-THAN SIGN).
            builder.append("<!--"sv);
            builder.append(comment_node.data());
            builder.append("-->"sv);
            return IterationDecision::Continue;
        }

        if (is<DOM::ProcessingInstruction>(current_node)) {
            // -> If current node is a ProcessingInstruction
            auto& processing_instruction_node = as<DOM::ProcessingInstruction>(current_node);

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
            auto& document_type_node = as<DOM::DocumentType>(current_node);

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
static RefPtr<CSS::StyleValue const> parse_current_dimension_value(float value, Utf8View input, Utf8View::Iterator position)
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
RefPtr<CSS::StyleValue const> parse_dimension_value(StringView string)
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
    auto integer_value = number_string.string_view().to_number<double>();

    float value = min(*integer_value, CSSPixels::max_dimension_value);

    // 6. If position is past the end of input, then return value as a length.
    if (position == input.end())
        return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels(value)));

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
RefPtr<CSS::StyleValue const> parse_nonzero_dimension_value(StringView string)
{
    // 1. Let input be the string being parsed.
    // 2. Let value be the result of parsing input using the rules for parsing dimension values.
    auto value = parse_dimension_value(string);

    // 3. If value is an error, return an error.
    if (!value)
        return nullptr;

    // 4. If value is zero, return an error.
    if (value->is_length() && value->as_length().raw_value() == 0)
        return nullptr;
    if (value->is_percentage() && value->as_percentage().percentage().value() == 0)
        return nullptr;

    // 5. If value is a percentage, return value as a percentage.
    // 6. Return value as a length.
    return value;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-a-legacy-colour-value
Optional<Color> parse_legacy_color_value(StringView string_view)
{
    // 1. If input is the empty string, then return failure.
    if (string_view.is_empty())
        return {};

    ByteString input = string_view;

    // 2. Strip leading and trailing ASCII whitespace from input.
    input = input.trim(Infra::ASCII_WHITESPACE);

    // 3. If input is an ASCII case-insensitive match for "transparent", then return failure.
    if (input.equals_ignoring_ascii_case("transparent"sv))
        return {};

    // 4. If input is an ASCII case-insensitive match for one of the named colors, then return the CSS color corresponding to that keyword. [CSSCOLOR]
    if (auto const color = Color::from_named_css_color_string(input); color.has_value())
        return color;

    auto hex_nibble_to_u8 = [](char nibble) -> u8 {
        if (nibble >= '0' && nibble <= '9')
            return nibble - '0';
        if (nibble >= 'a' && nibble <= 'f')
            return nibble - 'a' + 10;
        return nibble - 'A' + 10;
    };

    // 5. If input's code point length is four, and the first character in input is U+0023 (#), and the last three characters of input are all ASCII hex digits, then:
    if (input.length() == 4 && input[0] == '#' && is_ascii_hex_digit(input[1]) && is_ascii_hex_digit(input[2]) && is_ascii_hex_digit(input[3])) {
        // 1. Let result be a CSS color.
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

    // 6. Replace any code points greater than U+FFFF in input (i.e., any characters that are not in the basic multilingual plane) with "00".
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

    // 7. If input's code point length is greater than 128, truncate input, leaving only the first 128 characters.
    if (input.length() > 128)
        input = input.substring(0, 128);

    // 8. If the first character in input is U+0023 (#), then remove it.
    if (input[0] == '#')
        input = input.substring(1);

    // 9. Replace any character in input that is not an ASCII hex digit with U+0030 (0).
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

    // 10. While input's code point length is zero or not a multiple of three, append U+0030 (0) to input.
    StringBuilder builder;
    builder.append(input);
    while (builder.length() == 0 || (builder.length() % 3 != 0))
        builder.append_code_point('0');
    input = builder.to_byte_string();

    // 11. Split input into three strings of equal code point length, to obtain three components. Let length be the code point length that all of those components have (one third the code point length of input).
    auto length = input.length() / 3;
    auto first_component = input.substring_view(0, length);
    auto second_component = input.substring_view(length, length);
    auto third_component = input.substring_view(length * 2, length);

    // 12. If length is greater than 8, then remove the leading length-8 characters in each component, and let length be 8.
    if (length > 8) {
        first_component = first_component.substring_view(length - 8);
        second_component = second_component.substring_view(length - 8);
        third_component = third_component.substring_view(length - 8);
        length = 8;
    }

    // 13. While length is greater than two and the first character in each component is U+0030 (0), remove that character and reduce length by one.
    while (length > 2 && first_component[0] == '0' && second_component[0] == '0' && third_component[0] == '0') {
        --length;
        first_component = first_component.substring_view(1);
        second_component = second_component.substring_view(1);
        third_component = third_component.substring_view(1);
    }

    // 14. If length is still greater than two, truncate each component, leaving only the first two characters in each.
    if (length > 2) {
        first_component = first_component.substring_view(0, 2);
        second_component = second_component.substring_view(0, 2);
        third_component = third_component.substring_view(0, 2);
    }

    auto to_hex = [&](StringView string) -> u8 {
        if (length == 1) {
            return hex_nibble_to_u8(string[0]);
        }
        auto nib1 = hex_nibble_to_u8(string[0]);
        auto nib2 = hex_nibble_to_u8(string[1]);
        return nib1 << 4 | nib2;
    };

    // 15. Let result be a CSS color.
    Color result;
    result.set_alpha(0xFF);

    // 16. Interpret the first component as a hexadecimal number; let the red component of result be the resulting number.
    result.set_red(to_hex(first_component));

    // 17. Interpret the second component as a hexadecimal number; let the green component of result be the resulting number.
    result.set_green(to_hex(second_component));

    // 18. Interpret the third component as a hexadecimal number; let the blue component of result be the resulting number.
    result.set_blue(to_hex(third_component));

    // 19. Return result.
    return result;
}

// https://html.spec.whatwg.org/multipage/rendering.html#tables-2
RefPtr<CSS::StyleValue const> parse_table_child_element_align_value(StringView string_view)
{
    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for either the string "center" or the string "middle", are expected to center text within
    // themselves, as if they had their 'text-align' property set to 'center' in a presentational hint, and to align
    // descendants to the center.
    if (string_view.equals_ignoring_ascii_case("center"sv) || string_view.equals_ignoring_ascii_case("middle"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::LibwebCenter);

    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for the string "left", are expected to left-align text within themselves, as if they had
    // their 'text-align' property set to 'left' in a presentational hint, and to align descendants to the left.
    if (string_view.equals_ignoring_ascii_case("left"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::LibwebLeft);

    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for the string "right", are expected to right-align text within themselves, as if they
    // had their 'text-align' property set to 'right' in a presentational hint, and to align descendants to the right.
    if (string_view.equals_ignoring_ascii_case("right"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::LibwebRight);

    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for the string "justify", are expected to full-justify text within themselves, as if they
    // had their 'text-align' property set to 'justify' in a presentational hint, and to align descendants to the left.
    if (string_view.equals_ignoring_ascii_case("justify"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::Justify);

    return nullptr;
}

JS::Realm& HTMLParser::realm()
{
    return m_document->realm();
}

// https://html.spec.whatwg.org/multipage/parsing.html#start-the-speculative-html-parser
void HTMLParser::start_the_speculative_html_parser()
{
    // 1. Optionally, return.
    // NOTE: We do not opt out.

    // 2. If parser's active speculative HTML parser is not null, then stop the speculative HTML parser for parser.
    if (m_active_speculative_html_parser)
        stop_the_speculative_html_parser();

    // 3. Let speculativeParser be a new speculative HTML parser, with the same state as parser.
    // 4. Let speculativeDoc be a new isomorphic representation of parser's Document, where all elements are instead
    //    speculative mock elements. Let speculativeParser parse into speculativeDoc.
    // NOTE: Speculative mock elements are produced on the fly during run(); we do not materialize a full speculativeDoc tree.
    auto speculative_parser = SpeculativeHTMLParser::create(realm(), *m_document, m_tokenizer.unparsed_input(), m_document->base_url());

    // 5. Set parser's active speculative HTML parser to speculativeParser.
    m_active_speculative_html_parser = speculative_parser;

    // 6. In parallel, run speculativeParser until it is stopped or until it reaches the end of its input stream.
    speculative_parser->run();
}

// https://html.spec.whatwg.org/multipage/parsing.html#stop-the-speculative-html-parser
void HTMLParser::stop_the_speculative_html_parser()
{
    // 1. Let speculativeParser be parser's active speculative HTML parser.
    auto speculative_parser = m_active_speculative_html_parser;

    // 2. If speculativeParser is null, then return.
    if (!speculative_parser)
        return;

    // 3. Throw away any pending content in speculativeParser's input stream, and discard any future content that would
    //    have been added to it.
    speculative_parser->stop();

    // 4. Set parser's active speculative HTML parser to null.
    m_active_speculative_html_parser = nullptr;
}

// https://html.spec.whatwg.org/multipage/parsing.html#abort-a-parser
void HTMLParser::abort()
{
    // 1. Throw away any pending content in the input stream, and discard any future content that would have been added to it.
    m_tokenizer.abort();

    // 2. Stop the speculative HTML parser for this HTML parser.
    stop_the_speculative_html_parser();

    // 3. Update the current document readiness to "interactive".
    m_document->update_readiness(DocumentReadyState::Interactive);

    // 4. Pop all the nodes off the stack of open elements.
    pop_all_open_elements();

    // 5. Update the current document readiness to "complete".
    m_document->update_readiness(DocumentReadyState::Complete);

    m_aborted = true;
}

static StringView html_parser_ffi_string_view(u8 const* ptr, size_t len)
{
    if (ptr == nullptr || len == 0)
        return {};
    return { ptr, len };
}

static FlyString fly_string_from_html_parser_ffi(u8 const* ptr, size_t len)
{
    return MUST(FlyString::from_utf8(html_parser_ffi_string_view(ptr, len)));
}

static String string_from_html_parser_ffi(u8 const* ptr, size_t len)
{
    return MUST(String::from_utf8(html_parser_ffi_string_view(ptr, len)));
}

extern "C" void ladybird_html_parser_log_parse_error(void* parser, u8 const* message_ptr, size_t message_len)
{
    (void)parser_from_html_parser_ffi(parser);
    dbgln_if(HTML_PARSER_DEBUG, "Rust parser parse error: {}", html_parser_ffi_string_view(message_ptr, message_len));
}

extern "C" void ladybird_html_parser_stop_parsing(void* parser)
{
    parser_from_html_parser_ffi(parser).stop_parsing_from_rust_parser();
}

extern "C" bool ladybird_html_parser_parse_errors_enabled()
{
    return HTML_PARSER_DEBUG;
}

extern "C" void ladybird_html_parser_visit_node(void* visitor, size_t node)
{
    if (node == 0)
        return;
    static_cast<GC::Cell::Visitor*>(visitor)->visit(node_from_html_parser_ffi(node));
}

static Optional<FlyString> namespace_from_html_parser_ffi(RustFfiHtmlNamespace namespace_, u8 const* namespace_uri_ptr, size_t namespace_uri_len)
{
    switch (namespace_) {
    case RustFfiHtmlNamespace::Html:
        return Namespace::HTML;
    case RustFfiHtmlNamespace::MathMl:
        return Namespace::MathML;
    case RustFfiHtmlNamespace::Svg:
        return Namespace::SVG;
    case RustFfiHtmlNamespace::Other:
        if (namespace_uri_len == 0)
            return {};
        return fly_string_from_html_parser_ffi(namespace_uri_ptr, namespace_uri_len);
    }
    VERIFY_NOT_REACHED();
}

static Optional<FlyString> attribute_namespace_from_html_parser_ffi(RustFfiHtmlAttributeNamespace namespace_)
{
    switch (namespace_) {
    case RustFfiHtmlAttributeNamespace::None:
        return {};
    case RustFfiHtmlAttributeNamespace::XLink:
        return Namespace::XLink;
    case RustFfiHtmlAttributeNamespace::Xml:
        return Namespace::XML;
    case RustFfiHtmlAttributeNamespace::Xmlns:
        return Namespace::XMLNS;
    case RustFfiHtmlAttributeNamespace::Other:
        // Only fragment context attributes use this sentinel; parser-created attributes do not cross this path with
        // arbitrary namespace URIs.
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

static RustFfiHtmlAttributeNamespace attribute_namespace_to_html_parser_ffi(Optional<FlyString> const& namespace_)
{
    if (namespace_ == Namespace::XLink)
        return RustFfiHtmlAttributeNamespace::XLink;
    if (namespace_ == Namespace::XML)
        return RustFfiHtmlAttributeNamespace::Xml;
    if (namespace_ == Namespace::XMLNS)
        return RustFfiHtmlAttributeNamespace::Xmlns;
    if (namespace_.has_value())
        return RustFfiHtmlAttributeNamespace::Other;
    return RustFfiHtmlAttributeNamespace::None;
}

static RustFfiHtmlNamespace namespace_to_html_parser_ffi(Optional<FlyString> const& namespace_)
{
    if (!namespace_.has_value())
        return RustFfiHtmlNamespace::Other;
    if (namespace_ == Namespace::HTML)
        return RustFfiHtmlNamespace::Html;
    if (namespace_ == Namespace::MathML)
        return RustFfiHtmlNamespace::MathMl;
    if (namespace_ == Namespace::SVG)
        return RustFfiHtmlNamespace::Svg;
    return RustFfiHtmlNamespace::Other;
}

static DOM::QuirksMode quirks_mode_from_html_parser_ffi(RustFfiHtmlQuirksMode mode)
{
    switch (mode) {
    case RustFfiHtmlQuirksMode::No:
        return DOM::QuirksMode::No;
    case RustFfiHtmlQuirksMode::Limited:
        return DOM::QuirksMode::Limited;
    case RustFfiHtmlQuirksMode::Yes:
        return DOM::QuirksMode::Yes;
    }
    VERIFY_NOT_REACHED();
}

static RustFfiHtmlQuirksMode quirks_mode_to_html_parser_ffi(DOM::QuirksMode mode)
{
    switch (mode) {
    case DOM::QuirksMode::No:
        return RustFfiHtmlQuirksMode::No;
    case DOM::QuirksMode::Limited:
        return RustFfiHtmlQuirksMode::Limited;
    case DOM::QuirksMode::Yes:
        return RustFfiHtmlQuirksMode::Yes;
    }
    VERIFY_NOT_REACHED();
}

static HTMLParser& parser_from_html_parser_ffi(void* parser)
{
    VERIFY(parser);
    return *reinterpret_cast<HTMLParser*>(parser);
}

static DOM::Node& node_from_html_parser_ffi(size_t node)
{
    VERIFY(node);
    return *reinterpret_cast<DOM::Node*>(node);
}

extern "C" size_t ladybird_html_parser_document_node(void* parser)
{
    return reinterpret_cast<size_t>(&parser_from_html_parser_ffi(parser).document());
}

extern "C" size_t ladybird_html_parser_document_html_element(void* parser)
{
    auto* html_element = parser_from_html_parser_ffi(parser).document().document_element();
    if (!html_element || !is<HTMLHtmlElement>(*html_element))
        return 0;
    return reinterpret_cast<size_t>(html_element);
}

extern "C" void ladybird_html_parser_set_document_quirks_mode(void* parser, RustFfiHtmlQuirksMode mode)
{
    auto& document = parser_from_html_parser_ffi(parser).document();
    if (!document.parser_cannot_change_the_mode())
        document.set_quirks_mode(quirks_mode_from_html_parser_ffi(mode));
}

extern "C" size_t ladybird_html_parser_create_document_type(void* parser, u8 const* name_ptr, size_t name_len, u8 const* public_id_ptr, size_t public_id_len, u8 const* system_id_ptr, size_t system_id_len)
{
    auto& html_parser = parser_from_html_parser_ffi(parser);
    auto document_type = html_parser.document().realm().create<DOM::DocumentType>(html_parser.document());
    document_type->set_name(string_from_html_parser_ffi(name_ptr, name_len));
    document_type->set_public_id(string_from_html_parser_ffi(public_id_ptr, public_id_len));
    document_type->set_system_id(string_from_html_parser_ffi(system_id_ptr, system_id_len));
    return reinterpret_cast<size_t>(document_type.ptr());
}

extern "C" size_t ladybird_html_parser_create_comment(void* parser, u8 const* data_ptr, size_t data_len)
{
    auto& html_parser = parser_from_html_parser_ffi(parser);
    auto comment = html_parser.document().realm().create<DOM::Comment>(html_parser.document(), Utf16String::from_utf8(string_from_html_parser_ffi(data_ptr, data_len)));
    return reinterpret_cast<size_t>(comment.ptr());
}

extern "C" void ladybird_html_parser_insert_text(size_t parent, size_t before, u8 const* data_ptr, size_t data_len)
{
    auto& parent_node = node_from_html_parser_ffi(parent);
    if (parent_node.is_document())
        return;

    auto data = Utf16String::from_utf8(string_from_html_parser_ffi(data_ptr, data_len));
    if (before) {
        auto& before_node = node_from_html_parser_ffi(before);
        if (auto* previous_text = as_if<DOM::Text>(before_node.previous_sibling())) {
            (void)previous_text->append_data(data);
            return;
        }
        auto text = parent_node.document().realm().create<DOM::Text>(parent_node.document(), data);
        parent_node.insert_before(*text, &before_node);
        return;
    }

    if (auto* last_text = as_if<DOM::Text>(parent_node.last_child())) {
        (void)last_text->append_data(data);
        return;
    }

    auto text = parent_node.document().realm().create<DOM::Text>(parent_node.document(), data);
    MUST(parent_node.append_child(*text));
}

extern "C" void ladybird_html_parser_add_missing_attribute(size_t element, u8 const* local_name_ptr, size_t local_name_len, u8 const* value_ptr, size_t value_len)
{
    auto& dom_element = as<DOM::Element>(node_from_html_parser_ffi(element));
    auto local_name = fly_string_from_html_parser_ffi(local_name_ptr, local_name_len);
    if (dom_element.has_attribute(local_name))
        return;
    dom_element.append_attribute(local_name, string_from_html_parser_ffi(value_ptr, value_len));
}

extern "C" void ladybird_html_parser_remove_node(size_t node)
{
    node_from_html_parser_ffi(node).remove(true);
}

extern "C" void ladybird_html_parser_handle_element_popped(size_t element)
{
    // https://html.spec.whatwg.org/multipage/form-elements.html#the-option-element
    // When an option element is popped off the stack of open elements of an HTML parser or XML parser,
    // the user agent must run maybe clone an option into selectedcontent given the option element.
    // AD-HOC: The Rust tree builder flushes buffered text before invoking this hook, so the option's content is
    // up-to-date before cloning.
    if (auto* option_element = as_if<HTML::HTMLOptionElement>(node_from_html_parser_ffi(element)))
        MUST(option_element->maybe_clone_into_selectedcontent());
}

extern "C" void ladybird_html_parser_prepare_svg_script(void* parser, size_t element, size_t source_line_number)
{
    parser_from_html_parser_ffi(parser).prepare_svg_script_for_rust_parser(as<SVG::SVGScriptElement>(node_from_html_parser_ffi(element)), source_line_number);
}

extern "C" void ladybird_html_parser_set_script_source_line(void* parser, size_t element, size_t source_line_number)
{
    parser_from_html_parser_ffi(parser).set_script_source_line_from_rust_parser(as<DOM::Element>(node_from_html_parser_ffi(element)), source_line_number);
}

extern "C" void ladybird_html_parser_mark_script_already_started(void* parser, size_t element)
{
    if (auto* script = as_if<HTMLScriptElement>(node_from_html_parser_ffi(element)))
        parser_from_html_parser_ffi(parser).mark_script_already_started_from_rust_parser(*script);
}

extern "C" size_t ladybird_html_parser_parent_node(size_t node)
{
    auto* parent = node_from_html_parser_ffi(node).parent();
    return reinterpret_cast<size_t>(parent);
}

extern "C" size_t ladybird_html_parser_create_element(void* parser, size_t intended_parent, RustFfiHtmlNamespace namespace_, u8 const* namespace_uri_ptr, size_t namespace_uri_len, u8 const* local_name_ptr, size_t local_name_len, RustFfiHtmlParserAttribute const* attributes, size_t attribute_count, bool had_duplicate_attribute, size_t form_element, bool has_template_element_on_stack)
{
    auto& html_parser = parser_from_html_parser_ffi(parser);
    auto local_name = fly_string_from_html_parser_ffi(local_name_ptr, local_name_len);
    auto token = HTMLToken::make_start_tag(local_name);

    for (size_t i = 0; i < attribute_count; ++i) {
        auto const& attribute = attributes[i];
        Optional<FlyString> prefix;
        if (attribute.prefix_len != 0)
            prefix = fly_string_from_html_parser_ffi(attribute.prefix_ptr, attribute.prefix_len);
        HTMLToken::Attribute token_attribute;
        token_attribute.prefix = move(prefix);
        token_attribute.local_name = fly_string_from_html_parser_ffi(attribute.local_name_ptr, attribute.local_name_len);
        token_attribute.namespace_ = attribute_namespace_from_html_parser_ffi(attribute.namespace_);
        token_attribute.value = string_from_html_parser_ffi(attribute.value_ptr, attribute.value_len);
        token.add_attribute(move(token_attribute));
    }

    auto& intended_parent_node = node_from_html_parser_ffi(intended_parent);
    GC::Ptr<HTMLFormElement> form_element_ptr;
    if (form_element)
        form_element_ptr = as<HTMLFormElement>(node_from_html_parser_ffi(form_element));
    auto element = html_parser.create_element_for_rust_parser(token, namespace_from_html_parser_ffi(namespace_, namespace_uri_ptr, namespace_uri_len), intended_parent_node, had_duplicate_attribute, form_element_ptr, has_template_element_on_stack);

    return reinterpret_cast<size_t>(element.ptr());
}

extern "C" void ladybird_html_parser_append_child(size_t parent, size_t child)
{
    MUST(node_from_html_parser_ffi(parent).append_child(node_from_html_parser_ffi(child)));
}

extern "C" void ladybird_html_parser_insert_node(size_t parent, size_t before, size_t child, bool queue_custom_element_reactions)
{
    auto& parent_node = node_from_html_parser_ffi(parent);
    auto& child_node = node_from_html_parser_ffi(child);
    auto* child_element = as_if<DOM::Element>(child_node);
    if (queue_custom_element_reactions && child_element)
        relevant_similar_origin_window_agent(*child_element).custom_element_reactions_stack.element_queue_stack.append({});

    if (!before) {
        MUST(parent_node.append_child(child_node));
    } else {
        auto& before_node = node_from_html_parser_ffi(before);
        parent_node.insert_before(child_node, &before_node, false);
    }

    if (queue_custom_element_reactions && child_element) {
        auto queue = relevant_similar_origin_window_agent(*child_element).custom_element_reactions_stack.element_queue_stack.take_last();
        Bindings::invoke_custom_element_reactions(queue);
    }
}

extern "C" void ladybird_html_parser_move_all_children(size_t from, size_t to)
{
    auto& from_node = node_from_html_parser_ffi(from);
    auto& to_node = node_from_html_parser_ffi(to);
    for (auto& child : from_node.children_as_vector())
        MUST(to_node.append_child(from_node.remove_child(*child).release_value()));
}

extern "C" size_t ladybird_html_parser_template_content(size_t element)
{
    auto& template_element = as<HTMLTemplateElement>(node_from_html_parser_ffi(element));
    return reinterpret_cast<size_t>(template_element.content().ptr());
}

extern "C" size_t ladybird_html_parser_attach_declarative_shadow_root(size_t host, RustFfiHtmlShadowRootMode mode, RustFfiHtmlSlotAssignmentMode slot_assignment, bool clonable, bool serializable, bool delegates_focus, bool keep_custom_element_registry_null)
{
    auto& host_element = as<DOM::Element>(node_from_html_parser_ffi(host));
    if (host_element.is_shadow_host())
        return 0;

    GC::Ptr<CustomElementRegistry> registry;
    if (!keep_custom_element_registry_null)
        registry = host_element.document().custom_element_registry();

    auto result = host_element.attach_a_shadow_root(
        mode == RustFfiHtmlShadowRootMode::Open ? Bindings::ShadowRootMode::Open : Bindings::ShadowRootMode::Closed,
        clonable,
        serializable,
        delegates_focus,
        slot_assignment == RustFfiHtmlSlotAssignmentMode::Manual ? Bindings::SlotAssignmentMode::Manual : Bindings::SlotAssignmentMode::Named,
        registry);
    if (result.is_error())
        return 0;

    auto shadow_root = host_element.shadow_root();
    VERIFY(shadow_root);
    shadow_root->set_declarative(true);
    shadow_root->set_available_to_element_internals(true);
    if (keep_custom_element_registry_null)
        shadow_root->set_keep_custom_element_registry_null(true);
    return reinterpret_cast<size_t>(shadow_root.ptr());
}

extern "C" void ladybird_html_parser_set_template_content(size_t element, size_t content)
{
    as<HTMLTemplateElement>(node_from_html_parser_ffi(element)).set_template_contents(as<DOM::DocumentFragment>(node_from_html_parser_ffi(content)));
}

extern "C" bool ladybird_html_parser_allows_declarative_shadow_roots(size_t node)
{
    return node_from_html_parser_ffi(node).document().allow_declarative_shadow_roots();
}

}
