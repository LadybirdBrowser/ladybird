/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/DOM/FragmentSerializationMode.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>
#include <LibWeb/HTML/Parser/ParserScriptingMode.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Platform/Timer.h>

struct RustFfiHtmlParserHandle;

namespace Web::SVG {

class SVGScriptElement;

}

namespace Web::HTML {

class HTMLScriptElement;
class HTMLFormElement;

class WEB_API HTMLParser final : public JS::Cell {
    GC_CELL(HTMLParser, JS::Cell);
    GC_DECLARE_ALLOCATOR(HTMLParser);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~HTMLParser() override;

    static GC::Ref<HTMLParser> create_for_scripting(DOM::Document&);
    static GC::Ref<HTMLParser> create_with_open_input_stream(DOM::Document&);
    static GC::Ref<HTMLParser> create_with_uncertain_encoding(DOM::Document&, ByteBuffer const& input, Optional<MimeSniff::MimeType> maybe_mime_type = {});
    static GC::Ref<HTMLParser> create(DOM::Document&, StringView input, ParserScriptingMode, StringView encoding);

    void run(HTMLTokenizer::StopAtInsertionPoint = HTMLTokenizer::StopAtInsertionPoint::No);
    void run(URL::URL const&, HTMLTokenizer::StopAtInsertionPoint = HTMLTokenizer::StopAtInsertionPoint::No);
    void run_until_completion(HTMLTokenizer::StopAtInsertionPoint = HTMLTokenizer::StopAtInsertionPoint::No);
    void pop_all_open_elements();

    static void the_end(GC::Ref<DOM::Document>, GC::Ptr<HTMLParser> = nullptr);

    DOM::Document& document();
    enum class AllowDeclarativeShadowRoots {
        No,
        Yes,
    };
    static WebIDL::ExceptionOr<Vector<GC::Root<DOM::Node>>> parse_html_fragment(DOM::Element& context_element, StringView markup, AllowDeclarativeShadowRoots = AllowDeclarativeShadowRoots::No, ParserScriptingMode = ParserScriptingMode::Inert);

    enum class SerializableShadowRoots {
        No,
        Yes,
    };
    static String serialize_html_fragment(DOM::Node const&, SerializableShadowRoots, Vector<GC::Root<DOM::ShadowRoot>> const&, DOM::FragmentSerializationMode = DOM::FragmentSerializationMode::Inner);

    HTMLTokenizer& tokenizer() { return m_tokenizer; }

    void configure_element_created_by_rust_parser(DOM::Element&);
    GC::Ref<DOM::Element> create_element_for_rust_parser(HTMLToken const&, Optional<FlyString> const& namespace_, DOM::Node& intended_parent, bool had_duplicate_attribute, GC::Ptr<HTMLFormElement>, bool has_template_element_on_stack);
    void prepare_svg_script_for_rust_parser(SVG::SVGScriptElement&, size_t source_line_number);
    void set_script_source_line_from_rust_parser(DOM::Element&, size_t source_line_number);
    void mark_script_already_started_from_rust_parser(HTMLScriptElement&);
    void stop_parsing_from_rust_parser();
    bool process_script_end_tag_from_rust_parser(HTMLScriptElement&);
    bool process_svg_script_end_tag_from_rust_parser(SVG::SVGScriptElement&);

    // https://html.spec.whatwg.org/multipage/parsing.html#abort-a-parser
    void abort();

    bool aborted() const { return m_aborted; }
    bool stopped() const { return m_stop_parsing; }
    bool is_paused() const { return m_parser_pause_flag; }
    bool is_script_created() const { return m_script_created; }

    size_t script_nesting_level() const { return m_script_nesting_level; }

    void schedule_resume_check();
    void set_post_parse_action(Function<void()> action) { m_post_parse_action = move(action); }
    void invoke_post_parse_action_for_testing() { invoke_post_parse_action(); }

private:
    enum class ScriptCreatedParser {
        No,
        Yes,
    };

    HTMLParser(DOM::Document&, ParserScriptingMode, StringView input, StringView encoding);
    HTMLParser(DOM::Document&, ParserScriptingMode, ScriptCreatedParser);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;

    void stop_parsing() { m_stop_parsing = true; }

    // https://html.spec.whatwg.org/multipage/parsing.html#start-the-speculative-html-parser
    void start_the_speculative_html_parser();
    // https://html.spec.whatwg.org/multipage/parsing.html#stop-the-speculative-html-parser
    void stop_the_speculative_html_parser();

    GC::Ref<DOM::Element> create_element_for(HTMLToken const&, Optional<FlyString> const& namespace_, DOM::Node& intended_parent);
    void increment_script_nesting_level();
    void decrement_script_nesting_level();

    void resume_after_parser_blocking_script();
    void invoke_post_parse_action();

    HTMLTokenizer m_tokenizer;
    RustFfiHtmlParserHandle* m_rust_parser { nullptr };

    bool m_parsing_fragment { false };

    // https://html.spec.whatwg.org/multipage/parsing.html#scripting-mode
    ParserScriptingMode m_scripting_mode {};
    bool m_script_created { false };

    bool m_aborted { false };
    bool m_parser_pause_flag { false };
    bool m_stop_parsing { false };
    bool m_resume_check_pending { false };
    size_t m_script_nesting_level { 0 };

    Function<void()> m_post_parse_action;

    JS::Realm& realm();

    GC::Ptr<DOM::Document> m_document;
    GC::Ptr<HTMLFormElement> m_form_element;
    GC::Ptr<DOM::Element> m_context_element;

    // https://html.spec.whatwg.org/multipage/parsing.html#active-speculative-html-parser
    GC::Ptr<SpeculativeHTMLParser> m_active_speculative_html_parser;
};

class HTMLParserEndState final : public JS::Cell {
    GC_CELL(HTMLParserEndState, JS::Cell);
    GC_DECLARE_ALLOCATOR(HTMLParserEndState);

public:
    static GC::Ref<HTMLParserEndState> create(GC::Ref<DOM::Document>, GC::Ptr<HTMLParser>);

    void schedule_progress_check();

private:
    enum class Phase {
        WaitingForDeferredScripts,
        WaitingForASAPScripts,
        WaitingForLoadEventDelay,
        Completed,
    };

    HTMLParserEndState(GC::Ref<DOM::Document>, GC::Ptr<HTMLParser>);

    virtual void visit_edges(Cell::Visitor&) override;

    void check_progress();
    void advance_to_asap_scripts_phase();
    void complete();

    Phase m_phase { Phase::WaitingForDeferredScripts };
    bool m_check_pending { false };

    GC::Ref<DOM::Document> m_document;
    GC::Ptr<HTMLParser> m_parser;
    GC::Ref<Platform::Timer> m_timeout;
};

RefPtr<CSS::StyleValue const> parse_dimension_value(StringView);
RefPtr<CSS::StyleValue const> parse_nonzero_dimension_value(StringView);
Optional<Color> parse_legacy_color_value(StringView);
RefPtr<CSS::StyleValue const> parse_table_child_element_align_value(StringView);

}
