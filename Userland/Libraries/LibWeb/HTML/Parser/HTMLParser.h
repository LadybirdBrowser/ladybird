/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>
#include <LibWeb/HTML/Parser/ListOfActiveFormattingElements.h>
#include <LibWeb/HTML/Parser/StackOfOpenElements.h>

namespace Web::HTML {

#define ENUMERATE_INSERTION_MODES               \
    __ENUMERATE_INSERTION_MODE(Initial)         \
    __ENUMERATE_INSERTION_MODE(BeforeHTML)      \
    __ENUMERATE_INSERTION_MODE(BeforeHead)      \
    __ENUMERATE_INSERTION_MODE(InHead)          \
    __ENUMERATE_INSERTION_MODE(InHeadNoscript)  \
    __ENUMERATE_INSERTION_MODE(AfterHead)       \
    __ENUMERATE_INSERTION_MODE(InBody)          \
    __ENUMERATE_INSERTION_MODE(Text)            \
    __ENUMERATE_INSERTION_MODE(InTable)         \
    __ENUMERATE_INSERTION_MODE(InTableText)     \
    __ENUMERATE_INSERTION_MODE(InCaption)       \
    __ENUMERATE_INSERTION_MODE(InColumnGroup)   \
    __ENUMERATE_INSERTION_MODE(InTableBody)     \
    __ENUMERATE_INSERTION_MODE(InRow)           \
    __ENUMERATE_INSERTION_MODE(InCell)          \
    __ENUMERATE_INSERTION_MODE(InSelect)        \
    __ENUMERATE_INSERTION_MODE(InSelectInTable) \
    __ENUMERATE_INSERTION_MODE(InTemplate)      \
    __ENUMERATE_INSERTION_MODE(AfterBody)       \
    __ENUMERATE_INSERTION_MODE(InFrameset)      \
    __ENUMERATE_INSERTION_MODE(AfterFrameset)   \
    __ENUMERATE_INSERTION_MODE(AfterAfterBody)  \
    __ENUMERATE_INSERTION_MODE(AfterAfterFrameset)

class HTMLParser final : public JS::Cell {
    JS_CELL(HTMLParser, JS::Cell);
    JS_DECLARE_ALLOCATOR(HTMLParser);

    friend class HTMLTokenizer;

public:
    ~HTMLParser();

    static JS::NonnullGCPtr<HTMLParser> create_for_scripting(DOM::Document&);
    static JS::NonnullGCPtr<HTMLParser> create_with_uncertain_encoding(DOM::Document&, ByteBuffer const& input);
    static JS::NonnullGCPtr<HTMLParser> create(DOM::Document&, StringView input, StringView encoding);

    void run(HTMLTokenizer::StopAtInsertionPoint = HTMLTokenizer::StopAtInsertionPoint::No);
    void run(const URL::URL&, HTMLTokenizer::StopAtInsertionPoint = HTMLTokenizer::StopAtInsertionPoint::No);

    static void the_end(JS::NonnullGCPtr<DOM::Document>, JS::GCPtr<HTMLParser> = nullptr);

    DOM::Document& document();
    enum class AllowDeclarativeShadowRoots {
        No,
        Yes,
    };
    static Vector<JS::Handle<DOM::Node>> parse_html_fragment(DOM::Element& context_element, StringView, AllowDeclarativeShadowRoots = AllowDeclarativeShadowRoots::No);
    enum class SerializableShadowRoots {
        No,
        Yes,
    };
    static String serialize_html_fragment(DOM::Node const&, SerializableShadowRoots, Vector<JS::Handle<DOM::ShadowRoot>> const&, DOM::FragmentSerializationMode = DOM::FragmentSerializationMode::Inner);

    enum class InsertionMode {
#define __ENUMERATE_INSERTION_MODE(mode) mode,
        ENUMERATE_INSERTION_MODES
#undef __ENUMERATE_INSERTION_MODE
    };

    InsertionMode insertion_mode() const { return m_insertion_mode; }

    static bool is_special_tag(FlyString const& tag_name, Optional<FlyString> const& namespace_);

    HTMLTokenizer& tokenizer() { return m_tokenizer; }

    // https://html.spec.whatwg.org/multipage/parsing.html#abort-a-parser
    void abort();

    bool aborted() const { return m_aborted; }
    bool stopped() const { return m_stop_parsing; }

    size_t script_nesting_level() const { return m_script_nesting_level; }

private:
    HTMLParser(DOM::Document&, StringView input, StringView encoding);
    HTMLParser(DOM::Document&);

    virtual void visit_edges(Cell::Visitor&) override;

    char const* insertion_mode_name() const;

    DOM::QuirksMode which_quirks_mode(HTMLToken const&) const;

