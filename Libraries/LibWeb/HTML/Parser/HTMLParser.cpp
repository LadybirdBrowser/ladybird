/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/FFIHelpers.h>
#include <LibGC/Heap.h>
#include <LibGC/RootVector.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleElementBase.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactions.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/HTMLMetaElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Parser/ParserScriptingMode.h>
#include <LibWeb/HTML/Parser/SpeculativeHTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
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
struct NodeAndOffset;
static NodeAndOffset node_and_offset_from_html_parser_ffi(size_t, size_t);
static HTMLParser& parser_from_html_parser_ffi(void*);
static RustFfiHtmlNamespace namespace_to_html_parser_ffi(Optional<Utf16FlyString> const&);
static RustFfiHtmlAttributeNamespace attribute_namespace_to_html_parser_ffi(Optional<Utf16FlyString> const&);
static RustFfiHtmlQuirksMode quirks_mode_to_html_parser_ffi(DOM::QuirksMode);

static Vector<u16> utf16_code_units_for_ffi(Utf16View view)
{
    Vector<u16> code_units;
    code_units.ensure_capacity(view.length_in_code_units());
    for (size_t i = 0; i < view.length_in_code_units(); ++i)
        code_units.unchecked_append(static_cast<u16>(view.code_unit_at(i)));
    return code_units;
}

static Utf16FlyString utf16_fly_string_from_ffi(u16 const* ptr, size_t len)
{
    if (!ptr || len == 0)
        return {};
    return Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(ptr), len });
}

static Utf16String utf16_string_from_ffi(u16 const* ptr, size_t len)
{
    if (!ptr || len == 0)
        return {};
    return Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(ptr), len });
}

static Utf16String utf8_string_from_ffi(u8 const* ptr, size_t len)
{
    if (len == 0)
        return {};
    return Utf16String::from_utf8_without_validation({ reinterpret_cast<char const*>(ptr), len });
}

static Optional<Utf16FlyString> attribute_namespace_from_html_parser_ffi(RustFfiHtmlAttributeNamespace);

static Utf16String utf16_string_from_standardized_encoding_label(StringView label)
{
    return Utf16String::from_ascii_without_validation(label.bytes());
}

static Utf16String decode_html_parser_input(StringView input, StringView encoding)
{
    auto decoder = TextCodec::decoder_for(encoding);
    VERIFY(decoder.has_value());
    return MUST(TextCodec::convert_input_to_utf16_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, input));
}

extern "C" void ladybird_html_parser_log_parse_error(void*, u8 const*, size_t);
extern "C" void ladybird_html_parser_stop_parsing(void*);
extern "C" bool ladybird_html_parser_parse_errors_enabled();
extern "C" void ladybird_html_parser_visit_node(void*, size_t);
extern "C" size_t ladybird_html_parser_document_node(void*);
extern "C" size_t ladybird_html_parser_document_html_element(void*);
extern "C" void ladybird_html_parser_set_document_quirks_mode(void*, RustFfiHtmlQuirksMode);
extern "C" size_t ladybird_html_parser_create_document_type(void*, u16 const*, size_t, u16 const*, size_t, u16 const*, size_t);
extern "C" size_t ladybird_html_parser_create_comment(void*, u16 const*, size_t);
extern "C" void ladybird_html_parser_insert_text(size_t, size_t, u8 const*, size_t);
extern "C" void ladybird_html_parser_add_missing_attribute(size_t, size_t, u16 const*, size_t);
extern "C" void ladybird_html_parser_remove_node(size_t);
extern "C" void ladybird_html_parser_handle_element_popped(size_t);
extern "C" void ladybird_html_parser_prepare_svg_script(void*, size_t, size_t);
extern "C" void ladybird_html_parser_set_script_source_line(void*, size_t, size_t);
extern "C" void ladybird_html_parser_mark_script_already_started(void*, size_t);
extern "C" void ladybird_html_parser_process_meta_element(void*, size_t);
extern "C" size_t ladybird_html_parser_parent_node(size_t);
extern "C" size_t ladybird_html_parser_node_index(size_t);
extern "C" size_t ladybird_html_parser_create_element(void*, size_t, RustFfiHtmlNamespace, u16 const*, size_t, size_t, RustFfiHtmlParserAttribute const*, size_t, bool, size_t, bool);
extern "C" void ladybird_html_parser_append_child(size_t, size_t);
extern "C" void ladybird_html_parser_insert_node(size_t, size_t, size_t, bool);
extern "C" void ladybird_html_parser_move_all_children(size_t, size_t);
extern "C" size_t ladybird_html_parser_template_content(size_t);
extern "C" size_t ladybird_html_parser_attach_declarative_shadow_root(size_t, RustFfiHtmlShadowRootMode, RustFfiHtmlSlotAssignmentMode, bool, bool, bool, bool);
extern "C" void ladybird_html_parser_set_template_content(size_t, size_t);
extern "C" bool ladybird_html_parser_is_shadow_host(size_t);

HTMLParser::HTMLParser(DOM::Document& document, ParserScriptingMode scripting_mode, StringView input, StringView encoding, EncodingConfidence encoding_confidence)
    : m_tokenizer(decode_html_parser_input(input, encoding))
    , m_scripting_mode(scripting_mode)
    , m_encoding_confidence(encoding_confidence)
    , m_document(document)
{
    m_rust_parser = rust_html_parser_create();
    m_document->set_parser({}, *this);
    auto standardized_encoding = TextCodec::get_standardized_encoding(encoding);
    VERIFY(standardized_encoding.has_value());
    m_document->set_encoding(utf16_string_from_standardized_encoding_label(standardized_encoding.value()));
}

HTMLParser::HTMLParser(DOM::Document& document, ParserScriptingMode scripting_mode, Utf16View input, Utf16View encoding, EncodingConfidence encoding_confidence)
    : m_tokenizer(input)
    , m_scripting_mode(scripting_mode)
    , m_encoding_confidence(encoding_confidence)
    , m_document(document)
{
    m_rust_parser = rust_html_parser_create();
    m_document->set_parser({}, *this);
    auto standardized_encoding = TextCodec::get_standardized_encoding(encoding);
    VERIFY(standardized_encoding.has_value());
    m_document->set_encoding(utf16_string_from_standardized_encoding_label(standardized_encoding.value()));
}

HTMLParser::HTMLParser(DOM::Document& document, ParserScriptingMode scripting_mode, Utf16View input, FragmentParser fragment_parser)
    : m_tokenizer(input)
    , m_parsing_fragment(fragment_parser == FragmentParser::Yes)
    , m_scripting_mode(scripting_mode)
    , m_encoding_confidence(EncodingConfidence::Irrelevant)
    , m_document(document)
{
    VERIFY(m_parsing_fragment);
    m_rust_parser = rust_html_parser_create();
}

HTMLParser::HTMLParser(DOM::Document& document, ParserScriptingMode scripting_mode, ScriptCreatedParser script_created, EncodingConfidence encoding_confidence)
    : m_scripting_mode(scripting_mode)
    , m_script_created(script_created == ScriptCreatedParser::Yes)
    , m_encoding_confidence(encoding_confidence)
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
    visitor.visit(m_root_insertion_target);
    visitor.visit(m_active_speculative_html_parser);
    visitor.visit(m_change_encoding_callback);
    visitor.visit(m_parsing_complete_callback);

    rust_html_parser_visit_edges(m_rust_parser, &visitor);
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
            m_allow_declarative_shadow_roots == AllowDeclarativeShadowRoots::Yes,
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
    if (element.local_name() != HTML::TagNames::script || element.namespace_uri() != Namespace::HTML)
        return;

    auto& script_element = as<HTMLScriptElement>(element);
    if (!m_parsing_fragment && m_scripting_mode != ParserScriptingMode::Fragment)
        script_element.set_parser_document(Badge<HTMLParser> {}, document());
    script_element.set_force_async(Badge<HTMLParser> {}, false);
    if (m_parsing_fragment && m_scripting_mode != ParserScriptingMode::Fragment)
        script_element.set_already_started(Badge<HTMLParser> {}, true);
}

