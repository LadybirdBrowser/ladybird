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
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/ParsedFontFace.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Dimension.h>
#include <LibWeb/CSS/Parser/TokenStream.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>
#include <LibWeb/CSS/Parser/Types.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

class PropertyDependencyNode;

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

enum class ParsingMode {
    Normal,
    SVGPresentationAttribute, // See https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
};

struct ParsingParams {
    explicit ParsingParams(ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(JS::Realm&, ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(JS::Realm&, URL::URL, ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(DOM::Document const&, URL::URL, ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(DOM::Document const&, ParsingMode = ParsingMode::Normal);

    GC::Ptr<JS::Realm> realm;
    GC::Ptr<DOM::Document const> document;
    URL::URL url;
    ParsingMode mode { ParsingMode::Normal };
};

// The very large CSS Parser implementation code is broken up among several .cpp files:
// Parser.cpp contains the core parser algorithms, defined in https://drafts.csswg.org/css-syntax
// Everything else is in different *Parsing.cpp files
class Parser {
    AK_MAKE_NONCOPYABLE(Parser);
    AK_MAKE_NONMOVABLE(Parser);

public:
    static Parser create(ParsingParams const&, StringView input, StringView encoding = "utf-8"sv);

    CSSStyleSheet* parse_as_css_stylesheet(Optional<URL::URL> location);

    struct PropertiesAndCustomProperties {
        Vector<StyleProperty> properties;
        HashMap<FlyString, StyleProperty> custom_properties;
    };
    PropertiesAndCustomProperties parse_as_style_attribute();
    CSSRule* parse_as_css_rule();
    Optional<StyleProperty> parse_as_supports_condition();

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

    Vector<NonnullRefPtr<MediaQuery>> parse_as_media_query_list();
    RefPtr<MediaQuery> parse_as_media_query();

    RefPtr<Supports> parse_as_supports();

    RefPtr<CSSStyleValue> parse_as_css_value(PropertyID);

    Optional<ComponentValue> parse_as_component_value();

    Vector<ParsedFontFace::Source> parse_as_font_face_src();

    Vector<ComponentValue> parse_as_list_of_component_values();

    static NonnullRefPtr<CSSStyleValue> resolve_unresolved_style_value(ParsingParams const&, DOM::Element&, Optional<PseudoElement>, PropertyID, UnresolvedStyleValue const&);

    [[nodiscard]] LengthOrCalculated parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img = nullptr);

private:
    Parser(ParsingParams const&, Vector<Token>);

    enum class ParseError {
        IncludesIgnoredVendorPrefix,
        InternalError,
        SyntaxError,
    };
    template<typename T>
    using ParseErrorOr = ErrorOr<T, ParseError>;

    // "Parse a stylesheet" is intended to be the normal parser entry point, for parsing stylesheets.
    struct ParsedStyleSheet {
        Optional<URL::URL> location;
        Vector<Rule> rules;
    };
    template<typename T>
    ParsedStyleSheet parse_a_stylesheet(TokenStream<T>&, Optional<URL::URL> location);

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
    template<typename T>
    Optional<Declaration> consume_a_declaration(TokenStream<T>&, Nested = Nested::No);
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

    template<typename T>
    Vector<ParsedFontFace::Source> parse_font_face_src(TokenStream<T>&);

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
    GC::Ptr<CSSFontFaceRule> convert_to_font_face_rule(AtRule const&);
    GC::Ptr<CSSKeyframesRule> convert_to_keyframes_rule(AtRule const&);
    GC::Ptr<CSSImportRule> convert_to_import_rule(AtRule const&);
    GC::Ptr<CSSRule> convert_to_layer_rule(AtRule const&, Nested);
    GC::Ptr<CSSMediaRule> convert_to_media_rule(AtRule const&, Nested);
    GC::Ptr<CSSNamespaceRule> convert_to_namespace_rule(AtRule const&);
    GC::Ptr<CSSSupportsRule> convert_to_supports_rule(AtRule const&, Nested);
    GC::Ptr<CSSPropertyRule> convert_to_property_rule(AtRule const& rule);

    GC::Ref<CSSStyleProperties> convert_to_style_declaration(Vector<Declaration> const&);
    Optional<StyleProperty> convert_to_style_property(Declaration const&);

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

    Optional<LengthOrCalculated> parse_source_size_value(TokenStream<ComponentValue>&);
    Optional<Ratio> parse_ratio(TokenStream<ComponentValue>&);
    Optional<Gfx::UnicodeRange> parse_unicode_range(TokenStream<ComponentValue>&);
    Optional<Gfx::UnicodeRange> parse_unicode_range(StringView);
    Vector<Gfx::UnicodeRange> parse_unicode_ranges(TokenStream<ComponentValue>&);
    RefPtr<UnicodeRangeStyleValue> parse_unicode_range_value(TokenStream<ComponentValue>&);
    Optional<GridSize> parse_grid_size(ComponentValue const&);
    Optional<GridFitContent> parse_grid_fit_content(Vector<ComponentValue> const&);
    Optional<GridMinMax> parse_min_max(Vector<ComponentValue> const&);
    Optional<GridRepeat> parse_repeat(Vector<ComponentValue> const&);
    Optional<ExplicitGridTrack> parse_track_sizing_function(ComponentValue const&);

    Optional<URL::URL> parse_url_function(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_url_value(TokenStream<ComponentValue>&);

    Optional<ShapeRadius> parse_shape_radius(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_basic_shape_value(TokenStream<ComponentValue>&);

    RefPtr<FitContentStyleValue> parse_fit_content_value(TokenStream<ComponentValue>&);

    template<typename TElement>
    Optional<Vector<TElement>> parse_color_stop_list(TokenStream<ComponentValue>& tokens, auto parse_position);
    Optional<Vector<LinearColorStopListElement>> parse_linear_color_stop_list(TokenStream<ComponentValue>&);
    Optional<Vector<AngularColorStopListElement>> parse_angular_color_stop_list(TokenStream<ComponentValue>&);
    Optional<InterpolationMethod> parse_interpolation_method(TokenStream<ComponentValue>&);

    RefPtr<LinearGradientStyleValue> parse_linear_gradient_function(TokenStream<ComponentValue>&);
    RefPtr<ConicGradientStyleValue> parse_conic_gradient_function(TokenStream<ComponentValue>&);
    RefPtr<RadialGradientStyleValue> parse_radial_gradient_function(TokenStream<ComponentValue>&);

    ParseErrorOr<NonnullRefPtr<CSSStyleValue>> parse_css_value(PropertyID, TokenStream<ComponentValue>&, Optional<String> original_source_text = {});
    RefPtr<CSSStyleValue> parse_css_value_for_property(PropertyID, TokenStream<ComponentValue>&);
    struct PropertyAndValue {
        PropertyID property;
        RefPtr<CSSStyleValue> style_value;
    };
    Optional<PropertyAndValue> parse_css_value_for_properties(ReadonlySpan<PropertyID>, TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_builtin_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_calculated_value(ComponentValue const&);
    Optional<FlyString> parse_custom_ident(TokenStream<ComponentValue>&, ReadonlySpan<StringView> blacklist);
    RefPtr<CustomIdentStyleValue> parse_custom_ident_value(TokenStream<ComponentValue>&, ReadonlySpan<StringView> blacklist);
    // NOTE: Implemented in generated code. (GenerateCSSMathFunctions.cpp)
    RefPtr<CalculationNode> parse_math_function(Function const&, CalculationContext const&);
    RefPtr<CalculationNode> parse_a_calc_function_node(Function const&, CalculationContext const&);
    RefPtr<CSSStyleValue> parse_keyword_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_hue_none_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_solidus_and_alpha_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_rgb_color_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_hsl_color_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_hwb_color_value(TokenStream<ComponentValue>&);
    Optional<Array<RefPtr<CSSStyleValue>, 4>> parse_lab_like_color_value(TokenStream<ComponentValue>&, StringView);
    RefPtr<CSSStyleValue> parse_lab_color_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_oklab_color_value(TokenStream<ComponentValue>&);
    Optional<Array<RefPtr<CSSStyleValue>, 4>> parse_lch_like_color_value(TokenStream<ComponentValue>&, StringView);
    RefPtr<CSSStyleValue> parse_lch_color_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_oklch_color_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_color_function(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_light_dark_color_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_color_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_color_scheme_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_counter_value(TokenStream<ComponentValue>&);
    enum class AllowReversed {
        No,
        Yes,
    };
    RefPtr<CSSStyleValue> parse_counter_definitions_value(TokenStream<ComponentValue>&, AllowReversed, i32 default_value_if_not_reversed);
    RefPtr<CSSStyleValue> parse_rect_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_ratio_value(TokenStream<ComponentValue>&);
    RefPtr<StringStyleValue> parse_string_value(TokenStream<ComponentValue>&);
    RefPtr<AbstractImageStyleValue> parse_image_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_paint_value(TokenStream<ComponentValue>&);
    enum class PositionParsingMode {
        Normal,
        BackgroundPosition,
    };
    RefPtr<PositionStyleValue> parse_position_value(TokenStream<ComponentValue>&, PositionParsingMode = PositionParsingMode::Normal);
    RefPtr<CSSStyleValue> parse_filter_value_list_value(TokenStream<ComponentValue>&);
    RefPtr<StringStyleValue> parse_opentype_tag_value(TokenStream<ComponentValue>&);

    RefPtr<CSSStyleValue> parse_angle_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_angle_percentage_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_flex_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_frequency_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_frequency_percentage_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_integer_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_length_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_length_percentage_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_number_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_number_percentage_value(TokenStream<ComponentValue>& tokens);
    RefPtr<CSSStyleValue> parse_number_percentage_none_value(TokenStream<ComponentValue>& tokens);
    RefPtr<CSSStyleValue> parse_percentage_value(TokenStream<ComponentValue>& tokens);
    RefPtr<CSSStyleValue> parse_resolution_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_time_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_time_percentage_value(TokenStream<ComponentValue>&);

    template<typename ParseFunction>
    RefPtr<CSSStyleValue> parse_comma_separated_value_list(TokenStream<ComponentValue>&, ParseFunction);
    RefPtr<CSSStyleValue> parse_simple_comma_separated_value_list(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_all_as_single_keyword_value(TokenStream<ComponentValue>&, Keyword);

    RefPtr<CSSStyleValue> parse_aspect_ratio_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_background_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_single_background_position_x_or_y_value(TokenStream<ComponentValue>&, PropertyID);
    RefPtr<CSSStyleValue> parse_single_background_repeat_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_single_background_size_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_border_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_border_radius_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_border_radius_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_columns_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_content_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_counter_increment_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_counter_reset_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_counter_set_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_cursor_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_display_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_flex_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_flex_flow_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_family_name_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_family_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_language_override_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_feature_settings_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_variation_settings_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_variant(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_variant_alternates_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_variant_east_asian_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_variant_emoji(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_variant_ligatures_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_font_variant_numeric_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_list_style_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_math_depth_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_overflow_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_place_content_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_place_items_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_place_self_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_quotes_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_scrollbar_gutter_value(TokenStream<ComponentValue>&);
    enum class AllowInsetKeyword {
        No,
        Yes,
    };
    RefPtr<CSSStyleValue> parse_shadow_value(TokenStream<ComponentValue>&, AllowInsetKeyword);
    RefPtr<CSSStyleValue> parse_single_shadow_value(TokenStream<ComponentValue>&, AllowInsetKeyword);
    RefPtr<CSSStyleValue> parse_text_decoration_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_text_decoration_line_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_rotate_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_stroke_dasharray_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_easing_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_transform_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_transform_origin_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_transition_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_translate_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_scale_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_grid_track_size_list(TokenStream<ComponentValue>&, bool allow_separate_line_name_blocks = false);
    RefPtr<CSSStyleValue> parse_grid_auto_track_sizes(TokenStream<ComponentValue>&);
    RefPtr<GridAutoFlowStyleValue> parse_grid_auto_flow_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_grid_track_size_list_shorthand_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<GridTrackPlacementStyleValue> parse_grid_track_placement(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_grid_track_placement_shorthand_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_grid_template_areas_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_grid_area_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<CSSStyleValue> parse_grid_shorthand_value(TokenStream<ComponentValue>&);

    RefPtr<CalculationNode> convert_to_calculation_node(CalcParsing::Node const&, CalculationContext const&);
    RefPtr<CalculationNode> parse_a_calculation(Vector<ComponentValue> const&, CalculationContext const&);

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

    NonnullRefPtr<CSSStyleValue> resolve_unresolved_style_value(DOM::Element&, Optional<PseudoElement>, PropertyID, UnresolvedStyleValue const&);
    bool expand_variables(DOM::Element&, Optional<PseudoElement>, FlyString const& property_name, HashMap<FlyString, NonnullRefPtr<PropertyDependencyNode>>& dependencies, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest);
    bool expand_unresolved_values(DOM::Element&, FlyString const& property_name, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest);
    bool substitute_attr_function(DOM::Element& element, FlyString const& property_name, Function const& attr_function, Vector<ComponentValue>& dest);

    static bool has_ignored_vendor_prefix(StringView);

    PropertiesAndCustomProperties extract_properties(Vector<RuleOrListOfDeclarations> const&);
    void extract_property(Declaration const&, Parser::PropertiesAndCustomProperties&);

    DOM::Document const* document() const;
    HTML::Window const* window() const;
    JS::Realm& realm() const;
    bool in_quirks_mode() const;
    bool is_parsing_svg_presentation_attribute() const;
    Optional<URL::URL> complete_url(StringView) const;

    GC::Ptr<DOM::Document const> m_document;
    GC::Ptr<JS::Realm> m_realm;
    URL::URL m_url;
    ParsingMode m_parsing_mode { ParsingMode::Normal };

    Vector<Token> m_tokens;
    TokenStream<Token> m_token_stream;

    struct FunctionContext {
        StringView name;
    };
    using ValueParsingContext = Variant<PropertyID, FunctionContext>;
    Vector<ValueParsingContext> m_value_context;
    auto push_temporary_value_parsing_context(ValueParsingContext&& context)
    {
        m_value_context.append(context);
        return ScopeGuard { [&] { m_value_context.take_last(); } };
    }
    bool context_allows_quirky_length() const;

    enum class ContextType {
        Unknown,
        Style,
        AtMedia,
        AtFontFace,
        AtKeyframes,
        Keyframe,
        AtSupports,
        SupportsCondition,
        AtLayer,
        AtProperty,
    };
    static ContextType context_type_for_at_rule(FlyString const&);
    Vector<ContextType> m_rule_context;

    Vector<PseudoClass> m_pseudo_class_context; // Stack of pseudo-class functions we're currently inside
};

}

namespace Web {

CSS::CSSStyleSheet* parse_css_stylesheet(CSS::Parser::ParsingParams const&, StringView, Optional<URL::URL> location = {});
CSS::Parser::Parser::PropertiesAndCustomProperties parse_css_style_attribute(CSS::Parser::ParsingParams const&, StringView);
RefPtr<CSS::CSSStyleValue> parse_css_value(CSS::Parser::ParsingParams const&, StringView, CSS::PropertyID property_id = CSS::PropertyID::Invalid);
Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingParams const&, StringView);
Optional<CSS::SelectorList> parse_selector_for_nested_style_rule(CSS::Parser::ParsingParams const&, StringView);
Optional<CSS::Selector::PseudoElementSelector> parse_pseudo_element_selector(CSS::Parser::ParsingParams const&, StringView);
CSS::CSSRule* parse_css_rule(CSS::Parser::ParsingParams const&, StringView);
RefPtr<CSS::MediaQuery> parse_media_query(CSS::Parser::ParsingParams const&, StringView);
Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(CSS::Parser::ParsingParams const&, StringView);
RefPtr<CSS::Supports> parse_css_supports(CSS::Parser::ParsingParams const&, StringView);
GC::Ref<JS::Realm> internal_css_realm();

}
