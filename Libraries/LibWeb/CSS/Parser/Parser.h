/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/PageSelector.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/Parser/RustSyntaxHandle.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/Parser/SubstitutionFunctionsPresence.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

struct FunctionContext {
    Utf16FlyString name;
};

struct DescriptorContext {
    AtRuleID at_rule;
    DescriptorID descriptor;
};
enum SpecialContext : u8 {
    CanvasContextGenericValue,
    DOMMatrixInitString,
    MediaCondition,
    OnScreenCanvasContextFontValue
};
// FIXME: Use PropertyNameAndID instead of PropertyID as the context, for registered custom properties.
using ValueParsingContext = Variant<PropertyID, FunctionContext, DescriptorContext, SpecialContext>;

enum class ParsingMode {
    Normal,
    SVGPresentationAttribute, // See https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
};

enum class IsUAStyleSheet {
    Yes,
    No,
};

struct WEB_API ParsingParams {
    explicit ParsingParams(ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(ValueParsingContext);
    explicit ParsingParams(IsUAStyleSheet);
    explicit ParsingParams(DOM::Document const&, ParsingMode = ParsingMode::Normal);

    GC::Ptr<DOM::Document const> document;
    ParsingMode mode { ParsingMode::Normal };
    IsUAStyleSheet is_ua_style_sheet { IsUAStyleSheet::No };

    Vector<ValueParsingContext> value_context;
    Vector<RuleContext> rule_context;
    HashTable<Utf16FlyString> declared_namespaces;
};

struct DevToolsStyleDeclaration {
    Utf16FlyString name;
    Utf16String value;
    Important important { Important::No };
    bool is_custom_property { false };
    bool is_name_valid { false };
    bool is_valid { false };
};

WEB_API Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const&, StringView);
WEB_API Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const&, Utf16View);

// The CSS Parser facade is implemented across Parser.cpp and focused *Parsing.cpp files.
class Parser {
    AK_MAKE_NONCOPYABLE(Parser);
    AK_MAKE_NONMOVABLE(Parser);

public:
    static Parser create(ParsingParams const&, StringView input);
    static Parser create(ParsingParams const&, Utf16View input);
    static RefPtr<StyleValue const> parse_css_value_from_source(ParsingParams const&, Utf16View, PropertyID);

    GC::RootVector<GC::Ref<CSSRule>> convert_rules(Vector<Rule> const& raw_rules);
    GC::Ref<CSS::CSSStyleSheet> parse_as_css_stylesheet(Optional<::URL::URL> location, GC::Ptr<MediaList> = {});

    struct PropertiesAndCustomProperties {
        Vector<StyleProperty> properties;
        OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties;
    };
    PropertiesAndCustomProperties parse_as_property_declaration_block();
    Vector<DevToolsStyleDeclaration> parse_as_devtools_property_declaration_block();
    Vector<Descriptor> parse_as_descriptor_declaration_block(AtRuleID);
    CSSRule* parse_as_css_rule(bool nested = false);
    GC::Ptr<CSSKeyframeRule> parse_as_keyframe_rule();
    Vector<Percentage> parse_as_keyframe_selectors();
    GC::RootVector<GC::Ref<CSSRule>> parse_as_stylesheet_contents();

    enum class SelectorParsingMode {
        Standard,
        // `<forgiving-selector-list>` and `<forgiving-relative-selector-list>`
        // are handled with this parameter, not as separate functions.
        // https://drafts.csswg.org/selectors/#forgiving-selector
        Forgiving
    };
    // Contrary to the name, these parse a comma-separated list of selectors, according to the spec.
    Optional<SelectorList> parse_as_selector(SelectorParsingMode = SelectorParsingMode::Standard);
    Optional<SelectorList> parse_as_relative_selector(SelectorParsingMode = SelectorParsingMode::Standard);

    Optional<Selector::PseudoElementSelector> parse_as_pseudo_element_selector();

    Optional<PageSelectorList> parse_as_page_selector_list();

    Vector<NonnullRefPtr<MediaQuery>> parse_as_media_query_list();
    RefPtr<MediaQuery> parse_as_media_query();

    Optional<RustQueryHandle> parse_as_supports();