GC::Ref<DOM::Element> HTMLParser::create_element_for_rust_parser(Utf16FlyString const& local_name, ReadonlySpan<RustFfiHtmlParserAttribute> attributes, Optional<Utf16FlyString> const& namespace_, DOM::Node& intended_parent, bool had_duplicate_attribute, GC::Ptr<HTMLFormElement> form_element, bool has_template_element_on_stack)
{
    auto element = create_element_for(local_name, attributes, namespace_, intended_parent);
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
    if (!m_parsing_fragment && document().pending_parsing_blocking_script()) {
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

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead
// -> A start tag whose tag name is "meta"
void HTMLParser::process_meta_element_from_rust_parser(HTMLMetaElement& element)
{
    // If the active speculative HTML parser is null:
    if (m_active_speculative_html_parser)
        return;

    // NB: Both steps below require tentative confidence.
    if (m_encoding_confidence != EncodingConfidence::Tentative)
        return;

    // 1. If the element has a charset attribute, and getting an encoding from its value results in an encoding, and the
    //    confidence is currently tentative, then change the encoding to the resulting encoding.
    if (auto charset = element.get_attribute(AttributeNames::charset); charset.has_value()) {
        if (auto encoding = TextCodec::get_standardized_encoding(charset->utf16_view()); encoding.has_value()) {
            change_the_encoding(*encoding);
            return;
        }
    }

    // 2. Otherwise, if the element has an http-equiv attribute whose value is an ASCII case-insensitive match for
    //    "Content-Type", and the element has a content attribute, and applying the algorithm for extracting a character
    //    encoding from a meta element to that attribute's value returns an encoding, and the confidence is currently
    //    tentative, then change the encoding to the extracted encoding.
    if (element.http_equiv_state() != HTMLMetaElement::HttpEquivAttributeState::EncodingDeclaration)
        return;

    auto content = element.get_attribute(AttributeNames::content);
    if (!content.has_value())
        return;

    auto content_as_utf8 = content->to_utf8_but_should_be_ported_to_utf16().to_byte_string();
    if (auto encoding = extract_character_encoding_from_meta_element(content_as_utf8); encoding.has_value())
        change_the_encoding(*encoding);
}

// https://html.spec.whatwg.org/multipage/parsing.html#change-the-encoding
void HTMLParser::change_the_encoding(StringView new_encoding)
{
    VERIFY(m_encoding_confidence == EncodingConfidence::Tentative);
    VERIFY(m_document->has_encoding());

    auto current_encoding = TextCodec::get_standardized_encoding(m_document->encoding().value());
    VERIFY(current_encoding.has_value());

    // 1. If the encoding that is already being used to interpret the input stream is UTF-16BE/LE, then set the
    //    confidence to certain and return. The new encoding is ignored; if it was anything but the same encoding, then
    //    it would be clearly incorrect.
    if (current_encoding->is_one_of_ignoring_ascii_case("UTF-16BE"sv, "UTF-16LE"sv)) {
        m_encoding_confidence = EncodingConfidence::Certain;
        return;
    }

    // 2. If the new encoding is UTF-16BE/LE, then change it to UTF-8.
    if (new_encoding.is_one_of_ignoring_ascii_case("UTF-16BE"sv, "UTF-16LE"sv))
        new_encoding = "UTF-8"sv;

    // 3. If the new encoding is x-user-defined, then change it to windows-1252.
    if (new_encoding.equals_ignoring_ascii_case("x-user-defined"sv))
        new_encoding = "windows-1252"sv;

    // 4. If the new encoding is identical or equivalent to the encoding that is already being used to interpret the
    //    input stream, then set the confidence to certain and return. This happens when the encoding information found
    //    in the file matches what the encoding sniffing algorithm determined to be the encoding, and in the second pass
    //    through the parser if the first pass found that the encoding sniffing algorithm described in the earlier
    //    section failed to find the right encoding.
    if (current_encoding->equals_ignoring_ascii_case(new_encoding)) {
        m_encoding_confidence = EncodingConfidence::Certain;
        return;
    }

    // 5. If all the bytes up to the last byte converted by the current decoder have the same Unicode interpretations in
    //    both the current encoding and the new encoding, and if the user agent supports changing the converter on the
    //    fly, then the user agent may change to the new converter for the encoding on the fly. Set the document's
    //    character encoding and the encoding used to convert the input stream to the new encoding, set the confidence
    //    to certain, and return.
    if (m_change_encoding_callback && m_change_encoding_callback->function()(new_encoding)) {
        m_document->set_encoding(utf16_string_from_standardized_encoding_label(new_encoding));
        m_encoding_confidence = EncodingConfidence::Certain;
        return;
    }

    // FIXME: 6. Otherwise, restart the navigate algorithm, with historyHandling set to "replace" and other inputs kept the
    //           same, but this time skip the encoding sniffing algorithm and instead just set the encoding to the new encoding
    //           and the confidence to certain. Whenever possible, this should be done without actually contacting the network
    //           layer (the bytes should be re-parsed from memory), even if, e.g., the document is marked as not being
    //           cacheable. If this is not possible and contacting the network layer would involve repeating a request that
    //           uses a method other than `GET`, then instead set the confidence to certain and ignore the new encoding. The
    //           resource will be misinterpreted. User agents may notify the user of the situation, to aid in application
    //           development.
    dbgln_if(HTML_PARSER_DEBUG, "Unable to change HTML parser encoding from '{}' to '{}' without restarting navigation", *current_encoding, new_encoding);
    m_encoding_confidence = EncodingConfidence::Certain;
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
    if (!m_parsing_fragment && document().pending_parsing_blocking_svg_script()) {
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
HTMLParser::ParserlessCompletionToken HTMLParser::parserless_completion_token(DOM::Document const& document)
{
    return { document.parser_generation() };
}

void HTMLParser::the_end(GC::Ref<DOM::Document> document, ParserlessCompletionToken token)
{
    the_end(document, nullptr, token.parser_generation);
}

void HTMLParser::the_end(GC::Ref<DOM::Document> document, GC::Ptr<HTMLParser> parser)
{
    the_end(document, parser, document->parser_generation());
}

static bool parser_was_replaced(DOM::Document const& document, GC::Ptr<HTMLParser> parser, u64 parser_generation)
{
    return document.parser_generation() != parser_generation || document.parser() != parser;
}

void HTMLParser::the_end(GC::Ref<DOM::Document> document, GC::Ptr<HTMLParser> parser, u64 parser_generation)
{
    // Once the user agent stops parsing the document, the user agent must run the following steps:

    // NOTE: This is a static method because the spec sometimes wants us to "act as if the user agent had stopped
    //       parsing document" which means running these steps without an HTML Parser. That makes it awkward to call,
    //       but it's preferable to duplicating so much code.

    if (parser)
        VERIFY(document == parser->m_document);

    // The entirety of "the end" should be a no-op for HTML fragment parsers. Fragment parsing does not complete its
    // context document, dispatch document or Window events, or process the context document's script queues. Moreover,
    // the unconditional "spin the event loop" invocations perform a microtask checkpoint, causing things to happen out
    // of order. For example, YouTube sets the innerHTML of a <template> element in the constructor of the ytd-app custom
    // element _before_ setting up class attributes. Since custom elements use microtasks to run callbacks, this causes
    // custom element callbacks that rely on attributes setup by the constructor to run before the attributes are set up,
    // causing unhandled exceptions.
    // We can avoid these issues and also avoid doing unnecessary work by simply skipping "the end" for HTML fragment
    // parsers.
    // See the message of the commit that added this for more details.
    if (parser && parser->m_parsing_fragment)
        return;

    // INTEROP: Blink and WebKit keep a parser object associated with media documents, so parser identity prevents stale
    //          completion after document.open(). Ladybird's media documents are parserless, so also compare a generation
    //          that changes whenever a replacement parser is associated with the Document.
    if (parser_was_replaced(document, parser, parser_generation))
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

    // INTEROP: Step 3 can run a readystatechange event handler which calls document.open(), so recheck after it returns.
    if (parser_was_replaced(document, parser, parser_generation))
        return;

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
    auto state = HTMLParserEndState::create(document, parser, parser_generation);
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

GC::Ref<HTMLParserEndState> HTMLParserEndState::create(GC::Ref<DOM::Document> document, GC::Ptr<HTMLParser> parser, u64 parser_generation)
{
    return GC::Heap::the().allocate<HTMLParserEndState>(document, parser, parser_generation);
}

HTMLParserEndState::HTMLParserEndState(GC::Ref<DOM::Document> document, GC::Ptr<HTMLParser> parser, u64 parser_generation)
    : m_document(document)
    , m_parser(parser)
    , m_parser_generation(parser_generation)
    , m_timeout(Platform::Timer::create_single_shot(GC::Heap::the(), THE_END_TIMEOUT_MS, GC::create_function(GC::Heap::the(), [this] {
        if (m_phase != Phase::Completed && m_phase != Phase::Cancelled)
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

void HTMLParserEndState::cancel()
{
    m_phase = Phase::Cancelled;
    m_timeout->stop();
}

void HTMLParserEndState::schedule_progress_check()
{
    if (m_phase == Phase::Completed || m_phase == Phase::Cancelled)
        return;
    if (m_check_pending)
        return;
    m_check_pending = true;
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this] {
        perform_pre_progress_microtask_checkpoint();
        check_progress();
        m_check_pending = false;
    }));
}

void HTMLParserEndState::check_progress()
{
    if (m_phase == Phase::Cancelled)
        return;

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

            // INTEROP: The script may have called document.open(), cancelling this parser's completion and clearing
            //          its list of deferred scripts. Blink, WebKit, and Gecko detach or terminate the old parser here.
            if (m_phase == Phase::Cancelled)
                return;

            // 3. Remove the first script element from the list of scripts that will execute when the document has finished parsing (i.e. shift out the first entry in the list).
            (void)m_document->scripts_to_execute_when_parsing_has_finished().take_first();
        }

        advance_to_dom_content_loaded_phase();
        return;

    case Phase::WaitingForDOMContentLoaded:
        // NB: The task queued by advance_to_dom_content_loaded_phase() advances the state machine once it has run.
        return;

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
        // AD-HOC: Update style first — so any font fetches that the computed styles depend on get started; an in-flight
        //         font fetch delays the load event.
        // INTEROP: Gecko also flushes layout before firing the load event — so lazily-started font loads hold it back.
        m_document->update_style();
        if (m_document->anything_is_delaying_the_load_event())
            return;

        m_phase = Phase::Completed;
        [[fallthrough]];

    case Phase::Completed:
        complete();
        return;
    case Phase::Cancelled:
        return;
    }
}

void HTMLParserEndState::advance_to_dom_content_loaded_phase()
{
    // AD-HOC: We need to scroll to the fragment on page load somewhere.
    // But a script that ran in step 5 above may have scrolled the page already,
    // so only do this if there is an actual fragment to avoid resetting the scroll position unexpectedly.
    // Spec bug: https://github.com/whatwg/html/issues/10914
    auto indicated_part = m_document->determine_the_indicated_part();
    if (indicated_part.has<DOM::Element*>() && indicated_part.get<DOM::Element*>() != nullptr) {
        m_document->scroll_to_the_fragment();
    }

    m_phase = Phase::WaitingForDOMContentLoaded;

    // 6. Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following substeps:
    queue_global_task(HTML::Task::Source::DOMManipulation, relevant_global_object(*m_document), GC::create_function(GC::Heap::the(), [state = GC::Ref(*this), document = m_document] {
        if (state->m_phase != Phase::WaitingForDOMContentLoaded)
            return;
        // 1. Set the Document's load timing info's DOM content loaded event start time to the current high resolution time given the Document's relevant global object.
        document->load_timing_info().dom_content_loaded_event_start_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(*document));

        // 2. Fire an event named DOMContentLoaded at the Document object, with its bubbles attribute initialized to true.
        auto content_loaded_event = DOM::Event::create(
            HTML::EventNames::DOMContentLoaded,
            HighResolutionTime::current_high_resolution_time(relevant_global_object(*document)));
        content_loaded_event->set_bubbles(true);
        document->dispatch_event(content_loaded_event);

        // 3. Set the Document's load timing info's DOM content loaded event end time to the current high resolution time given the Document's relevant global object.
        document->load_timing_info().dom_content_loaded_event_end_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(*document));

        // FIXME: 4. Enable the client message queue of the ServiceWorkerContainer object whose associated service worker client is the Document object's relevant settings object.

        // FIXME: 5. Invoke WebDriver BiDi DOM content loaded with the Document's browsing context, and a new WebDriver BiDi navigation status whose id is the Document object's navigation id, status is "pending", and url is the Document object's URL.

        // NB: Only advance the state machine after this task has run, so that anything the DOMContentLoaded event
        //     handlers did (e.g. starting loads that delay the load event) is visible to the remaining phases. This
        //     matches steps 7 and 8, whose "spin the event loop" continuations resume in a new task queued after the
        //     DOMContentLoaded task, per https://html.spec.whatwg.org/multipage/webappapis.html#spin-the-event-loop.
        if (state->m_phase == Phase::WaitingForDOMContentLoaded) {
            state->m_phase = Phase::WaitingForASAPScripts;
            state->schedule_progress_check();
        }
    }));
}

void HTMLParserEndState::complete()
{
    m_phase = Phase::Completed;

    // 9. Queue a global task on the DOM manipulation task source given the Document's relevant global object to run the following steps:
    queue_global_task(HTML::Task::Source::DOMManipulation, relevant_global_object(*m_document), GC::create_function(GC::Heap::the(), [state = GC::Ref(*this), document = m_document, parser = m_parser, parser_generation = m_parser_generation] {
        if (state->m_phase != Phase::Completed)
            return;
        // document.open() can replace the parser after this completion task
        // was queued but before it gets a chance to run. Do not let the old
        // parser mark the replacement document ready for post-load tasks.
        if (parser_was_replaced(document, parser, parser_generation))
            return;

        state->m_timeout->stop();
        document->set_html_parser_end_state(nullptr);

        // 1. Update the current document readiness to "complete".
        document->update_readiness(HTML::DocumentReadyState::Complete);

        // INTEROP: Step 1 can run a readystatechange event handler which calls document.open(), so recheck after it returns.
        if (parser_was_replaced(document, parser, parser_generation))
            return;

        // AD-HOC: We need to wait until the document ready state is complete before detaching the parser, otherwise the DOM complete time will not be set correctly.
        if (parser)
            document->detach_parser();

        // 2. If the Document object's browsing context is null, then abort these steps.
        if (!document->browsing_context())
            return;

        // AD-HOC: Give the document a styled, laid-out state before load listeners run. Style and
        //         layout otherwise wait for the next rendering update, and a load handler that
        //         changes style would fold its change into the document's very first style pass,
        //         where a transition it expects to start has no before-change style to start from.
        document->update_layout(DOM::UpdateLayoutReason::DocumentReadinessComplete);

        // 3. Let window be the Document's relevant global object.
        auto& window = relevant_window(*document);

        // 4. Set the Document's load timing info's load event start time to the current high resolution time given window.
        document->load_timing_info().load_event_start_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(window));

        // 5. Fire an event named load at window, with legacy target override flag set.
        // FIXME: The legacy target override flag is currently set by a virtual override of dispatch_event()
        //        We should reorganize this so that the flag appears explicitly here instead.
        window.dispatch_event(DOM::Event::create(
            HTML::EventNames::load,
            HighResolutionTime::current_high_resolution_time(relevant_global_object(window))));

        // INTEROP: A load event handler can call document.open(), which associates a replacement parser after the old
        //          parser was detached above. Do not let this completion continue into the replacement document.
        if (document->parser_generation() != parser_generation)
            return;

        // FIXME: 6. Invoke WebDriver BiDi load complete with the Document's browsing context, and a new WebDriver BiDi navigation status whose id is the Document object's navigation id, status is "complete", and url is the Document object's URL.

        // FIXME: 7. Set the Document object's navigation id to null.

        // 8. Set the Document's load timing info's load event end time to the current high resolution time given window.
        document->load_timing_info().load_event_end_time = HighResolutionTime::current_high_resolution_time(relevant_global_object(window));

        // 9. Assert: Document's page showing is false.
        VERIFY(!document->page_showing());

        // 10. Set the Document's page showing to true.
        document->set_page_showing(true);

        // 11. Fire a page transition event named pageshow at window with false.
        window.fire_a_page_transition_event(HTML::EventNames::pageshow, false);

        // INTEROP: Step 11 can run a pageshow event handler which calls document.open(). As with the load event above,
        //          do not let this completion continue into the replacement document contents.
        if (document->parser_generation() != parser_generation)
            return;

        // 11. The Document is now ready for post-load tasks.
        document->set_ready_for_post_load_tasks(true);

        // 12. Completely finish loading the Document.
        document->completely_finish_loading();

        // FIXME: 13. Queue the navigation timing entry for the Document.
    }));

    // FIXME: 10. If the Document's print when loaded flag is set, then run the printing steps.

    // NB: Step 11 (the Document is now ready for post-load tasks) runs inside the task queued above; see there.
}

// https://html.spec.whatwg.org/multipage/parsing.html#create-an-element-for-the-token
GC::Ref<DOM::Element> HTMLParser::create_element_for(Utf16FlyString const& local_name, ReadonlySpan<RustFfiHtmlParserAttribute> attributes, Optional<Utf16FlyString> const& namespace_, DOM::Node& intended_parent)
{
    // 1. If the active speculative HTML parser is not null, then return the result of creating a speculative mock element given namespace, token's tag name, and token's attributes.
    // The active speculative HTML parser runs synchronously to completion, so it is null whenever the real parser
    // invokes this algorithm. It emits speculative fetch candidates directly instead of producing mock elements.

    // 2. Otherwise, optionally create a speculative mock element given namespace, token's tag name, and token's attributes.
    // We deliberately skip step 2, the active speculative parser already issues these fetches, so doing it again here
    // would be redundant.

    // 3. Let document be intendedParent's node document.
    GC::Ref<DOM::Document> document = intended_parent.document();

    // 4. Let localName be token's tag name.

    auto attribute_value = [&](Utf16FlyString const& name) -> Optional<StringView> {
        for (auto const& attribute : attributes) {
            if (attribute.local_name == name.raw_identity())
                return StringView { reinterpret_cast<char const*>(attribute.value_ptr), attribute.value_len };
        }
        return {};
    };

    // 5. Let is be the value of the "is" attribute in token, if such an attribute exists; otherwise null.
    Optional<Utf16FlyString> is_value;
    if (auto is_attribute = attribute_value(AttributeNames::is); is_attribute.has_value())
        is_value = Utf16FlyString::from_utf8_without_validation(*is_attribute);

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
    auto element = DOM::create_element(*document, local_name, namespace_, {}, is_value, will_execute_script, registry).release_value_but_fixme_should_propagate_errors();

    // AD-HOC: Let <link> elements know which document they were originally parsed for.
    //         This is used for the render-blocking logic.
    if (!m_parsing_fragment && local_name == HTML::TagNames::link && namespace_ == Namespace::HTML) {
        auto& link_element = as<HTMLLinkElement>(*element);
        link_element.set_parser_document({}, document);
        link_element.set_was_enabled_when_created_by_parser({}, !attribute_value(HTML::AttributeNames::disabled).has_value());
    }

    // AD-HOC: Let style elements know which document they were originally parsed for.
    //         This is used for the render-blocking logic.
    if (auto* style_element = as_if<DOM::StyleElementBase>(*element))
        style_element->set_parser_document({}, document);

    // 11. Append each attribute in the given token to element.
    for (auto const& attribute : attributes) {
        Optional<Utf16FlyString> prefix;
        if (attribute.prefix_len != 0)
            prefix = Utf16FlyString::from_utf8_without_validation({ reinterpret_cast<char const*>(attribute.prefix_ptr), attribute.prefix_len });
        DOM::QualifiedName qualified_name { Utf16FlyString::from_raw(attribute.local_name), move(prefix), attribute_namespace_from_html_parser_ffi(attribute.namespace_) };
        element->append_attribute(move(qualified_name), utf8_string_from_ffi(attribute.value_ptr, attribute.value_len));
    }

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
        invoke_custom_element_reactions(queue);

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
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this] {
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

    // INTEROP: Blink and WebKit detach a parser when document.open() replaces it. Do not let resume work from the
    //          detached parser consume a parsing-blocking script owned by the replacement parser.
    if (m_document->parser().ptr() != this)
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
    if (auto callback = exchange(m_parsing_complete_callback, nullptr))
        callback->function()();
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

namespace {

struct FragmentAttribute {
    StringView name;
    StringView value;
};

Utf16FlyString const* supported_fragment_tag(StringView name)
{
    // Keep this subset deliberately small. Adding a tag requires checking its in-body tree-builder rules,
    // its insertion steps, and interactions with every other supported tag.
    if (name == "a"sv)
        return &TagNames::a;
    if (name == "b"sv)
        return &TagNames::b;
    if (name == "br"sv)
        return &TagNames::br;
    if (name == "button"sv)
        return &TagNames::button;
    if (name == "div"sv)
        return &TagNames::div;
    if (name == "footer"sv)
        return &TagNames::footer;
    if (name == "i"sv)
        return &TagNames::i;
    if (name == "input"sv)
        return &TagNames::input;
    if (name == "label"sv)
        return &TagNames::label;
    if (name == "li"sv)
        return &TagNames::li;
    if (name == "ol"sv)
        return &TagNames::ol;
    if (name == "span"sv)
        return &TagNames::span;
    if (name == "strong"sv)
        return &TagNames::strong;
    if (name == "ul"sv)
        return &TagNames::ul;
    if (name == "article"sv)
        return &TagNames::article;
    if (name == "aside"sv)
        return &TagNames::aside;
    if (name == "header"sv)
        return &TagNames::header;
    if (name == "main"sv)
        return &TagNames::main;
    if (name == "nav"sv)
        return &TagNames::nav;
    if (name == "section"sv)
        return &TagNames::section;
    if (name == "big"sv)
        return &TagNames::big;
    if (name == "code"sv)
        return &TagNames::code;
    if (name == "em"sv)
        return &TagNames::em;
    if (name == "s"sv)
        return &TagNames::s;
    if (name == "small"sv)
        return &TagNames::small;
    if (name == "strike"sv)
        return &TagNames::strike;
    if (name == "tt"sv)
        return &TagNames::tt;
    if (name == "u"sv)
        return &TagNames::u;
    return nullptr;
}

class FragmentScanner {
public:
    explicit FragmentScanner(StringView input)
        : m_input(input)
    {
    }

    template<typename Open, typename Close, typename Text>
    bool parse(Open&& open, Close&& close, Text&& text)
    {
        Vector<Utf16FlyString const*, 32> stack;
        Vector<FragmentAttribute, 16> attributes;
        while (m_offset < m_input.length()) {
            if (peek() != '<') {
                auto start = m_offset;
                while (m_offset < m_input.length() && peek() != '<') {
                    if (peek() == '&' || peek() == '\r' || peek() == '\0')
                        return false;
                    ++m_offset;
                }
                text(m_input.substring_view(start, m_offset - start));
                continue;
            }
            ++m_offset;
            bool is_end_tag = consume('/');
            auto name = scan_name();
            auto* tag = supported_fragment_tag(name);
            if (!tag)
                return false;
            if (is_end_tag) {
                skip_whitespace();
                if (!consume('>') || stack.is_empty() || stack.take_last() != tag)
                    return false;
                close();
                continue;
            }

            // Leave implicit closing of anchors, buttons, and list items to the general parser.
            if ((tag == &TagNames::a || tag == &TagNames::button) && stack.contains_slow(tag))
                return false;
            if (tag == &TagNames::li) {
                for (size_t i = stack.size(); i > 0; --i) {
                    auto* ancestor = stack[i - 1];
                    if (ancestor == &TagNames::li)
                        return false;
                    // A nested list stops the in-body rule from closing an outer list item.
                    if (ancestor == &TagNames::ul || ancestor == &TagNames::ol)
                        break;
                }
            }
            if (stack.size() >= 128)
                return false;

            attributes.clear_with_capacity();
            while (true) {
                skip_whitespace();
                if (peek() == '>' || peek() == '/')
                    break;
                auto attribute_name = scan_name();
                if (attribute_name.is_empty() || attribute_name == "is"sv || attributes.size() >= 64)
                    return false;
                for (auto const& attribute : attributes) {
                    if (attribute.name == attribute_name)
                        return false;
                }
                skip_whitespace();
                StringView value = ""sv;
                if (consume('=')) {
                    skip_whitespace();
                    char quote = peek();
                    if (quote != '\'' && quote != '"')
                        return false;
                    ++m_offset;
                    auto start = m_offset;
                    while (m_offset < m_input.length() && peek() != quote) {
                        if (peek() == '&' || peek() == '\r' || peek() == '\0')
                            return false;
                        ++m_offset;
                    }
                    value = m_input.substring_view(start, m_offset - start);
                    if (!consume(quote))
                        return false;
                }
                if (tag == &TagNames::input && attribute_name == "type"sv && !value.is_one_of("text"sv, "checkbox"sv))
                    return false;
                attributes.append({ attribute_name, value });
            }
            bool is_void = tag == &TagNames::br || tag == &TagNames::input;
            if (consume('/') && !is_void)
                return false;
            if (!consume('>'))
                return false;
            open(*tag, attributes.span(), is_void);
            if (!is_void)
                stack.append(tag);
        }
        return stack.is_empty();
    }

private:
    char peek() const { return m_offset < m_input.length() ? m_input[m_offset] : '\0'; }

    bool consume(char ch)
    {
        if (m_offset == m_input.length() || peek() != ch)
            return false;
        ++m_offset;
        return true;
    }

    void skip_whitespace()
    {
        while (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\f')
            ++m_offset;
    }

    StringView scan_name()
    {
        auto start = m_offset;
        while (is_ascii_lower_alpha(peek()) || is_ascii_digit(peek()) || peek() == '-')
            ++m_offset;
        return m_input.substring_view(start, m_offset - start);
    }

    StringView m_input;
    size_t m_offset { 0 };
};

}

GC::Ptr<DOM::DocumentFragment> HTMLParser::try_parse_html_fragment_fast(DOM::Element& context, Utf16View input)
{
    // OPTIMIZATION: Validate a restricted in-body grammar before allocating or exposing any DOM nodes. The second
    //               pass borrows input spans and constructs elements directly, bypassing tokenization and tree building.
    //               All unsupported syntax falls back without side effects, including custom elements and foreign content.
    if (context.namespace_uri() != Namespace::HTML || !input.has_ascii_storage())
        return nullptr;
    if (!context.local_name().is_one_of(TagNames::body, TagNames::div, TagNames::span, TagNames::ul, TagNames::ol,
            TagNames::li, TagNames::footer, TagNames::button, TagNames::label, TagNames::p))
        return nullptr;
    if (context.first_ancestor_of_type<HTMLFormElement>())
        return nullptr;

    StringView source { input.ascii_span().data(), input.length_in_code_units() };
    // Character references and input preprocessing use the general parser.
    if (!FragmentScanner { source }.parse([](auto const&, auto, bool) { }, [] { }, [](auto) { }))
        return nullptr;

    auto fragment = DOM::DocumentFragment::create(context.document());
    auto registry = look_up_a_custom_element_registry(context);
    GC::RootVector<GC::Ref<DOM::Node>, 32> parents;
    parents.append(fragment);
    auto success = FragmentScanner { source }.parse(
        [&](Utf16FlyString const& tag, ReadonlySpan<FragmentAttribute> attributes, bool is_void) {
            auto& parent = *parents.last();
            auto element = MUST(DOM::create_element(context.document(), tag, Namespace::HTML, {}, {}, false, registry));
            for (auto const& attribute : attributes) {
                DOM::QualifiedName name { Utf16FlyString::from_utf8_without_validation(attribute.name), {}, {} };
                element->append_attribute(move(name), Utf16String::from_utf8_without_validation(attribute.value));
            }
            auto& html_element = as<HTMLElement>(*element);
            if (html_element.is_form_associated_element() && html_element.is_resettable())
                html_element.reset_algorithm();
            MUST(parent.append_child(element));
            if (!is_void)
                parents.append(element);
        },
        [&] { parents.take_last(); },
        [&](StringView text) {
            MUST(parents.last()->append_child(DOM::Text::create(context.document(), Utf16String::from_utf8_without_validation(text))));
        });
    VERIFY(success);
    return fragment;
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-html-fragments
WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> HTMLParser::parse_html_fragment(Variant<GC::Ref<DOM::Element>, GC::Ref<DOM::DocumentFragment>> target, Utf16View input, AllowDeclarativeShadowRoots allow_declarative_shadow_roots, ParserScriptingMode scripting_mode)
{
    // 1. Assert: scriptingMode is either Inert or Fragment.
    VERIFY(scripting_mode == HTML::ParserScriptingMode::Inert || scripting_mode == HTML::ParserScriptingMode::Fragment);

    // 2. Let context be target if target is an Element; otherwise target's host.
    DOM::Element* context = target.has<GC::Ref<DOM::Element>>()
        ? target.get<GC::Ref<DOM::Element>>().ptr()
        : target.get<GC::Ref<DOM::DocumentFragment>>()->host();

    // 3. Assert: context is non-null.
    VERIFY(context);

    // OPTIMIZATION: A validated subset of in-body fragments can be constructed directly from input spans.
    if (target.has<GC::Ref<DOM::Element>>()) {
        if (auto fragment = try_parse_html_fragment_fast(*context, input))
            return GC::Ref { *fragment };
    }

    // 4. Let document be a Document node whose type is "html".

    // 5. Let contextDocument be context's node document.
    auto& context_document = context->document();

    // 6. If contextDocument is in quirks mode, then set document's mode to "quirks".
    // 7. Otherwise, if contextDocument is in limited-quirks mode, then set document's mode to "limited-quirks".

    // 8. Create a new HTML parser whose allow declarative shadow roots is allowDeclarativeShadowRoots, and associate it with document.
    // 9. If contextDocument's scripting is disabled, then set scriptingMode to Disabled.
    // 10. Set the parser's scripting mode to scriptingMode.
    if (context_document.is_scripting_disabled())
        scripting_mode = HTML::ParserScriptingMode::Disabled;

    // OPTIMIZATION: Use the context document directly while keeping the parser detached from its document parser state.
    //               The detached root below supplies the temporary tree-builder state, while all output nodes are
    //               created for their final document. This avoids initializing a complete Document for every fragment.
    auto parser = GC::Heap::the().allocate<HTMLParser>(context_document, scripting_mode, input, FragmentParser::Yes);
    parser->initialize(context_document.relevant_settings_object().realm());
    parser->set_allow_declarative_shadow_roots(allow_declarative_shadow_roots);
    parser->m_context_element = context; // FIXME: Is this needed?

    // 11. Set the state of the HTML parser's tokenization stage as follows, switching on context:
    bool const context_element_is_html = context->namespace_uri() == Namespace::HTML;
    // - title
    // - textarea
    if (context_element_is_html
        && context->local_name().is_one_of(HTML::TagNames::title, HTML::TagNames::textarea)) {
        // Switch the tokenizer to the RCDATA state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::RCDATA);
    }
    // - style
    // - xmp
    // - iframe
    // - noembed
    // - noframes
    else if (context_element_is_html
        && context->local_name().is_one_of(HTML::TagNames::style, HTML::TagNames::xmp, HTML::TagNames::iframe, HTML::TagNames::noembed, HTML::TagNames::noframes)) {
        // Switch the tokenizer to the RAWTEXT state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::RAWTEXT);
    }
    // - script
    else if (context_element_is_html && context->local_name().is_one_of(HTML::TagNames::script)) {
        // Switch the tokenizer to the script data state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::ScriptData);
    }
    // - noscript
    else if (context_element_is_html && context->local_name().is_one_of(HTML::TagNames::noscript)) {
        // If scripting mode is not Disabled, switch the tokenizer to the RAWTEXT state. Otherwise, leave the tokenizer in the data state.
        if (scripting_mode != HTML::ParserScriptingMode::Disabled)
            parser->m_tokenizer.switch_to(HTMLTokenizer::State::RAWTEXT);
    }
    // - plaintext
    else if (context_element_is_html && context->local_name().is_one_of(HTML::TagNames::plaintext)) {
        // Switch the tokenizer to the PLAINTEXT state.
        parser->m_tokenizer.switch_to(HTMLTokenizer::State::PLAINTEXT);
    }
    // Any other element
    else {
        // Leave the tokenizer in the data state.
    }

    auto target_node = target.visit([](auto node) -> GC::Ref<DOM::Node> { return node; });

    // 12. Let root be the result of creating an element given document, "html", the HTML namespace, null, null, false,
    //     and the result of looking up a custom element registry given target.
    auto root_registry = look_up_a_custom_element_registry(target_node);
    auto root = MUST(DOM::create_element(context_document, HTML::TagNames::html, Namespace::HTML, {}, {}, false, root_registry));

    // 13. Append root to document.
    // OPTIMIZATION: Keep the root detached. It is only a sentinel for the tree builder, whose root insertions are
    //               redirected to the output fragment.

    // 14. Set up the HTML parser's stack of open elements so that it contains just the single element root.
    // 15. Let fragment be a new DocumentFragment whose node document is target's node document.
    auto fragment = DOM::DocumentFragment::create(target_node->document());

    // 16. Set the parser's root insertion target to fragment.
    parser->m_root_insertion_target = fragment;

    // 17. If context is a template element, then push "in template" onto the stack of template insertion modes so that
    //     it is the new current template insertion mode.
    // 18. Create a start tag token whose name is the local name of context and whose attributes are the attributes of context.
    //     Let this start tag token be the start tag token of context; e.g. for the purposes of determining if it is an
    //     HTML integration point.
    // 19. Reset the parser's insertion mode appropriately.
    // NB: The parser will reference the context element as part of that algorithm.

    // 20. Set the HTML parser's form element pointer to the nearest node to context that is a form element
    //     (going straight up the ancestor chain, and including the element itself, if it is a form element), if any.
    //     (If there is no such form element, the form element pointer keeps its initial value, null.)
    parser->m_form_element = as_if<HTMLFormElement>(context);
    if (!parser->m_form_element)
        parser->m_form_element = context->first_ancestor_of_type<HTMLFormElement>();

    auto context_namespace = context->namespace_uri();
    auto context_namespace_ffi = namespace_to_html_parser_ffi(context_namespace);
    Vector<u16> context_namespace_uri;
    if (context_namespace_ffi == RustFfiHtmlNamespace::Other && context_namespace.has_value()) {
        context_namespace_uri = utf16_code_units_for_ffi(context_namespace->view());
    }
    Vector<RustFfiHtmlParserContextAttribute> context_attributes;
    Vector<Vector<u16>> attribute_prefixes;
    Vector<Vector<u16>> attribute_values;
    if (auto attributes = context->attributes()) {
        context_attributes.ensure_capacity(attributes->length());
        attribute_prefixes.ensure_capacity(attributes->length());
        attribute_values.ensure_capacity(attributes->length());
        for (size_t i = 0; i < attributes->length(); ++i) {
            auto attribute = attributes->item(i);
            auto attribute_value = attribute->value();
            attribute_values.unchecked_append(utf16_code_units_for_ffi(attribute_value));
            auto const& value = attribute_values.last();
            Vector<u16> const* prefix = nullptr;
            if (attribute->prefix().has_value()) {
                attribute_prefixes.unchecked_append(utf16_code_units_for_ffi(attribute->prefix()->view()));
                prefix = &attribute_prefixes.last();
            }
            context_attributes.unchecked_append(RustFfiHtmlParserContextAttribute {
                attribute->local_name().raw_identity(),
                prefix ? prefix->data() : nullptr,
                prefix ? prefix->size() : 0,
                attribute_namespace_to_html_parser_ffi(attribute->namespace_uri()),
                value.data(),
                value.size(),
            });
        }
    }
    rust_html_parser_begin_fragment(
        parser->m_rust_parser,
        reinterpret_cast<size_t>(root.ptr()),
        reinterpret_cast<size_t>(fragment.ptr()),
        reinterpret_cast<size_t>(context),
        context_namespace_ffi,
        context_namespace_uri.data(),
        context_namespace_uri.size(),
        context->local_name().raw_identity(),
        context_attributes.data(),
        context_attributes.size(),
        quirks_mode_to_html_parser_ffi(context_document.mode()),
        allow_declarative_shadow_roots == AllowDeclarativeShadowRoots::Yes,
        parser->m_form_element ? reinterpret_cast<size_t>(parser->m_form_element.ptr()) : 0);

    // 22. Place the input into the input stream for the HTML parser just created. The encoding confidence is irrelevant.
    // 23. Start the HTML parser and let it run until it has consumed all the characters just inserted into the input stream.
    parser->run_until_completion();

    return fragment;
}

GC::Ref<HTMLParser> HTMLParser::create_for_scripting(DOM::Document& document)
{
    auto scripting_mode = document.is_scripting_enabled() ? ParserScriptingMode::Normal : ParserScriptingMode::Disabled;
    return document.relevant_settings_object().realm().create<HTMLParser>(document, scripting_mode, ScriptCreatedParser::Yes, EncodingConfidence::Irrelevant);
}

GC::Ref<HTMLParser> HTMLParser::create_with_open_input_stream(DOM::Document& document, EncodingConfidence encoding_confidence)
{
    auto scripting_mode = document.is_scripting_enabled() ? ParserScriptingMode::Normal : ParserScriptingMode::Disabled;
    return document.relevant_settings_object().realm().create<HTMLParser>(document, scripting_mode, ScriptCreatedParser::No, encoding_confidence);
}

GC::Ref<HTMLParser> HTMLParser::create_with_uncertain_encoding(DOM::Document& document, ByteBuffer const& input, Optional<MimeSniff::MimeType> maybe_mime_type)
{
    auto scripting_mode = document.is_scripting_enabled() ? ParserScriptingMode::Normal : ParserScriptingMode::Disabled;

    if (document.has_encoding())
        return document.relevant_settings_object().realm().create<HTMLParser>(document, scripting_mode, input, document.encoding().value().to_byte_string(), EncodingConfidence::Certain);

    auto [encoding, confidence] = run_encoding_sniffing_algorithm(document, input, maybe_mime_type);
    dbgln_if(HTML_PARSER_DEBUG, "The encoding sniffing algorithm returned encoding '{}'", encoding);
    return document.relevant_settings_object().realm().create<HTMLParser>(document, scripting_mode, input, encoding, confidence);
}

GC::Ref<HTMLParser> HTMLParser::create_from_byte_string(DOM::Document& document, StringView input, ParserScriptingMode scripting_mode, StringView encoding)
{
    return document.relevant_settings_object().realm().create<HTMLParser>(document, scripting_mode, input, encoding, EncodingConfidence::Certain);
}

GC::Ref<HTMLParser> HTMLParser::create_for_decoded_string(DOM::Document& document, Utf16View input, ParserScriptingMode scripting_mode, Utf16View encoding)
{
    return document.relevant_settings_object().realm().create<HTMLParser>(document, scripting_mode, input, encoding, EncodingConfidence::Irrelevant);
}

enum class AttributeMode {
    No,
    Yes,
};

template<OneOf<Utf8View, Utf16View> ViewType>
static Utf16String escape_string(ViewType const& string, AttributeMode attribute_mode)
{
    // https://html.spec.whatwg.org/multipage/parsing.html#escapingString
    Utf16StringBuilder builder;
    for (auto code_point : string) {
        // 1. Replace any occurrence of the "&" character by the string "&amp;".
        if (code_point == '&')
            builder.append_ascii("&amp;"sv);
        // 2. Replace any occurrences of the U+00A0 NO-BREAK SPACE character by the string "&nbsp;".
        else if (code_point == 0xA0)
            builder.append_ascii("&nbsp;"sv);
        // 3. Replace any occurrences of the "<" character by the string "&lt;".
        else if (code_point == '<')
            builder.append_ascii("&lt;"sv);
        // 4. Replace any occurrences of the ">" character by the string "&gt;".
        else if (code_point == '>')
            builder.append_ascii("&gt;"sv);
        // 5. If the algorithm was invoked in the attribute mode, then replace any occurrences of the """ character by the string "&quot;".
        else if (code_point == '"' && attribute_mode == AttributeMode::Yes)
            builder.append_ascii("&quot;"sv);
        else
            builder.append_code_point(code_point);
    }
    return builder.to_string();
}

// https://html.spec.whatwg.org/multipage/parsing.html#html-fragment-serialisation-algorithm
Utf16String HTMLParser::serialize_html_fragment(DOM::Node const& node, SerializableShadowRoots serializable_shadow_roots, ReadonlySpan<GC::Ref<DOM::ShadowRoot>> shadow_roots, DOM::FragmentSerializationMode fragment_serialization_mode)
{
    // NOTE: Steps in this function are jumbled a bit to accommodate the Element.outerHTML API.
    //       When called with FragmentSerializationMode::Outer, we will serialize the element itself,
    //       not just its children.

    // 2. Let s be a string, and initialize it to the empty string.
    Utf16StringBuilder builder;

    auto serialize_element = [&](DOM::Element const& element) {
        // If current node is an element in the HTML namespace, the MathML namespace, or the SVG namespace, then let tagname be current node's local name.
        // Otherwise, let tagname be current node's qualified name.
        Utf16FlyString tag_name;

        if (element.namespace_uri().has_value() && element.namespace_uri()->is_one_of(Namespace::HTML, Namespace::MathML, Namespace::SVG))
            tag_name = element.local_name();
        else
            tag_name = element.qualified_name();

        // Append a U+003C LESS-THAN SIGN character (<), followed by tagname.
        builder.append_ascii('<');
        builder.append(tag_name.view());

        // If current node's is value is not null, and the element does not have an is attribute in its attribute list,
        // then append the string " is="",
        // followed by current node's is value escaped as described below in attribute mode,
        // followed by a U+0022 QUOTATION MARK character (").
        if (element.is_value().has_value() && !element.has_attribute(AttributeNames::is)) {
            builder.append_ascii(" is=\""sv);
            builder.append(escape_string(element.is_value()->view(), AttributeMode::Yes));
            builder.append_ascii('"');
        }

        // For each attribute that the element has,
        // append a U+0020 SPACE character,
        // the attribute's serialized name as described below,
        // a U+003D EQUALS SIGN character (=),
        // a U+0022 QUOTATION MARK character ("),
        // the attribute's value, escaped as described below in attribute mode,
        // and a second U+0022 QUOTATION MARK character (").
        element.for_each_attribute([&](DOM::Attr const& attribute) {
            builder.append_ascii(' ');

            // An attribute's serialized name for the purposes of the previous paragraph must be determined as follows:
            // -> If the attribute has no namespace:
            if (!attribute.namespace_uri().has_value()) {
                // The attribute's serialized name is the attribute's local name.
                builder.append(attribute.local_name().view());
            }
            // -> If the attribute is in the XML namespace:
            else if (attribute.namespace_uri() == Namespace::XML) {
                // The attribute's serialized name is the string "xml:" followed by the attribute's local name.
                builder.append_ascii("xml:"sv);
                builder.append(attribute.local_name().view());
            }
            // -> If the attribute is in the XMLNS namespace and the attribute's local name is xmlns:
            else if (attribute.namespace_uri() == Namespace::XMLNS && attribute.local_name() == u"xmlns"sv) {
                // The attribute's serialized name is the string "xmlns".
                builder.append_ascii("xmlns"sv);
            }
            // -> If the attribute is in the XMLNS namespace and the attribute's local name is not xmlns:
            else if (attribute.namespace_uri() == Namespace::XMLNS) {
                // The attribute's serialized name is the string "xmlns:" followed by the attribute's local name.
                builder.append_ascii("xmlns:"sv);
                builder.append(attribute.local_name().view());
            }
            // -> If the attribute is in the XLink namespace:
            else if (attribute.namespace_uri() == Namespace::XLink) {
                // The attribute's serialized name is the string "xlink:" followed by the attribute's local name.
                builder.append_ascii("xlink:"sv);
                builder.append(attribute.local_name().view());
            }
            // -> If the attribute is in some other namespace:
            else {
                // The attribute's serialized name is the attribute's qualified name.
                builder.append(attribute.name().view());
            }

            builder.append_ascii("=\""sv);
            auto attribute_value = attribute.value();
            builder.append(escape_string(attribute_value.utf16_view(), AttributeMode::Yes));
            builder.append_ascii('"');
        });

        // Append a U+003E GREATER-THAN SIGN character (>).
        builder.append_ascii('>');

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
        builder.append_ascii("</"sv);
        builder.append(tag_name.view());
        builder.append_ascii('>');

        return IterationDecision::Continue;
    };

    if (fragment_serialization_mode == DOM::FragmentSerializationMode::Outer) {
        serialize_element(as<DOM::Element>(node));
        return builder.to_string();
    }

    // The algorithm takes as input a DOM Element, Document, or DocumentFragment referred to as the node.
    VERIFY(node.is_element() || node.is_document() || node.is_document_fragment());
    GC::Ref<DOM::Node const> actual_node = node;

    if (is<DOM::Element>(node)) {
        auto const& element = as<DOM::Element>(node);

        // 1. If the node serializes as void, then return the empty string.
        //    (NOTE: serializes as void is defined only on elements in the spec)
        if (element.serializes_as_void())
            return {};

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
                || any_of(shadow_roots, [&](auto& entry) { return entry == shadow; })) {
                // then:
                // 1. Append "<template shadowrootmode="".
                builder.append_ascii("<template shadowrootmode=\""sv);

                // 2. If shadow's mode is "open", then append "open". Otherwise, append "closed".
                builder.append(shadow->mode() == Web::DOM::ShadowRootMode::Open ? "open"sv : "closed"sv);

                // 3. Append """.
                builder.append_ascii('"');

                // 4. If shadow's delegates focus is set, then append " shadowrootdelegatesfocus=""".
                if (shadow->delegates_focus())
                    builder.append_ascii(" shadowrootdelegatesfocus=\"\""sv);

                // 5. If shadow's serializable is set, then append " shadowrootserializable=""".
                if (shadow->serializable())
                    builder.append_ascii(" shadowrootserializable=\"\""sv);

                // 6. If shadow's slot assignment is "manual", then append " shadowrootslotassignment="manual"".
                if (shadow->slot_assignment() == Web::DOM::SlotAssignmentMode::Manual)
                    builder.append(" shadowrootslotassignment=\"manual\""sv);

                // 7. If shadow's clonable is set, then append " shadowrootclonable=""".
                if (shadow->clonable())
                    builder.append_ascii(" shadowrootclonable=\"\""sv);

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
                    builder.append_ascii(" shadowrootcustomelementregistry=\"\""sv);

                // 9. Append ">".
                builder.append_ascii('>');

                // 10. Append the value of running the HTML fragment serialization algorithm with shadow,
                //    serializableShadowRoots, and shadowRoots (thus recursing into this algorithm for that element).
                builder.append(serialize_html_fragment(*shadow, serializable_shadow_roots, shadow_roots));

                // 11. Append "</template>".
                builder.append_ascii("</template>"sv);
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
                    builder.append(text_node.data().utf16_view());
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
            builder.append_ascii("<!--"sv);
            builder.append(comment_node.data().utf16_view());
            builder.append_ascii("-->"sv);
            return IterationDecision::Continue;
        }

        if (is<DOM::ProcessingInstruction>(current_node)) {
            // -> If current node is a ProcessingInstruction
            auto& processing_instruction_node = as<DOM::ProcessingInstruction>(current_node);

            // Append the literal string "<?" (U+003C LESS-THAN SIGN, U+003F QUESTION MARK), followed by the value of current node's target IDL attribute,
            // followed by a single U+0020 SPACE character, followed by the value of current node's data IDL attribute, followed by a single U+003E GREATER-THAN SIGN character (>).
            builder.append_ascii("<?"sv);
            builder.append(processing_instruction_node.target().view());
            builder.append_ascii(' ');
            builder.append(processing_instruction_node.data().utf16_view());
            builder.append_ascii('>');
            return IterationDecision::Continue;
        }

        if (is<DOM::DocumentType>(current_node)) {
            // -> If current node is a DocumentType
            auto& document_type_node = as<DOM::DocumentType>(current_node);

            // Append the literal string "<!DOCTYPE" (U+003C LESS-THAN SIGN, U+0021 EXCLAMATION MARK, U+0044 LATIN CAPITAL LETTER D, U+004F LATIN CAPITAL LETTER O,
            // U+0043 LATIN CAPITAL LETTER C, U+0054 LATIN CAPITAL LETTER T, U+0059 LATIN CAPITAL LETTER Y, U+0050 LATIN CAPITAL LETTER P, U+0045 LATIN CAPITAL LETTER E),
            // followed by a space (U+0020 SPACE), followed by the value of current node's name IDL attribute, followed by the literal string ">" (U+003E GREATER-THAN SIGN).
            builder.append_ascii("<!DOCTYPE "sv);
            builder.append(document_type_node.name().view());
            builder.append_ascii('>');
            return IterationDecision::Continue;
        }

        return IterationDecision::Continue;
    });

    // 6. Return s.
    return builder.to_string();
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#current-dimension-value
static size_t code_unit_length(Utf16View string)
{
    return string.length_in_code_units();
}

static u32 code_unit_at(Utf16View string, size_t index)
{
    return string.code_unit_at(index);
}

template<typename StringType>
static bool is_eof(StringType string, size_t position)
{
    return position >= code_unit_length(string);
}

static RefPtr<CSS::StyleValue const> parse_current_dimension_value(float value, Utf16View input, size_t position)
{
    // 1. If position is past the end of input, then return value as a length.
    if (is_eof(input, position))
        return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(value)));

    // 2. If the code point at position within input is U+0025 (%), then return value as a percentage.
    if (code_unit_at(input, position) == '%')
        return CSS::PercentageStyleValue::create(CSS::Percentage(value));

    // 3. Return value as a length.
    return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(value)));
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-dimension-values
RefPtr<CSS::StyleValue const> parse_dimension_value(Utf16View input)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a position variable for input, initially pointing at the start of input.
    size_t position = 0;

    // 3. Skip ASCII whitespace within input given position.
    while (!is_eof(input, position) && Infra::is_ascii_whitespace(code_unit_at(input, position)))
        ++position;

    // 4. If position is past the end of input or the code point at position within input is not an ASCII digit,
    //    then return failure.
    if (is_eof(input, position) || !is_ascii_digit(code_unit_at(input, position)))
        return nullptr;

    // 5. Collect a sequence of code points that are ASCII digits from input given position,
    //    and interpret the resulting sequence as a base-ten integer. Let value be that number.
    double integer_value = 0;
    while (!is_eof(input, position) && is_ascii_digit(code_unit_at(input, position))) {
        integer_value = integer_value * 10 + (code_unit_at(input, position) - '0');
        ++position;
    }

    float value = min(integer_value, CSSPixels::max_dimension_value);

    // 6. If position is past the end of input, then return value as a length.
    if (is_eof(input, position))
        return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels(value)));

    // 7. If the code point at position within input is U+002E (.), then:
    if (code_unit_at(input, position) == '.') {
        // 1. Advance position by 1.
        ++position;

        // 2. If position is past the end of input or the code point at position within input is not an ASCII digit,
        //    then return the current dimension value with value, input, and position.
        if (is_eof(input, position) || !is_ascii_digit(code_unit_at(input, position)))
            return parse_current_dimension_value(value, input, position);

        // 3. Let divisor have the value 1.
        float divisor = 1;

        // 4. While true:
        while (true) {
            // 1. Multiply divisor by ten.
            divisor *= 10;

            // 2. Add the value of the code point at position within input,
            //    interpreted as a base-ten digit (0..9) and divided by divisor, to value.
            value += (code_unit_at(input, position) - '0') / divisor;

            // 3. Advance position by 1.
            ++position;

            // 4. If position is past the end of input, then return value as a length.
            if (is_eof(input, position))
                return CSS::LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(value)));

            // 5. If the code point at position within input is not an ASCII digit, then break.
            if (!is_ascii_digit(code_unit_at(input, position)))
                break;
        }
    }

    // 8. Return the current dimension value with value, input, and position.
    return parse_current_dimension_value(value, input, position);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-non-zero-dimension-values