    void handle_initial(HTMLToken&);
    void handle_before_html(HTMLToken&);
    void handle_before_head(HTMLToken&);
    void handle_in_head(HTMLToken&);
    void handle_in_head_noscript(HTMLToken&);
    void handle_after_head(HTMLToken&);
    void handle_in_body(HTMLToken&);
    void handle_after_body(HTMLToken&);
    void handle_after_after_body(HTMLToken&);
    void handle_text(HTMLToken&);
    void handle_in_table(HTMLToken&);
    void handle_in_table_body(HTMLToken&);
    void handle_in_row(HTMLToken&);
    void handle_in_cell(HTMLToken&);
    void handle_in_table_text(HTMLToken&);
    void handle_in_select_in_table(HTMLToken&);
    void handle_in_select(HTMLToken&);
    void handle_in_caption(HTMLToken&);
    void handle_in_column_group(HTMLToken&);
    void handle_in_template(HTMLToken&);
    void handle_in_frameset(HTMLToken&);
    void handle_after_frameset(HTMLToken&);
    void handle_after_after_frameset(HTMLToken&);

    void stop_parsing() { m_stop_parsing = true; }

    void generate_implied_end_tags(FlyString const& exception = {});
    void generate_all_implied_end_tags_thoroughly();
    JS::NonnullGCPtr<DOM::Element> create_element_for(HTMLToken const&, Optional<FlyString> const& namespace_, DOM::Node& intended_parent);

    struct AdjustedInsertionLocation {
        JS::GCPtr<DOM::Node> parent;
        JS::GCPtr<DOM::Node> insert_before_sibling;
    };

    AdjustedInsertionLocation find_appropriate_place_for_inserting_node(JS::GCPtr<DOM::Element> override_target = nullptr);

    void insert_an_element_at_the_adjusted_insertion_location(JS::NonnullGCPtr<DOM::Element>);

    DOM::Text* find_character_insertion_node();
    void flush_character_insertions();
    enum class OnlyAddToElementStack {
        No,
        Yes,
    };
    JS::NonnullGCPtr<DOM::Element> insert_foreign_element(HTMLToken const&, Optional<FlyString> const& namespace_, OnlyAddToElementStack);
    JS::NonnullGCPtr<DOM::Element> insert_html_element(HTMLToken const&);
    DOM::Element& current_node();
    DOM::Element& adjusted_current_node();
    DOM::Element& node_before_current_node();
    void insert_character(u32 data);
    void insert_comment(HTMLToken&);
    void reconstruct_the_active_formatting_elements();
    void close_a_p_element();
    void process_using_the_rules_for(InsertionMode, HTMLToken&);
    void process_using_the_rules_for_foreign_content(HTMLToken&);
    void parse_generic_raw_text_element(HTMLToken&);
    void increment_script_nesting_level();
    void decrement_script_nesting_level();
    void reset_the_insertion_mode_appropriately();

    void adjust_mathml_attributes(HTMLToken&);
    void adjust_svg_tag_names(HTMLToken&);
    void adjust_svg_attributes(HTMLToken&);
    static void adjust_foreign_attributes(HTMLToken&);

    enum AdoptionAgencyAlgorithmOutcome {
        DoNothing,
        RunAnyOtherEndTagSteps,
    };

    AdoptionAgencyAlgorithmOutcome run_the_adoption_agency_algorithm(HTMLToken&);
    void clear_the_stack_back_to_a_table_context();
    void clear_the_stack_back_to_a_table_body_context();
    void clear_the_stack_back_to_a_table_row_context();
    void close_the_cell();

    InsertionMode m_insertion_mode { InsertionMode::Initial };
    InsertionMode m_original_insertion_mode { InsertionMode::Initial };

    StackOfOpenElements m_stack_of_open_elements;
    Vector<InsertionMode> m_stack_of_template_insertion_modes;
    ListOfActiveFormattingElements m_list_of_active_formatting_elements;

    HTMLTokenizer m_tokenizer;

    bool m_foster_parenting { false };
    bool m_frameset_ok { true };
    bool m_parsing_fragment { false };

    // https://html.spec.whatwg.org/multipage/parsing.html#scripting-flag
    // The scripting flag is set to "enabled" if scripting was enabled for the Document with which the parser is associated when the parser was created, and "disabled" otherwise.
    bool m_scripting_enabled { true };

    bool m_invoked_via_document_write { false };
    bool m_aborted { false };
    bool m_parser_pause_flag { false };
    bool m_stop_parsing { false };
    size_t m_script_nesting_level { 0 };

    JS::Realm& realm();

    JS::GCPtr<DOM::Document> m_document;
    JS::GCPtr<HTMLHeadElement> m_head_element;
    JS::GCPtr<HTMLFormElement> m_form_element;
    JS::GCPtr<DOM::Element> m_context_element;

    Vector<HTMLToken> m_pending_table_character_tokens;

    JS::GCPtr<DOM::Text> m_character_insertion_node;
    StringBuilder m_character_insertion_builder;
};

RefPtr<CSS::StyleValue> parse_dimension_value(StringView);
RefPtr<CSS::StyleValue> parse_nonzero_dimension_value(StringView);
Optional<Color> parse_legacy_color_value(StringView);

}