    RefPtr<StyleValue const> parse_as_css_value(PropertyID);
    RefPtr<StyleValue const> parse_as_descriptor_value(AtRuleID, DescriptorNameAndID const&);
    RefPtr<StyleValue const> parse_as_type(ValueType);
    RefPtr<StyleValue const> parse_entirely_as_type(ValueType);
    RefPtr<StyleValue const> parse_primitive_value_from_source(ValueType, Utf16View, NumericRange const& = infinite_range);

    [[nodiscard]] NonnullRefPtr<StyleValue const> parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img = nullptr);

    NonnullRefPtr<StyleValue const> parse_with_a_syntax(Utf16View input, RustSyntaxHandle const& syntax);
    NonnullRefPtr<StyleValue const> parse_with_a_syntax(RustSyntaxHandle const& syntax) { return parse_with_a_syntax(m_source, syntax); }

    template<typename Descriptors>
    GC::Ref<Descriptors> convert_to_descriptors(AtRuleID, Vector<Declaration> const& declarations);
    GC::Ref<CSSStyleProperties> convert_to_style_declaration(Vector<Declaration> const&);

    enum class ParseError : u8 {
        SyntaxError,
    };
    template<typename T>
    using ParseErrorOr = ErrorOr<T, ParseError>;

    static ParseErrorOr<void> collect_arbitrary_substitution_function_presence(Utf16View, SubstitutionFunctionsPresence&);

private:
    friend class RustSyntaxParser;
    friend class RustQueryParser;
    Parser(ParsingParams const&, Utf16String source);
    enum class Nested {
        No,
        Yes,
    };
    enum class ParseContextMode {
        Syntax,
        Value,
        RegisteredSyntax,
    };
    // Self-referential: `context` points into this object's storage.
    struct ParseContextStorage {
        AK_MAKE_NONCOPYABLE(ParseContextStorage);
        AK_MAKE_NONMOVABLE(ParseContextStorage);

    public:
        ParseContextStorage(Parser&, ParseContextMode, Optional<PropertyID>);

        Vector<ValueParserFFI::FfiValueParsingContext, 1> value_contexts;
        ValueParserFFI::FfiValueParsingContext single_property_context {};
        Vector<ValueParserFFI::FfiUtf16View> declared_namespaces;
        Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_resolution_context;
        ValueParserFFI::ParseContext context {};
    };

    template<typename NestedDeclarationsRule>
    GC::Ptr<CSSRule> convert_to_rule(Rule const&, Nested);
    template<typename NestedDeclarationsRule>
    GC::Ref<CSSRuleList> convert_child_rules(Vector<RuleOrListOfDeclarations> const&, Nested);
    GC::Ptr<CSSStyleRule> convert_to_style_rule(QualifiedRule const&, Nested);
    template<typename NestedDeclarationsRule>
    GC::Ptr<CSSContainerRule> convert_to_container_rule(AtRule const&, Nested);
    GC::Ptr<CSSCounterStyleRule> convert_to_counter_style_rule(AtRule const&);
    GC::Ptr<CSSFontFaceRule> convert_to_font_face_rule(AtRule const&);
    GC::Ptr<CSSFontFeatureValuesRule> convert_to_font_feature_values_rule(AtRule const&);
    GC::Ptr<CSSFunctionRule> convert_to_function_rule(AtRule const&);
    GC::Ptr<CSSKeyframeRule> convert_to_keyframe_rule(QualifiedRule const&);
    GC::Ptr<CSSKeyframesRule> convert_to_keyframes_rule(AtRule const&);
    GC::Ptr<CSSImportRule> convert_to_import_rule(AtRule const&);

    template<typename NestedDeclarationsRule>
    GC::Ptr<CSSRule> convert_to_layer_rule(AtRule const&, Nested);
    GC::Ptr<CSSMarginRule> convert_to_margin_rule(AtRule const&);

    template<typename NestedDeclarationsRule>
    GC::Ptr<CSSMediaRule> convert_to_media_rule(AtRule const&, Nested);
    GC::Ptr<CSSNamespaceRule> convert_to_namespace_rule(AtRule const&);
    GC::Ptr<CSSPageRule> convert_to_page_rule(AtRule const& rule);
    GC::Ptr<CSSPropertyRule> convert_to_property_rule(AtRule const& rule);

    template<typename NestedDeclarationsRule>
    GC::Ptr<CSSSupportsRule> convert_to_supports_rule(AtRule const&, Nested);
    template<typename NestedDeclarationsRule>
    GC::Ptr<CSSScopeRule> convert_to_scope_rule(AtRule const&, Nested);

    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_css_value_from_source(PropertyID, Utf16View);
    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_css_value_in_rust(PropertyID, Utf16View source, Optional<PropertyID> direct_property_context = {});
    ParseContextStorage make_parse_context(ParseContextMode, Optional<PropertyID> direct_property_context = {});
    void extract_property(Declaration const&, Parser::PropertiesAndCustomProperties&);

    DOM::Document const* document() const;
    HTML::Window const* window() const;
    bool in_quirks_mode() const;
    bool is_parsing_svg_presentation_attribute() const;

    GC::Ptr<DOM::Document const> m_document;
    Optional<String> m_serialized_document_url;
    Optional<String> m_serialized_document_base_url;
    ParsingMode m_parsing_mode { ParsingMode::Normal };
    IsUAStyleSheet m_is_ua_style_sheet { IsUAStyleSheet::No };

    Utf16String m_source;
    Vector<ValueParsingContext> m_value_context;
    size_t m_random_function_index = 0;
    auto push_temporary_value_parsing_context(ValueParsingContext&& context)
    {
        m_value_context.append(context);
        return ScopeGuard { [&] {
            auto removed_context = m_value_context.take_last();

            // Reset the random function index when we leave the top-level property parsing context
            if (removed_context.has<PropertyID>() && !m_value_context.find_first_index_if([](ValueParsingContext context) { return context.has<PropertyID>(); }).has_value())
                m_random_function_index = 0;
        } };
    }
    Vector<RuleContext> m_rule_context;
    HashTable<Utf16FlyString> m_declared_namespaces;
};

