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
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/PageSelector.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/GeneratedValueTypesParsing.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/Parser/TokenStream.h>
#include <LibWeb/CSS/Parser/Types.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

Optional<FeatureComparison> parse_feature_comparison(TokenStream<ComponentValue>&);

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

// The very large CSS Parser implementation code is broken up among several .cpp files:
// Parser.cpp contains the core parser algorithms, defined in https://drafts.csswg.org/css-syntax
// Everything else is in different *Parsing.cpp files
class Parser {
    AK_MAKE_NONCOPYABLE(Parser);
    AK_MAKE_NONMOVABLE(Parser);

public:
    static Parser create(ParsingParams const&, StringView input, StringView encoding = "utf-8"sv);
    static Parser create(ParsingParams const&, Utf16View input);
    static RefPtr<StyleValue const> parse_css_value_from_filtered_source(ParsingParams const&, Utf16View, PropertyID);

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

    RefPtr<Supports> parse_as_supports();

    RefPtr<StyleValue const> parse_as_css_value(PropertyID);
    RefPtr<StyleValue const> parse_as_descriptor_value(AtRuleID, DescriptorNameAndID const&);
    RefPtr<StyleValue const> parse_as_type(ValueType);
    RefPtr<StyleValue const> parse_entirely_as_type(ValueType);

    Optional<ComponentValue> parse_as_component_value();

    Vector<ComponentValue> parse_as_list_of_component_values();

    static NonnullRefPtr<StyleValue const> resolve_unresolved_style_value(ParsingParams const&, AbstractOrHypotheticalElement, ArbitrarySubstitutionReplacementContext const&, PropertyNameAndID const&, UnresolvedStyleValue const&, Optional<GuardedSubstitutionContexts&> = {});

    [[nodiscard]] NonnullRefPtr<StyleValue const> parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img = nullptr);

    enum class DisallowTopLevelCurlyBlocks : u8 {
        No,
        Yes,
    };
    static Optional<Vector<ComponentValue>> parse_declaration_value(TokenStream<ComponentValue>&, Optional<Token::Type> end_token_type = {});
    static Optional<ReadonlySpan<ComponentValue>> parse_declaration_value_as_span(TokenStream<ComponentValue>&, Optional<Token::Type> end_token_type = {}, DisallowTopLevelCurlyBlocks = DisallowTopLevelCurlyBlocks::No);

    NonnullRefPtr<StyleValue const> parse_with_a_syntax(Vector<ComponentValue> const& input, SyntaxNode const& syntax);

    OwnPtr<BooleanExpression> parse_if_condition(TokenStream<ComponentValue>&);

    template<typename Descriptors>
    GC::Ref<Descriptors> convert_to_descriptors(AtRuleID, Vector<Declaration> const& declarations);
    GC::Ref<CSSStyleProperties> convert_to_style_declaration(Vector<Declaration> const&);

    enum class ParseError : u8 {
        IncludesIgnoredVendorPrefix,
        SyntaxError,
    };
    template<typename T>
    using ParseErrorOr = ErrorOr<T, ParseError>;

    static ParseErrorOr<void> collect_arbitrary_substitution_function_presence(Vector<ComponentValue> const&, SubstitutionFunctionsPresence&);
    static ParseErrorOr<void> collect_arbitrary_substitution_function_presence(ComponentValue const&, SubstitutionFunctionsPresence&);

