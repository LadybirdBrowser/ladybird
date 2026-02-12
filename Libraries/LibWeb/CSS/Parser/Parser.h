/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRawPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/PageSelector.h>
#include <LibWeb/CSS/ParsedFontFace.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Dimension.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/Parser/TokenStream.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>
#include <LibWeb/CSS/Parser/Types.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/TreeCountingFunctionStyleValue.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

namespace CalcParsing {

struct Operator {
    char delim;
};
struct ProductNode;
struct SumNode;
struct InvertNode;
struct NegateNode;
using Node = Variant<Operator, NonnullOwnPtr<ProductNode>, NonnullOwnPtr<SumNode>, NonnullOwnPtr<InvertNode>, NonnullOwnPtr<NegateNode>, NonnullRawPtr<ComponentValue const>>;
struct ProductNode {
    Vector<Node> children;
};
struct SumNode {
    Vector<Node> children;
};
struct InvertNode {
    Node child;
};
struct NegateNode {
    Node child;
};

}

struct FunctionContext {
    StringView name;
};
struct DescriptorContext {
    AtRuleID at_rule;
    DescriptorID descriptor;
};
enum SpecialContext : u8 {
    AngularColorStopList,
    BorderRadius,
    CubicBezierFunctionXCoordinate,
    DOMMatrixInitString,
    FontStyleAngle,
    MediaCondition,
    RadialSizeLengthPercentage,
    RandomValueSharingFixedValue,
    ShadowBlurRadius,
    StepsIntervalsJumpNone,
    StepsIntervalsNormal,
    TranslateZArgument,
};
// FIXME: Use PropertyNameAndID instead of PropertyID as the context, for registered custom properties.
using ValueParsingContext = Variant<PropertyID, FunctionContext, DescriptorContext, SpecialContext>;

enum class ParsingMode {
    Normal,
    SVGPresentationAttribute, // See https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
};