RefPtr<CSS::StyleValue const> parse_nonzero_dimension_value(Utf16View string)
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
Optional<Color> parse_legacy_color_value(Utf16View string)
{
    // 1. If input is the empty string, then return failure.
    if (string.is_empty())
        return {};

    // 2. Strip leading and trailing ASCII whitespace from input.
    auto input = Utf16String::from_utf16(string.trim(Infra::ASCII_WHITESPACE));

    // 3. If input is an ASCII case-insensitive match for "transparent", then return failure.
    if (input.equals_ignoring_ascii_case(u"transparent"sv))
        return {};

    // 4. If input is an ASCII case-insensitive match for one of the named colors, then return the CSS color corresponding to that keyword. [CSSCOLOR]
    if (auto const color = Color::from_named_css_color_string(input.utf16_view()); color.has_value())
        return color;

    auto hex_nibble_to_u8 = [](u16 nibble) -> u8 {
        if (nibble >= '0' && nibble <= '9')
            return nibble - '0';
        if (nibble >= 'a' && nibble <= 'f')
            return nibble - 'a' + 10;
        return nibble - 'A' + 10;
    };

    // 5. If input's code point length is four, and the first character in input is U+0023 (#), and the last three characters of input are all ASCII hex digits, then:
    if (input.length_in_code_units() == 4
        && input.code_unit_at(0) == '#'
        && is_ascii_hex_digit(input.code_unit_at(1))
        && is_ascii_hex_digit(input.code_unit_at(2))
        && is_ascii_hex_digit(input.code_unit_at(3))) {
        // 1. Let result be a CSS color.
        Color result;
        result.set_alpha(0xFF);

        // 2. Interpret the second character of input as a hexadecimal digit; let the red component of result be the resulting number multiplied by 17.
        result.set_red(hex_nibble_to_u8(input.code_unit_at(1)) * 17);

        // 3. Interpret the third character of input as a hexadecimal digit; let the green component of result be the resulting number multiplied by 17.
        result.set_green(hex_nibble_to_u8(input.code_unit_at(2)) * 17);

        // 4. Interpret the fourth character of input as a hexadecimal digit; let the blue component of result be the resulting number multiplied by 17.
        result.set_blue(hex_nibble_to_u8(input.code_unit_at(3)) * 17);

        // 5. Return result.
        return result;
    }

    // 6. Replace any code points greater than U+FFFF in input (i.e., any characters that are not in the basic multilingual plane) with "00".
    auto replace_non_basic_multilingual_code_points = [](Utf16View string) -> Utf16String {
        Utf16StringBuilder builder;
        for (auto code_point : string) {
            if (code_point > 0xFFFF)
                builder.append(u"00"sv);
            else
                builder.append_code_point(code_point);
        }
        return builder.to_string();
    };
    input = replace_non_basic_multilingual_code_points(input);

    // 7. If input's code point length is greater than 128, truncate input, leaving only the first 128 characters.
    if (input.length_in_code_units() > 128)
        input = Utf16String::from_utf16(input.utf16_view().substring_view(0, 128));

    // 8. If the first character in input is U+0023 (#), then remove it.
    if (input.code_unit_at(0) == '#')
        input = Utf16String::from_utf16(input.utf16_view().substring_view(1));

    // 9. Replace any character in input that is not an ASCII hex digit with U+0030 (0).
    auto replace_non_ascii_hex = [](Utf16View string) -> Utf16String {
        Utf16StringBuilder builder;
        for (auto code_point : string) {
            if (is_ascii_hex_digit(code_point))
                builder.append_code_point(code_point);
            else
                builder.append_code_point('0');
        }
        return builder.to_string();
    };
    input = replace_non_ascii_hex(input);

    // 10. While input's code point length is zero or not a multiple of three, append U+0030 (0) to input.
    Utf16StringBuilder builder;
    builder.append(input);
    while (builder.length_in_code_units() == 0 || (builder.length_in_code_units() % 3 != 0))
        builder.append_code_point('0');
    input = builder.to_string();

    // 11. Split input into three strings of equal code point length, to obtain three components. Let length be the code point length that all of those components have (one third the code point length of input).
    auto length = input.length_in_code_units() / 3;
    auto first_component = input.utf16_view().substring_view(0, length);
    auto second_component = input.utf16_view().substring_view(length, length);
    auto third_component = input.utf16_view().substring_view(length * 2, length);

    // 12. If length is greater than 8, then remove the leading length-8 characters in each component, and let length be 8.
    if (length > 8) {
        first_component = first_component.substring_view(length - 8);
        second_component = second_component.substring_view(length - 8);
        third_component = third_component.substring_view(length - 8);
        length = 8;
    }

    // 13. While length is greater than two and the first character in each component is U+0030 (0), remove that character and reduce length by one.
    while (length > 2
        && first_component.code_unit_at(0) == '0'
        && second_component.code_unit_at(0) == '0'
        && third_component.code_unit_at(0) == '0') {
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

    auto to_hex = [&](Utf16View string) -> u8 {
        if (length == 1) {
            return hex_nibble_to_u8(string.code_unit_at(0));
        }
        auto nib1 = hex_nibble_to_u8(string.code_unit_at(0));
        auto nib2 = hex_nibble_to_u8(string.code_unit_at(1));
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
RefPtr<CSS::StyleValue const> parse_table_child_element_align_value(Utf16View string_view)
{
    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for either the string "center" or the string "middle", are expected to center text within
    // themselves, as if they had their 'text-align' property set to 'center' in a presentational hint, and to align
    // descendants to the center.
    if (string_view.equals_ignoring_ascii_case(u"center"sv) || string_view.equals_ignoring_ascii_case(u"middle"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::LibwebCenter);

    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for the string "left", are expected to left-align text within themselves, as if they had
    // their 'text-align' property set to 'left' in a presentational hint, and to align descendants to the left.
    if (string_view.equals_ignoring_ascii_case(u"left"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::LibwebLeft);

    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for the string "right", are expected to right-align text within themselves, as if they
    // had their 'text-align' property set to 'right' in a presentational hint, and to align descendants to the right.
    if (string_view.equals_ignoring_ascii_case(u"right"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::LibwebRight);

    // The thead, tbody, tfoot, tr, td, and th elements, when they have an align attribute whose value is an ASCII
    // case-insensitive match for the string "justify", are expected to full-justify text within themselves, as if they
    // had their 'text-align' property set to 'justify' in a presentational hint, and to align descendants to the left.
    if (string_view.equals_ignoring_ascii_case(u"justify"sv))
        return CSS::KeywordStyleValue::create(CSS::Keyword::Justify);

    return nullptr;
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
    // NOTE: The Rust preload scanner emits speculative fetch candidates directly, so we do not materialize a
    // speculativeDoc tree or speculative mock elements.
    auto speculative_parser = SpeculativeHTMLParser::create(*m_document, m_tokenizer.unparsed_input(), m_document->base_url());

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

extern "C" void ladybird_html_parser_log_parse_error(void* parser, u8 const* message_ptr, size_t message_len)
{
    (void)parser_from_html_parser_ffi(parser);
    dbgln_if(HTML_PARSER_DEBUG, "Rust parser parse error: {}", ffi_string_view(message_ptr, message_len));
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

static Optional<Utf16FlyString> namespace_from_html_parser_ffi(RustFfiHtmlNamespace namespace_, u16 const* namespace_uri_ptr, size_t namespace_uri_len)
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
        return utf16_fly_string_from_ffi(namespace_uri_ptr, namespace_uri_len);
    }
    VERIFY_NOT_REACHED();
}

static Optional<Utf16FlyString> attribute_namespace_from_html_parser_ffi(RustFfiHtmlAttributeNamespace namespace_)
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

static RustFfiHtmlAttributeNamespace attribute_namespace_to_html_parser_ffi(Optional<Utf16FlyString> const& namespace_)
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

static RustFfiHtmlNamespace namespace_to_html_parser_ffi(Optional<Utf16FlyString> const& namespace_)
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

struct NodeAndOffset {
    GC::Ref<DOM::Node> node;
    size_t offset;

    static constexpr auto append_child_offset = NumericLimits<decltype(offset)>::max();

    bool should_append() const
    {
        return offset == append_child_offset;
    }

    DOM::Node* child_at_offset() const
    {
        if (should_append())
            return nullptr;

        VERIFY(offset <= node->child_count());
        return node->child_at_index(offset);
    }

    DOM::Node* previous_child() const
    {
        if (should_append())
            return node->last_child();
        if (offset == 0)
            return nullptr;
        return node->child_at_index(offset - 1);
    }
};

static u64 s_parser_non_append_insertions { 0 };

u64 parser_non_append_insertions()
{
    return s_parser_non_append_insertions;
}

static NodeAndOffset node_and_offset_from_html_parser_ffi(size_t node, size_t offset)
{
    auto& dom_node = node_from_html_parser_ffi(node);
    VERIFY(offset == NodeAndOffset::append_child_offset || offset <= dom_node.child_count());
    if (offset != NodeAndOffset::append_child_offset)
        ++s_parser_non_append_insertions;
    return { dom_node, offset };
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

extern "C" size_t ladybird_html_parser_create_document_type(void* parser, u16 const* name_ptr, size_t name_len, u16 const* public_id_ptr, size_t public_id_len, u16 const* system_id_ptr, size_t system_id_len)
{
    auto& html_parser = parser_from_html_parser_ffi(parser);
    auto document_type = DOM::DocumentType::create(html_parser.document());
    document_type->set_name(utf16_fly_string_from_ffi(name_ptr, name_len));
    auto public_id = utf16_string_from_ffi(public_id_ptr, public_id_len);
    auto system_id = utf16_string_from_ffi(system_id_ptr, system_id_len);
    document_type->set_public_id(public_id.utf16_view());
    document_type->set_system_id(system_id.utf16_view());
    return reinterpret_cast<size_t>(document_type.ptr());
}

extern "C" size_t ladybird_html_parser_create_comment(void* parser, u16 const* data_ptr, size_t data_len)
{
    auto& html_parser = parser_from_html_parser_ffi(parser);
    auto comment = DOM::Comment::create(html_parser.document(), utf16_string_from_ffi(data_ptr, data_len));
    return reinterpret_cast<size_t>(comment.ptr());
}

// https://html.spec.whatwg.org/multipage/parsing.html#insert-a-character
extern "C" void ladybird_html_parser_insert_text(size_t parent, size_t offset, u8 const* data_ptr, size_t data_len)
{
    auto insertion_location = node_and_offset_from_html_parser_ffi(parent, offset);
    auto& parent_node = *insertion_location.node;

    // 3. If insertionLocation is in a Document node, then return.
    // NOTE: The DOM will not let Document nodes have Text node children, so they are dropped on the floor.
    if (parent_node.is_document())
        return;

    auto data = utf8_string_from_ffi(data_ptr, data_len);
    if (auto* previous_text = as_if<DOM::Text>(insertion_location.previous_child())) {
        (void)previous_text->append_data(data);
        return;
    }

    if (auto* before_node = insertion_location.child_at_offset()) {
        auto text = DOM::Text::create(parent_node.document(), data);
        parent_node.insert_before(*text, before_node);
        return;
    }

    auto text = DOM::Text::create(parent_node.document(), data);
    MUST(parent_node.append_child(*text));
}

extern "C" void ladybird_html_parser_add_missing_attribute(size_t element, size_t local_name_raw, u16 const* value_ptr, size_t value_len)
{
    auto& dom_element = as<DOM::Element>(node_from_html_parser_ffi(element));
    auto local_name = Utf16FlyString::from_raw(local_name_raw);
    if (dom_element.has_attribute(local_name))
        return;
    auto value = utf16_string_from_ffi(value_ptr, value_len);
    dom_element.append_attribute(DOM::QualifiedName { move(local_name), {}, {} }, move(value));
}

extern "C" void ladybird_html_parser_remove_node(size_t node)
{
    node_from_html_parser_ffi(node).remove(true);
}

extern "C" void ladybird_html_parser_handle_element_popped(size_t element)
{
    // https://html.spec.whatwg.org/multipage/semantics.html#update-a-style-block
    // When a style element is popped off the stack of open elements of an HTML parser or XML parser,
    // the user agent must run the update a style block algorithm.
    if (auto* style_element = as_if<DOM::StyleElementBase>(node_from_html_parser_ffi(element)))
        style_element->did_pop_off_parser_stack_of_open_elements();

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

extern "C" void ladybird_html_parser_process_meta_element(void* parser, size_t element)
{
    parser_from_html_parser_ffi(parser).process_meta_element_from_rust_parser(as<HTMLMetaElement>(node_from_html_parser_ffi(element)));
}

extern "C" size_t ladybird_html_parser_parent_node(size_t node)
{
    auto* parent = node_from_html_parser_ffi(node).parent();
    return reinterpret_cast<size_t>(parent);
}

extern "C" size_t ladybird_html_parser_node_index(size_t node)
{
    return node_from_html_parser_ffi(node).index();
}

extern "C" size_t ladybird_html_parser_create_element(void* parser, size_t intended_parent, RustFfiHtmlNamespace namespace_, u16 const* namespace_uri_ptr, size_t namespace_uri_len, size_t local_name_raw, RustFfiHtmlParserAttribute const* attributes, size_t attribute_count, bool had_duplicate_attribute, size_t form_element, bool has_template_element_on_stack)
{
    auto& html_parser = parser_from_html_parser_ffi(parser);
    auto local_name = Utf16FlyString::from_raw(local_name_raw);
    auto& intended_parent_node = node_from_html_parser_ffi(intended_parent);
    GC::Ptr<HTMLFormElement> form_element_ptr;
    if (form_element)
        form_element_ptr = as<HTMLFormElement>(node_from_html_parser_ffi(form_element));
    auto element = html_parser.create_element_for_rust_parser(local_name, { attributes, attribute_count }, namespace_from_html_parser_ffi(namespace_, namespace_uri_ptr, namespace_uri_len), intended_parent_node, had_duplicate_attribute, form_element_ptr, has_template_element_on_stack);

    return reinterpret_cast<size_t>(element.ptr());
}

extern "C" void ladybird_html_parser_append_child(size_t parent, size_t child)
{
    MUST(node_from_html_parser_ffi(parent).append_child(node_from_html_parser_ffi(child)));
}

extern "C" void ladybird_html_parser_insert_node(size_t parent, size_t offset, size_t child, bool queue_custom_element_reactions)
{
    auto insertion_location = node_and_offset_from_html_parser_ffi(parent, offset);
    auto& parent_node = *insertion_location.node;
    auto& child_node = node_from_html_parser_ffi(child);
    auto* child_element = as_if<DOM::Element>(child_node);
    if (queue_custom_element_reactions && child_element)
        relevant_similar_origin_window_agent(*child_element).custom_element_reactions_stack.element_queue_stack.append({});

    if (auto* before_node = insertion_location.child_at_offset())
        parent_node.insert_before(child_node, before_node, false);
    else
        MUST(parent_node.append_child(child_node));

    if (queue_custom_element_reactions && child_element) {
        auto queue = relevant_similar_origin_window_agent(*child_element).custom_element_reactions_stack.element_queue_stack.take_last();
        invoke_custom_element_reactions(queue);
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
        mode == RustFfiHtmlShadowRootMode::Open ? Web::DOM::ShadowRootMode::Open : Web::DOM::ShadowRootMode::Closed,
        clonable,
        serializable,
        delegates_focus,
        slot_assignment == RustFfiHtmlSlotAssignmentMode::Manual ? Web::DOM::SlotAssignmentMode::Manual : Web::DOM::SlotAssignmentMode::Named,
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

extern "C" bool ladybird_html_parser_is_shadow_host(size_t node)
{
    auto* element = as_if<DOM::Element>(&node_from_html_parser_ffi(node));
    return element && element->is_shadow_host();
}

}