private:
    friend class RustSyntaxParser;
    Parser(ParsingParams const&, Utf16String source);
    TokenStream<Token>& token_stream();

    // "Parse a stylesheet" is intended to be the normal parser entry point, for parsing stylesheets.
    struct ParsedStyleSheet {
        Optional<::URL::URL> location;
        Vector<Rule> rules;
    };
    template<typename T>
    ParsedStyleSheet parse_a_stylesheet(TokenStream<T>&, Optional<::URL::URL> location);

    // "Parse a stylesheet’s contents" is intended for use by the CSSStyleSheet replace() method, and similar, which parse text into the contents of an existing stylesheet.
    template<typename T>
    Vector<Rule> parse_a_stylesheets_contents(TokenStream<T>&);

    // "Parse a block’s contents" is intended for parsing the contents of any block in CSS (including things like the style attribute),
    // and APIs such as the CSSStyleDeclaration cssText attribute.
    template<typename T>
    Vector<RuleOrListOfDeclarations> parse_a_blocks_contents(TokenStream<T>&);

    enum class Nested {
        No,
        Yes,
    };

    // "Parse a rule" is intended for use by the CSSStyleSheet#insertRule method, and similar functions which might exist, which parse text into a single rule.
    template<typename T>
    Optional<Rule> parse_a_rule(TokenStream<T>&, Nested nested = Nested::No);

    // "Parse a declaration" is used in @supports conditions. [CSS3-CONDITIONAL]
    template<typename T>
    Optional<Declaration> parse_a_declaration(TokenStream<T>&);

    // "Parse a component value" is for things that need to consume a single value, like the parsing rules for attr().
    template<typename T>
    Optional<ComponentValue> parse_a_component_value(TokenStream<T>&);

    // "Parse a list of component values" is for the contents of presentational attributes, which parse text into a single declaration’s value,
    // or for parsing a stand-alone selector [SELECT] or list of Media Queries [MEDIAQ], as in Selectors API or the media HTML attribute.
    template<typename T>
    Vector<ComponentValue> parse_a_list_of_component_values(TokenStream<T>&);

    template<typename T>
    Vector<Vector<ComponentValue>> parse_a_comma_separated_list_of_component_values(TokenStream<T>&);

    enum class SelectorType {
        Standalone,
        Relative
    };
    template<typename T>
    ParseErrorOr<SelectorList> parse_a_selector_list(TokenStream<T>&, SelectorType, SelectorParsingMode = SelectorParsingMode::Standard);

    template<typename T>
    ParseErrorOr<PageSelectorList> parse_a_page_selector_list(TokenStream<T>&);

    template<typename T>
    Vector<NonnullRefPtr<MediaQuery>> parse_a_media_query_list(TokenStream<T>&);
    template<typename T>
    RefPtr<Supports> parse_a_supports(TokenStream<T>&);

    template<typename T>
    [[nodiscard]] Vector<Rule> consume_a_stylesheets_contents(TokenStream<T>&);
    template<typename T>
    Optional<AtRule> consume_an_at_rule(TokenStream<T>&, Nested nested = Nested::No);
    struct InvalidRuleError { };
    template<typename T>
    Variant<Empty, QualifiedRule, InvalidRuleError> consume_a_qualified_rule(TokenStream<T>&, Optional<Token::Type> stop_token = {}, Nested = Nested::No);
    template<typename T>
    Vector<RuleOrListOfDeclarations> consume_a_block(TokenStream<T>&);
    template<typename T>
    Vector<RuleOrListOfDeclarations> consume_a_blocks_contents(TokenStream<T>&);
    enum class SaveOriginalText : u8 {
        No,
        Yes,
    };
    template<typename T>
    Optional<Declaration> consume_a_declaration(TokenStream<T>&, Nested = Nested::No, SaveOriginalText = SaveOriginalText::No);
    template<typename T>
    void consume_the_remnants_of_a_bad_declaration(TokenStream<T>&, Nested);
    template<typename T>
    [[nodiscard]] Vector<ComponentValue> consume_a_list_of_component_values(TokenStream<T>&, Optional<Token::Type> stop_token = {}, Nested = Nested::No);
    template<typename T>
    [[nodiscard]] ComponentValue consume_a_component_value(TokenStream<T>&);
    template<typename T>
    void consume_a_component_value_and_do_nothing(TokenStream<T>&);
    SimpleBlock consume_a_simple_block(TokenStream<Token>&);
    void consume_a_simple_block_and_do_nothing(TokenStream<Token>&);
    Function consume_a_function(TokenStream<Token>&);
    void consume_a_function_and_do_nothing(TokenStream<Token>&);
    // TODO: consume_a_unicode_range_value()

    OwnPtr<GeneralEnclosed> parse_general_enclosed(TokenStream<ComponentValue>&, MatchResult);

    enum class AllowBlankLayerName {
        No,
        Yes,
    };
    Optional<Utf16FlyString> parse_layer_name(TokenStream<ComponentValue>&, AllowBlankLayerName);
    Optional<Vector<Utf16FlyString>> parse_comma_separated_family_name_list(TokenStream<ComponentValue>&);

    struct FunctionPrelude {
        Utf16FlyString name;
        Vector<FunctionParameterInternal> parameters;
        NonnullRefPtr<SyntaxNode> return_type;
    };
    Optional<FunctionPrelude> parse_function_prelude(TokenStream<ComponentValue>&);

    bool is_valid_in_the_current_context(Declaration const&) const;
    bool is_valid_in_the_current_context(AtRule const&) const;
    bool is_valid_in_the_current_context(QualifiedRule const&) const;

    Vector<Percentage> parse_keyframe_selectors(TokenStream<ComponentValue>&);

    template<typename NestedDeclarationsRule>
    GC::Ptr<CSSRule> convert_to_rule(Rule const&, Nested);
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

    Optional<StylePropertyAndName> convert_to_style_property(Declaration const&);

    Optional<Descriptor> convert_to_descriptor(AtRuleID, Declaration const&);

    RefPtr<StyleValue const> parse_source_size_value(TokenStream<ComponentValue>&);

    RefPtr<StyleValue const> parse_value(ValueType, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_primitive_value(ValueType, TokenStream<ComponentValue>&, NumericRange const& = infinite_range);

    Optional<URL> parse_url_function(TokenStream<ComponentValue>&);
    RefPtr<URLStyleValue const> parse_url_value(TokenStream<ComponentValue>&);

    enum class ValueIsSubstituted : u8 {
        No,
        Yes,
    };
    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_css_value_from_source(PropertyID, Utf16View);
    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_css_value(PropertyID, TokenStream<ComponentValue>&, Optional<Utf16String> original_source_text = {}, ValueIsSubstituted = ValueIsSubstituted::No);
    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_css_value_in_rust(PropertyID, Utf16View source, Utf16View unresolved_source, Utf16View comparison_source, bool contains_attr_tainted_values, ValueIsSubstituted, bool retry_with_serialized_source = false, bool retry_without_document_urls = false, bool* needs_serialized_source = nullptr, bool* needs_document_urls = nullptr, Optional<PropertyID> direct_property_context = {});
    enum class FontDescriptorKind : u8 {
        FamilyName,
        SourceList,
        UnicodeRangeList,
    };
    Optional<RefPtr<StyleValue const>> parse_font_descriptor_value_in_rust(FontDescriptorKind, TokenStream<ComponentValue>&);
    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_descriptor_value(AtRuleID, DescriptorNameAndID const&, TokenStream<ComponentValue>&);
    Optional<Utf16FlyString> parse_custom_ident(TokenStream<ComponentValue>&, ReadonlySpan<Utf16View> blacklist);
    RefPtr<CustomIdentStyleValue const> parse_custom_ident_value(TokenStream<ComponentValue>&, ReadonlySpan<Utf16View> blacklist = {});
    Optional<Utf16FlyString> parse_dashed_ident(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_keyword_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_specific_keyword_value(TokenStream<ComponentValue>&, ReadonlySpan<Keyword>);
    RefPtr<StyleValue const> parse_color_value(TokenStream<ComponentValue>&);
    Optional<Utf16FlyString> parse_counter_style_name(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_nonnegative_integer_symbol_pair_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_ratio_value(TokenStream<ComponentValue>&);
    RefPtr<StringStyleValue const> parse_string_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_integer_value(TokenStream<ComponentValue>&, NumericRange const& accepted_range);
    RefPtr<StyleValue const> parse_length_value(TokenStream<ComponentValue>&, NumericRange const& accepted_range);
    RefPtr<StyleValue const> parse_number_value(TokenStream<ComponentValue>&, NumericRange const& accepted_range);
    RefPtr<StyleValue const> parse_percentage_value(TokenStream<ComponentValue>& tokens, NumericRange const& accepted_range);
    RefPtr<StyleValue const> parse_resolution_value(TokenStream<ComponentValue>&, NumericRange const& accepted_range);

    using ParseFunction = AK::Function<RefPtr<StyleValue const>(TokenStream<ComponentValue>&)>;
    RefPtr<StyleValueList const> parse_comma_separated_value_list(TokenStream<ComponentValue>&, ParseFunction);
    RefPtr<StyleValue const> parse_all_as_single_keyword_value(TokenStream<ComponentValue>&, Keyword);
    RefPtr<StyleValue const> parse_family_name_value(TokenStream<ComponentValue>&);

#define __ENUMERATE_GENERATED_CSS_VALUE_TYPE(value_type_name) \
    RefPtr<StyleValue const> parse_##value_type_name##_value(TokenStream<ComponentValue>& tokens);
    ENUMERATE_GENERATED_CSS_VALUE_TYPES
#undef __ENUMERATE_GENERATED_CSS_VALUE_TYPE

    NonnullRefPtr<MediaQuery> parse_media_query(TokenStream<ComponentValue>&);
    OwnPtr<BooleanExpression> parse_media_condition(TokenStream<ComponentValue>&);
    OwnPtr<MediaFeature> parse_media_feature(TokenStream<ComponentValue>&);
    Optional<MediaQuery::MediaType> parse_media_type(TokenStream<ComponentValue>&);
    Optional<FeatureValue> parse_media_feature_value(MediaFeatureID, TokenStream<ComponentValue>&);
    OwnPtr<SizeFeature> parse_size_feature(TokenStream<ComponentValue>&);
    Optional<FeatureValue> parse_size_feature_value(SizeFeatureID, TokenStream<ComponentValue>&);
    OwnPtr<BooleanExpression> parse_style_query(TokenStream<ComponentValue>&, MatchResult result_for_general_enclosed);
    OwnPtr<BooleanExpression> parse_style_feature(TokenStream<ComponentValue>&);

    template<typename FeatureID, typename FeatureAcceptsKeyword, typename FeatureAcceptsType>
    Optional<FeatureValue> parse_feature_value(FeatureID, TokenStream<ComponentValue>&, FeatureAcceptsKeyword, FeatureAcceptsType);

    using ParseTest = AK::Function<OwnPtr<BooleanExpression>(TokenStream<ComponentValue>&)> const&;
    OwnPtr<BooleanExpression> parse_boolean_expression(TokenStream<ComponentValue>&, MatchResult result_for_general_enclosed, ParseTest parse_test);
    OwnPtr<BooleanExpression> parse_boolean_expression_group(TokenStream<ComponentValue>&, MatchResult result_for_general_enclosed, ParseTest parse_test);

    OwnPtr<BooleanExpression> parse_supports_condition(TokenStream<ComponentValue>&);
    OwnPtr<BooleanExpression> parse_supports_feature(TokenStream<ComponentValue>&);
    OwnPtr<Supports::Declaration> parse_supports_declaration(TokenStream<ComponentValue>&);

    OwnPtr<BooleanExpression> parse_container_query_condition(TokenStream<ComponentValue>&);
    OwnPtr<BooleanExpression> parse_container_query_feature(TokenStream<ComponentValue>&);
    RefPtr<ContainerQuery> parse_container_query(TokenStream<ComponentValue>&);

    NonnullRefPtr<StyleValue const> resolve_unresolved_style_value(AbstractOrHypotheticalElement, GuardedSubstitutionContexts&, ArbitrarySubstitutionReplacementContext const&, PropertyNameAndID const&, UnresolvedStyleValue const&);

    static bool has_ignored_vendor_prefix(Utf16View);

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
    Vector<Token> m_tokens;
    OwnPtr<TokenStream<Token>> m_token_stream;

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
    bool context_allows_random_functions() const;
    Utf16FlyString random_value_sharing_auto_name() const;

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
RefPtr<CSS::Supports> parse_css_supports(CSS::Parser::ParsingParams const&, Utf16View);
Vector<CSS::Parser::ComponentValue> parse_component_values_list(CSS::Parser::ParsingParams const&, Utf16View);
ErrorOr<Utf16String> css_decode_bytes(Optional<StringView> const& environment_encoding, Optional<StringView> mime_type_charset, ReadonlyBytes encoded_string);
bool is_valid_custom_ident(Utf16View, ReadonlySpan<Utf16View> const& blacklist);

}