GC::Ptr<CSSKeyframeRule> parse_keyframe_rule(ParsingParams const&, Utf16View);
Vector<Percentage> parse_keyframe_selectors(ParsingParams const&, Utf16View);

}

namespace Web {

GC::Ref<CSS::CSSStyleSheet> parse_css_stylesheet(CSS::Parser::ParsingParams const&, StringView, Optional<::URL::URL> location = {}, GC::Ptr<CSS::MediaList> media_list = {});
GC::Ref<CSS::CSSStyleSheet> parse_css_stylesheet(CSS::Parser::ParsingParams const&, Utf16View, Optional<::URL::URL> location = {}, GC::Ptr<CSS::MediaList> media_list = {});
CSS::Parser::Parser::PropertiesAndCustomProperties parse_css_property_declaration_block(CSS::Parser::ParsingParams const&, Utf16View);
Vector<CSS::Descriptor> parse_css_descriptor_declaration_block(CSS::Parser::ParsingParams const&, CSS::AtRuleID, Utf16View);
RefPtr<CSS::StyleValue const> parse_css_value(CSS::Parser::ParsingParams const&, StringView, CSS::PropertyID);
RefPtr<CSS::StyleValue const> parse_css_value(CSS::Parser::ParsingParams const&, Utf16View, CSS::PropertyID);
RefPtr<CSS::StyleValue const> parse_css_type(CSS::Parser::ParsingParams const&, Utf16View, CSS::ValueType);
RefPtr<CSS::StyleValue const> parse_css_descriptor(CSS::Parser::ParsingParams const&, CSS::AtRuleID, CSS::DescriptorNameAndID const&, Utf16View);
Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingParams const&, Utf16View);
Optional<CSS::SelectorList> parse_selector_for_nested_style_rule(CSS::Parser::ParsingParams const&, Utf16View, CSS::StyleNestingParent);
Optional<CSS::PageSelectorList> parse_page_selector_list(CSS::Parser::ParsingParams const&, Utf16View);
Optional<CSS::Selector::PseudoElementSelector> parse_pseudo_element_selector(CSS::Parser::ParsingParams const&, Utf16View);
CSS::CSSRule* parse_css_rule(CSS::Parser::ParsingParams const&, Utf16View, bool nested = false);
RefPtr<CSS::MediaQuery> parse_media_query(CSS::Parser::ParsingParams const&, Utf16View);
Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(CSS::Parser::ParsingParams const&, Utf16View);
Optional<CSS::RustQueryHandle> parse_css_supports(CSS::Parser::ParsingParams const&, Utf16View);
WEB_API ErrorOr<Utf16String> css_decode_bytes(Optional<StringView> const& environment_encoding, Optional<StringView> mime_type_charset, ReadonlyBytes encoded_string);
bool is_valid_animation_name_custom_ident(Utf16View);

}