struct ParsingParams {
    explicit ParsingParams(ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(JS::Realm&, ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(DOM::Document const&, ParsingMode = ParsingMode::Normal);

    GC::Ptr<JS::Realm> realm;
    GC::Ptr<DOM::Document const> document;
    ParsingMode mode { ParsingMode::Normal };

    Vector<ValueParsingContext> value_context;
    Vector<RuleContext> rule_context;
    HashTable<FlyString> declared_namespaces;
};

// The very large CSS Parser implementation code is broken up among several .cpp files:
// Parser.cpp contains the core parser algorithms, defined in https://drafts.csswg.org/css-syntax
// Everything else is in different *Parsing.cpp files
class Parser {
    AK_MAKE_NONCOPYABLE(Parser);
    AK_MAKE_NONMOVABLE(Parser);

public:
    static Parser create(ParsingParams const&, StringView input, StringView encoding = "utf-8"sv);

    GC::RootVector<GC::Ref<CSSRule>> convert_rules(Vector<Rule> const& raw_rules);
    GC::Ref<CSS::CSSStyleSheet> parse_as_css_stylesheet(Optional<::URL::URL> location, GC::Ptr<MediaList> = {});

    struct PropertiesAndCustomProperties {
        Vector<StyleProperty> properties;
        OrderedHashMap<FlyString, StyleProperty> custom_properties;
    };
    PropertiesAndCustomProperties parse_as_property_declaration_block();
    Vector<Descriptor> parse_as_descriptor_declaration_block(AtRuleID);
    CSSRule* parse_as_css_rule();
    Optional<StyleProperty> parse_as_supports_condition();
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
    RefPtr<StyleValue const> parse_as_descriptor_value(AtRuleID, DescriptorID);
    RefPtr<StyleValue const> parse_as_type(ValueType);

    Optional<ComponentValue> parse_as_component_value();

    Vector<ComponentValue> parse_as_list_of_component_values();

    static NonnullRefPtr<StyleValue const> resolve_unresolved_style_value(ParsingParams const&, DOM::AbstractElement, PropertyNameAndID const&, UnresolvedStyleValue const&, Optional<GuardedSubstitutionContexts&> = {});

    [[nodiscard]] LengthOrCalculated parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img = nullptr);

    enum class StopAtComma : u8 {
        No,
        Yes,
    };
    static Optional<Vector<ComponentValue>> parse_declaration_value(TokenStream<ComponentValue>&, StopAtComma = StopAtComma::No);

    NonnullRefPtr<StyleValue const> parse_with_a_syntax(Vector<ComponentValue> const& input, SyntaxNode const& syntax, Optional<DOM::AbstractElement> const& element = {});

    RefPtr<CalculatedStyleValue const> parse_calculated_value(ComponentValue const&);
    RefPtr<TreeCountingFunctionStyleValue const> parse_tree_counting_function(TokenStream<ComponentValue>&, TreeCountingFunctionStyleValue::ComputedType);

private:
    Parser(ParsingParams const&, Vector<Token>);

    enum class ParseError : u8 {
        IncludesIgnoredVendorPrefix,
        SyntaxError,
    };
    template<typename T>
    using ParseErrorOr = ErrorOr<T, ParseError>;

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

    // "Parse a rule" is intended for use by the CSSStyleSheet#insertRule method, and similar functions which might exist, which parse text into a single rule.
    template<typename T>
    Optional<Rule> parse_a_rule(TokenStream<T>&);

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

    Optional<Selector::SimpleSelector::ANPlusBPattern> parse_a_n_plus_b_pattern(TokenStream<ComponentValue>&);

    template<typename T>
    [[nodiscard]] Vector<Rule> consume_a_stylesheets_contents(TokenStream<T>&);
    enum class Nested {
        No,
        Yes,
    };
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
    Optional<FlyString> parse_layer_name(TokenStream<ComponentValue>&, AllowBlankLayerName);

    bool is_valid_in_the_current_context(Declaration const&) const;
    bool is_valid_in_the_current_context(AtRule const&) const;
    bool is_valid_in_the_current_context(QualifiedRule const&) const;
    GC::Ptr<CSSRule> convert_to_rule(Rule const&, Nested);
    GC::Ptr<CSSStyleRule> convert_to_style_rule(QualifiedRule const&, Nested);
    GC::Ptr<CSSCounterStyleRule> convert_to_counter_style_rule(AtRule const&);
    GC::Ptr<CSSFontFaceRule> convert_to_font_face_rule(AtRule const&);
    GC::Ptr<CSSKeyframesRule> convert_to_keyframes_rule(AtRule const&);
    GC::Ptr<CSSImportRule> convert_to_import_rule(AtRule const&);
    GC::Ptr<CSSRule> convert_to_layer_rule(AtRule const&, Nested);
    GC::Ptr<CSSMarginRule> convert_to_margin_rule(AtRule const&);
    GC::Ptr<CSSMediaRule> convert_to_media_rule(AtRule const&, Nested);
    GC::Ptr<CSSNamespaceRule> convert_to_namespace_rule(AtRule const&);
    GC::Ptr<CSSPageRule> convert_to_page_rule(AtRule const& rule);
    GC::Ptr<CSSPropertyRule> convert_to_property_rule(AtRule const& rule);
    GC::Ptr<CSSSupportsRule> convert_to_supports_rule(AtRule const&, Nested);

    GC::Ref<CSSStyleProperties> convert_to_style_declaration(Vector<Declaration> const&);
    Optional<StylePropertyAndName> convert_to_style_property(Declaration const&);

    Optional<Descriptor> convert_to_descriptor(AtRuleID, Declaration const&);

    Optional<Dimension> parse_dimension(ComponentValue const&);
    Optional<AngleOrCalculated> parse_angle(TokenStream<ComponentValue>&);
    Optional<AnglePercentage> parse_angle_percentage(TokenStream<ComponentValue>&);
    Optional<FlexOrCalculated> parse_flex(TokenStream<ComponentValue>&);
    Optional<FrequencyOrCalculated> parse_frequency(TokenStream<ComponentValue>&);
    Optional<FrequencyPercentage> parse_frequency_percentage(TokenStream<ComponentValue>&);
    Optional<IntegerOrCalculated> parse_integer(TokenStream<ComponentValue>&);
    Optional<LengthOrCalculated> parse_length(TokenStream<ComponentValue>&);
    Optional<LengthPercentage> parse_length_percentage(TokenStream<ComponentValue>&);
    Optional<NumberOrCalculated> parse_number(TokenStream<ComponentValue>&);
    Optional<NumberPercentage> parse_number_percentage(TokenStream<ComponentValue>&);
    Optional<ResolutionOrCalculated> parse_resolution(TokenStream<ComponentValue>&);
    Optional<TimeOrCalculated> parse_time(TokenStream<ComponentValue>&);
    Optional<TimePercentage> parse_time_percentage(TokenStream<ComponentValue>&);

    Optional<LengthOrAutoOrCalculated> parse_source_size_value(TokenStream<ComponentValue>&);
    Optional<Ratio> parse_ratio(TokenStream<ComponentValue>&);
    Optional<Gfx::UnicodeRange> parse_unicode_range(TokenStream<ComponentValue>&);
    Optional<Gfx::UnicodeRange> parse_unicode_range(StringView);
    Vector<Gfx::UnicodeRange> parse_unicode_ranges(TokenStream<ComponentValue>&);
    RefPtr<UnicodeRangeStyleValue const> parse_unicode_range_value(TokenStream<ComponentValue>&);

    RefPtr<StyleValue const> parse_value(ValueType, TokenStream<ComponentValue>&);

    Optional<GridSize> parse_grid_track_breadth(TokenStream<ComponentValue>&);
    Optional<GridSize> parse_grid_inflexible_breadth(TokenStream<ComponentValue>&);
    Optional<LengthPercentage> parse_grid_fixed_breadth(TokenStream<ComponentValue>&);

    Optional<GridLineNames> parse_grid_line_names(TokenStream<ComponentValue>&);

    Optional<GridRepeat> parse_grid_track_repeat(TokenStream<ComponentValue>&);
    Optional<GridRepeat> parse_grid_auto_repeat(TokenStream<ComponentValue>&);
    Optional<GridRepeat> parse_grid_fixed_repeat(TokenStream<ComponentValue>&);

    using GridRepeatTypeParser = AK::Function<Optional<GridRepeatParams>(TokenStream<ComponentValue>&)>;
    using GridTrackParser = AK::Function<Optional<ExplicitGridTrack>(TokenStream<ComponentValue>&)>;
    Optional<GridRepeat> parse_grid_track_repeat_impl(TokenStream<ComponentValue>&, GridRepeatTypeParser const&, GridTrackParser const&);

    using GridMinMaxParamParser = AK::Function<Optional<GridSize>(TokenStream<ComponentValue>&)>;
    Optional<ExplicitGridTrack> parse_grid_minmax(TokenStream<ComponentValue>&, GridMinMaxParamParser const&, GridMinMaxParamParser const&);

    Optional<ExplicitGridTrack> parse_grid_track_size(TokenStream<ComponentValue>&);
    Optional<ExplicitGridTrack> parse_grid_fixed_size(TokenStream<ComponentValue>&);

    enum class AllowTrailingLineNamesForEachTrack {
        Yes,
        No
    };
    [[nodiscard]] size_t parse_track_list_impl(TokenStream<ComponentValue>& tokens, GridTrackSizeList& output, GridTrackParser const& track_parsing_callback, AllowTrailingLineNamesForEachTrack = AllowTrailingLineNamesForEachTrack::No);
    GridTrackSizeList parse_grid_track_list(TokenStream<ComponentValue>&);
    GridTrackSizeList parse_grid_auto_track_list(TokenStream<ComponentValue>&);
    GridTrackSizeList parse_explicit_track_list(TokenStream<ComponentValue>&);

    Optional<URL> parse_url_function(TokenStream<ComponentValue>&);
    RefPtr<URLStyleValue const> parse_url_value(TokenStream<ComponentValue>&);

    RefPtr<BorderRadiusRectStyleValue const> parse_border_radius_rect_value(TokenStream<ComponentValue>&);
    RefPtr<RadialSizeStyleValue const> parse_radial_size(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_basic_shape_value(TokenStream<ComponentValue>&);

    RefPtr<FitContentStyleValue const> parse_fit_content_value(TokenStream<ComponentValue>&);

    Optional<Vector<ColorStopListElement>> parse_color_stop_list(TokenStream<ComponentValue>& tokens, auto parse_position);
    Optional<Vector<ColorStopListElement>> parse_linear_color_stop_list(TokenStream<ComponentValue>&);
    Optional<Vector<ColorStopListElement>> parse_angular_color_stop_list(TokenStream<ComponentValue>&);
    Optional<InterpolationMethod> parse_interpolation_method(TokenStream<ComponentValue>&);

    RefPtr<LinearGradientStyleValue const> parse_linear_gradient_function(TokenStream<ComponentValue>&);
    RefPtr<ConicGradientStyleValue const> parse_conic_gradient_function(TokenStream<ComponentValue>&);
    RefPtr<RadialGradientStyleValue const> parse_radial_gradient_function(TokenStream<ComponentValue>&);

    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_css_value(PropertyID, TokenStream<ComponentValue>&, Optional<String> original_source_text = {});
    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_descriptor_value(AtRuleID, DescriptorID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_positional_value_list_shorthand(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_css_value_for_property(PropertyID, TokenStream<ComponentValue>&);
    struct PropertyAndValue {
        PropertyID property;
        RefPtr<StyleValue const> style_value;
    };
    Optional<PropertyAndValue> parse_css_value_for_properties(ReadonlySpan<PropertyID>, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_builtin_value(TokenStream<ComponentValue>&);
    Optional<FlyString> parse_custom_ident(TokenStream<ComponentValue>&, ReadonlySpan<StringView> blacklist);
    RefPtr<CustomIdentStyleValue const> parse_custom_ident_value(TokenStream<ComponentValue>&, ReadonlySpan<StringView> blacklist);
    Optional<FlyString> parse_dashed_ident(TokenStream<ComponentValue>&);
    RefPtr<CustomIdentStyleValue const> parse_dashed_ident_value(TokenStream<ComponentValue>&);
    RefPtr<RandomValueSharingStyleValue const> parse_random_value_sharing(TokenStream<ComponentValue>&);
    // NOTE: Implemented in generated code. (GenerateCSSMathFunctions.cpp)
    RefPtr<CalculationNode const> parse_math_function(Function const&, CalculationContext const&);
    RefPtr<CalculationNode const> parse_a_calc_function_node(Function const&, CalculationContext const&);
    RefPtr<StyleValue const> parse_keyword_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_hue_none_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_solidus_and_alpha_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_rgb_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_hsl_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_hwb_color_value(TokenStream<ComponentValue>&);
    Optional<Array<RefPtr<StyleValue const>, 4>> parse_lab_like_color_value(TokenStream<ComponentValue>&, StringView);
    RefPtr<StyleValue const> parse_lab_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_oklab_color_value(TokenStream<ComponentValue>&);
    Optional<Array<RefPtr<StyleValue const>, 4>> parse_lch_like_color_value(TokenStream<ComponentValue>&, StringView);
    RefPtr<StyleValue const> parse_lch_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_oklch_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_color_function(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_color_mix_function(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_light_dark_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_color_scheme_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_corner_shape_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_counter_value(TokenStream<ComponentValue>&);
    Optional<FlyString> parse_counter_style_name(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_counter_style_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_symbol_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_nonnegative_integer_symbol_pair_value(TokenStream<ComponentValue>&);
    enum class AllowReversed {
        No,
        Yes,
    };
    RefPtr<StyleValue const> parse_counter_definitions_value(TokenStream<ComponentValue>&, AllowReversed, i32 default_value_if_not_reversed);
    RefPtr<StyleValue const> parse_rect_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_ratio_value(TokenStream<ComponentValue>&);
    RefPtr<StringStyleValue const> parse_string_value(TokenStream<ComponentValue>&);
    RefPtr<AbstractImageStyleValue const> parse_image_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_paint_value(TokenStream<ComponentValue>&);
    enum class PositionParsingMode {
        Normal,
        BackgroundPosition,
    };
    RefPtr<PositionStyleValue const> parse_position_value(TokenStream<ComponentValue>&, PositionParsingMode = PositionParsingMode::Normal);
    RefPtr<StyleValue const> parse_filter_value_list_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_contain_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_container_type_value(TokenStream<ComponentValue>&);
    RefPtr<StringStyleValue const> parse_opentype_tag_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_opacity_value(TokenStream<ComponentValue>&);
    RefPtr<FontSourceStyleValue const> parse_font_source_value(TokenStream<ComponentValue>&);

    RefPtr<StyleValue const> parse_anchor(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_anchor_size(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_angle_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_angle_percentage_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_flex_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_frequency_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_frequency_percentage_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_integer_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_length_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_length_percentage_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_number_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_number_percentage_value(TokenStream<ComponentValue>& tokens);
    RefPtr<StyleValue const> parse_number_percentage_none_value(TokenStream<ComponentValue>& tokens);
    RefPtr<StyleValue const> parse_percentage_value(TokenStream<ComponentValue>& tokens);
    RefPtr<StyleValue const> parse_resolution_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_time_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_time_percentage_value(TokenStream<ComponentValue>&);

    RefPtr<StyleValue const> parse_view_timeline_inset_value(TokenStream<ComponentValue>&);
    RefPtr<ScrollFunctionStyleValue const> parse_scroll_function_value(TokenStream<ComponentValue>&);
    RefPtr<ViewFunctionStyleValue const> parse_view_function_value(TokenStream<ComponentValue>&);

    using ParseFunction = AK::Function<RefPtr<StyleValue const>(TokenStream<ComponentValue>&)>;
    RefPtr<StyleValueList const> parse_comma_separated_value_list(TokenStream<ComponentValue>&, ParseFunction);
    RefPtr<StyleValueList const> parse_simple_comma_separated_value_list(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_coordinating_value_list_shorthand(TokenStream<ComponentValue>&, PropertyID shorthand_id, Vector<PropertyID> const& longhand_ids, Vector<PropertyID> const& reset_only_longhand_ids);
    RefPtr<StyleValue const> parse_all_as_single_keyword_value(TokenStream<ComponentValue>&, Keyword);

    RefPtr<StyleValue const> parse_anchor_name_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_anchor_scope_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_aspect_ratio_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_animation_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_background_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_background_position_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_single_background_position_x_or_y_value(TokenStream<ComponentValue>&, PropertyID);
    RefPtr<StyleValue const> parse_single_background_size_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_border_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_border_image_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_border_image_slice_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_border_radius_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_border_radius_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_columns_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_content_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_counter_increment_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_counter_reset_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_counter_set_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_cursor_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_display_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_flex_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_flex_flow_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_family_name_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_family_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_language_override_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_feature_settings_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_style_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_variation_settings_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_variant(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_variant_alternates_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_variant_east_asian_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_variant_emoji(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_variant_ligatures_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_font_variant_numeric_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_list_style_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_mask_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_math_depth_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_overflow_clip_margin_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_overflow_clip_margin_shorthand(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_paint_order_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_place_content_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_place_items_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_place_self_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_position_anchor_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_position_area(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_position_area_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_single_position_try_fallbacks_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_position_try_fallbacks_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_position_visibility_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_quotes_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_single_repeat_style_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_scroll_timeline_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_scrollbar_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_scrollbar_gutter_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_shadow_value(TokenStream<ComponentValue>&, ShadowStyleValue::ShadowType);
    RefPtr<StyleValue const> parse_single_shadow_value(TokenStream<ComponentValue>&, ShadowStyleValue::ShadowType);
    RefPtr<StyleValue const> parse_shape_outside_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_text_decoration_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_text_decoration_line_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_text_indent_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_text_underline_position_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_rotate_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_stroke_dasharray_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_easing_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_timeline_scope_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_transform_function_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_transform_list_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_transform_origin_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_transition_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_transition_property_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_translate_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_try_tactic_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_scale_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_grid_track_size_list(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_grid_auto_track_sizes(TokenStream<ComponentValue>&);
    RefPtr<GridAutoFlowStyleValue const> parse_grid_auto_flow_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_grid_track_size_list_shorthand_value(PropertyID, TokenStream<ComponentValue>&, bool include_grid_auto_properties = false);
    RefPtr<GridTrackPlacementStyleValue const> parse_grid_track_placement(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_grid_track_placement_shorthand_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_grid_template_areas_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_grid_area_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_grid_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_touch_action_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_view_timeline_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_white_space_shorthand(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_white_space_trim_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue const> parse_will_change_value(TokenStream<ComponentValue>&);

    RefPtr<CalculationNode const> convert_to_calculation_node(CalcParsing::Node const&, CalculationContext const&);
    RefPtr<CalculationNode const> parse_a_calculation(TokenStream<ComponentValue>&, CalculationContext const&);

    ParseErrorOr<NonnullRefPtr<Selector>> parse_complex_selector(TokenStream<ComponentValue>&, SelectorType);
    ParseErrorOr<Optional<Selector::CompoundSelector>> parse_compound_selector(TokenStream<ComponentValue>&);
    Optional<Selector::Combinator> parse_selector_combinator(TokenStream<ComponentValue>&);
    enum class AllowWildcardName {
        No,
        Yes,
    };
    Optional<Selector::SimpleSelector::QualifiedName> parse_selector_qualified_name(TokenStream<ComponentValue>&, AllowWildcardName);
    ParseErrorOr<Selector::SimpleSelector> parse_attribute_simple_selector(ComponentValue const&);
    ParseErrorOr<Selector::SimpleSelector> parse_pseudo_simple_selector(TokenStream<ComponentValue>&);
    ParseErrorOr<Optional<Selector::SimpleSelector>> parse_simple_selector(TokenStream<ComponentValue>&);

    NonnullRefPtr<MediaQuery> parse_media_query(TokenStream<ComponentValue>&);
    OwnPtr<BooleanExpression> parse_media_condition(TokenStream<ComponentValue>&);
    OwnPtr<MediaFeature> parse_media_feature(TokenStream<ComponentValue>&);
    Optional<MediaQuery::MediaType> parse_media_type(TokenStream<ComponentValue>&);
    Optional<MediaFeatureValue> parse_media_feature_value(MediaFeatureID, TokenStream<ComponentValue>&);

    using ParseTest = AK::Function<OwnPtr<BooleanExpression>(TokenStream<ComponentValue>&)> const&;
    OwnPtr<BooleanExpression> parse_boolean_expression(TokenStream<ComponentValue>&, MatchResult result_for_general_enclosed, ParseTest parse_test);
    OwnPtr<BooleanExpression> parse_boolean_expression_group(TokenStream<ComponentValue>&, MatchResult result_for_general_enclosed, ParseTest parse_test);

    OwnPtr<BooleanExpression> parse_supports_feature(TokenStream<ComponentValue>&);
    OwnPtr<Supports::Declaration> parse_supports_declaration(TokenStream<ComponentValue>&);

    NonnullRefPtr<StyleValue const> resolve_unresolved_style_value(DOM::AbstractElement, GuardedSubstitutionContexts&, PropertyNameAndID const&, UnresolvedStyleValue const&);

    RefPtr<StyleValue const> parse_according_to_syntax_node(TokenStream<ComponentValue>& tokens, SyntaxNode const& syntax_node, Optional<DOM::AbstractElement> const& element);

    static bool has_ignored_vendor_prefix(StringView);

    void extract_property(Declaration const&, Parser::PropertiesAndCustomProperties&);

    DOM::Document const* document() const;
    HTML::Window const* window() const;
    JS::Realm& realm() const;
    bool in_quirks_mode() const;
    bool is_parsing_svg_presentation_attribute() const;

    GC::Ptr<DOM::Document const> m_document;
    GC::Ptr<JS::Realm> m_realm;
    ParsingMode m_parsing_mode { ParsingMode::Normal };

    Vector<Token> m_tokens;
    TokenStream<Token> m_token_stream;

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
    bool context_allows_quirky_length() const;
    bool context_allows_tree_counting_functions() const;
    bool context_allows_random_functions() const;
    FlyString random_value_sharing_auto_name() const;

    Vector<RuleContext> m_rule_context;
    HashTable<FlyString> m_declared_namespaces;

    Vector<PseudoClass> m_pseudo_class_context; // Stack of pseudo-class functions we're currently inside
};

}

namespace Web {

GC::Ref<CSS::CSSStyleSheet> parse_css_stylesheet(CSS::Parser::ParsingParams const&, StringView, Optional<::URL::URL> location = {}, GC::Ptr<CSS::MediaList> media_list = {});
CSS::Parser::Parser::PropertiesAndCustomProperties parse_css_property_declaration_block(CSS::Parser::ParsingParams const&, StringView);
Vector<CSS::Descriptor> parse_css_descriptor_declaration_block(CSS::Parser::ParsingParams const&, CSS::AtRuleID, StringView);
RefPtr<CSS::StyleValue const> parse_css_value(CSS::Parser::ParsingParams const&, StringView, CSS::PropertyID);
RefPtr<CSS::StyleValue const> parse_css_type(CSS::Parser::ParsingParams const&, StringView, CSS::ValueType);
RefPtr<CSS::StyleValue const> parse_css_descriptor(CSS::Parser::ParsingParams const&, CSS::AtRuleID, CSS::DescriptorID, StringView);
Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingParams const&, StringView);
Optional<CSS::SelectorList> parse_selector_for_nested_style_rule(CSS::Parser::ParsingParams const&, StringView);
Optional<CSS::PageSelectorList> parse_page_selector_list(CSS::Parser::ParsingParams const&, StringView);
Optional<CSS::Selector::PseudoElementSelector> parse_pseudo_element_selector(CSS::Parser::ParsingParams const&, StringView);
CSS::CSSRule* parse_css_rule(CSS::Parser::ParsingParams const&, StringView);
RefPtr<CSS::MediaQuery> parse_media_query(CSS::Parser::ParsingParams const&, StringView);
Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(CSS::Parser::ParsingParams const&, StringView);
RefPtr<CSS::Supports> parse_css_supports(CSS::Parser::ParsingParams const&, StringView);
Vector<CSS::Parser::ComponentValue> parse_component_values_list(CSS::Parser::ParsingParams const&, StringView);
GC::Ref<JS::Realm> internal_css_realm();
ErrorOr<String> css_decode_bytes(Optional<StringView> const& environment_encoding, Optional<String> mime_type_charset, ByteBuffer const& encoded_string);

}
