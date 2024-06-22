/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/GeneralEnclosed.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/ParsedFontFace.h>
#include <LibWeb/CSS/Parser/Block.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Declaration.h>
#include <LibWeb/CSS/Parser/DeclarationOrAtRule.h>
#include <LibWeb/CSS/Parser/Dimension.h>
#include <LibWeb/CSS/Parser/Function.h>
#include <LibWeb/CSS/Parser/ParsingContext.h>
#include <LibWeb/CSS/Parser/Rule.h>
#include <LibWeb/CSS/Parser/TokenStream.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/Ratio.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleValue.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

class PropertyDependencyNode;

class Parser {
public:
    static ErrorOr<Parser> create(ParsingContext const&, StringView input, StringView encoding = "utf-8"sv);

    Parser(Parser&&);

    CSSStyleSheet* parse_as_css_stylesheet(Optional<URL::URL> location);
    ElementInlineCSSStyleDeclaration* parse_as_style_attribute(DOM::Element&);
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

    Vector<NonnullRefPtr<MediaQuery>> parse_as_media_query_list();
    RefPtr<MediaQuery> parse_as_media_query();

    RefPtr<Supports> parse_as_supports();

    RefPtr<StyleValue> parse_as_css_value(PropertyID);

    Optional<ComponentValue> parse_as_component_value();

    Vector<ParsedFontFace::Source> parse_as_font_face_src();

    static NonnullRefPtr<StyleValue> resolve_unresolved_style_value(ParsingContext const&, DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, PropertyID, UnresolvedStyleValue const&);

    [[nodiscard]] LengthOrCalculated parse_as_sizes_attribute();

    // https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
    static constexpr Array case_insensitive_html_attributes = {
        "accept"sv,
        "accept-charset"sv,
        "align"sv,
        "alink"sv,
        "axis"sv,
        "bgcolor"sv,
        "charset"sv,
        "checked"sv,
        "clear"sv,
        "codetype"sv,
        "color"sv,
        "compact"sv,
        "declare"sv,
        "defer"sv,
        "dir"sv,
        "direction"sv,
        "disabled"sv,
        "enctype"sv,
        "face"sv,
        "frame"sv,
        "hreflang"sv,
        "http-equiv"sv,
        "lang"sv,
        "language"sv,
        "link"sv,
        "media"sv,
        "method"sv,
        "multiple"sv,
        "nohref"sv,
        "noresize"sv,
        "noshade"sv,
        "nowrap"sv,
        "readonly"sv,
        "rel"sv,
        "rev"sv,
        "rules"sv,
        "scope"sv,
        "scrolling"sv,
        "selected"sv,
        "shape"sv,
        "target"sv,
        "text"sv,
        "type"sv,
        "valign"sv,
        "valuetype"sv,
        "vlink"sv,
    };

private:
    Parser(ParsingContext const&, Vector<Token>);

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
        Vector<NonnullRefPtr<Rule>> rules;
    };
    template<typename T>
    ParsedStyleSheet parse_a_stylesheet(TokenStream<T>&, Optional<URL::URL> location);

    // "Parse a list of rules" is intended for the content of at-rules such as @media. It differs from "Parse a stylesheet" in the handling of <CDO-token> and <CDC-token>.
    template<typename T>
    Vector<NonnullRefPtr<Rule>> parse_a_list_of_rules(TokenStream<T>&);

    // "Parse a rule" is intended for use by the CSSStyleSheet#insertRule method, and similar functions which might exist, which parse text into a single rule.
    template<typename T>
    RefPtr<Rule> parse_a_rule(TokenStream<T>&);

    // "Parse a declaration" is used in @supports conditions. [CSS3-CONDITIONAL]
    template<typename T>
    Optional<Declaration> parse_a_declaration(TokenStream<T>&);

    template<typename T>
    Vector<DeclarationOrAtRule> parse_a_style_blocks_contents(TokenStream<T>&);

    // "Parse a list of declarations" is for the contents of a style attribute, which parses text into the contents of a single style rule.
    template<typename T>
    Vector<DeclarationOrAtRule> parse_a_list_of_declarations(TokenStream<T>&);

    // "Parse a component value" is for things that need to consume a single value, like the parsing rules for attr().
    template<typename T>
    Optional<ComponentValue> parse_a_component_value(TokenStream<T>&);

    // "Parse a list of component values" is for the contents of presentational attributes, which parse text into a single declaration’s value, or for parsing a stand-alone selector [SELECT] or list of Media Queries [MEDIAQ], as in Selectors API or the media HTML attribute.
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

    enum class TopLevel {
        No,
        Yes
    };
    template<typename T>
    [[nodiscard]] Vector<NonnullRefPtr<Rule>> consume_a_list_of_rules(TokenStream<T>&, TopLevel);
    template<typename T>
    [[nodiscard]] NonnullRefPtr<Rule> consume_an_at_rule(TokenStream<T>&);
    template<typename T>
    RefPtr<Rule> consume_a_qualified_rule(TokenStream<T>&);
    template<typename T>
    [[nodiscard]] Vector<DeclarationOrAtRule> consume_a_style_blocks_contents(TokenStream<T>&);
    template<typename T>
    [[nodiscard]] Vector<DeclarationOrAtRule> consume_a_list_of_declarations(TokenStream<T>&);
    template<typename T>
    Optional<Declaration> consume_a_declaration(TokenStream<T>&);
    template<typename T>
    [[nodiscard]] ComponentValue consume_a_component_value(TokenStream<T>&);
    template<typename T>
    NonnullRefPtr<Block> consume_a_simple_block(TokenStream<T>&);
    template<typename T>
    NonnullRefPtr<Function> consume_a_function(TokenStream<T>&);

    Optional<GeneralEnclosed> parse_general_enclosed(TokenStream<ComponentValue>&);

    CSSRule* parse_font_face_rule(TokenStream<ComponentValue>&);

    template<typename T>
    Vector<ParsedFontFace::Source> parse_font_face_src(TokenStream<T>&);

    CSSRule* convert_to_rule(NonnullRefPtr<Rule>);
    CSSMediaRule* convert_to_media_rule(NonnullRefPtr<Rule>);

    PropertyOwningCSSStyleDeclaration* convert_to_style_declaration(Vector<DeclarationOrAtRule> const& declarations);
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
    Optional<ResolutionOrCalculated> parse_resolution(TokenStream<ComponentValue>&);
    Optional<TimeOrCalculated> parse_time(TokenStream<ComponentValue>&);
    Optional<TimePercentage> parse_time_percentage(TokenStream<ComponentValue>&);

    Optional<Color> parse_rgb_or_hsl_color(StringView function_name, Vector<ComponentValue> const&);
    Optional<Color> parse_color(ComponentValue const&);
    Optional<LengthOrCalculated> parse_source_size_value(TokenStream<ComponentValue>&);
    Optional<Ratio> parse_ratio(TokenStream<ComponentValue>&);
    Optional<Gfx::UnicodeRange> parse_unicode_range(TokenStream<ComponentValue>&);
    Optional<Gfx::UnicodeRange> parse_unicode_range(StringView);
    Vector<Gfx::UnicodeRange> parse_unicode_ranges(TokenStream<ComponentValue>&);
    Optional<GridSize> parse_grid_size(ComponentValue const&);
    Optional<GridMinMax> parse_min_max(Vector<ComponentValue> const&);
    Optional<GridRepeat> parse_repeat(Vector<ComponentValue> const&);
    Optional<ExplicitGridTrack> parse_track_sizing_function(ComponentValue const&);

    Optional<URL::URL> parse_url_function(ComponentValue const&);
    RefPtr<StyleValue> parse_url_value(TokenStream<ComponentValue>&);

    RefPtr<StyleValue> parse_basic_shape_function(ComponentValue const&);
    RefPtr<StyleValue> parse_basic_shape_value(TokenStream<ComponentValue>&);

    template<typename TElement>
    Optional<Vector<TElement>> parse_color_stop_list(TokenStream<ComponentValue>& tokens, auto is_position, auto get_position);
    Optional<Vector<LinearColorStopListElement>> parse_linear_color_stop_list(TokenStream<ComponentValue>&);
    Optional<Vector<AngularColorStopListElement>> parse_angular_color_stop_list(TokenStream<ComponentValue>&);

    RefPtr<StyleValue> parse_linear_gradient_function(ComponentValue const&);
    RefPtr<StyleValue> parse_conic_gradient_function(ComponentValue const&);
    RefPtr<StyleValue> parse_radial_gradient_function(ComponentValue const&);

    ParseErrorOr<NonnullRefPtr<StyleValue>> parse_css_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_css_value_for_property(PropertyID, TokenStream<ComponentValue>&);
    struct PropertyAndValue {
        PropertyID property;
        RefPtr<StyleValue> style_value;
    };
    Optional<PropertyAndValue> parse_css_value_for_properties(ReadonlySpan<PropertyID>, TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_builtin_value(ComponentValue const&);
    RefPtr<CalculatedStyleValue> parse_calculated_value(ComponentValue const&);
    // NOTE: Implemented in generated code. (GenerateCSSMathFunctions.cpp)
    OwnPtr<CalculationNode> parse_math_function(PropertyID, Function const&);
    OwnPtr<CalculationNode> parse_a_calc_function_node(Function const&);
    RefPtr<StyleValue> parse_dimension_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_integer_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_number_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_number_or_percentage_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_identifier_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_color_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_rect_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_ratio_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_string_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_image_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_paint_value(TokenStream<ComponentValue>&);
    enum class PositionParsingMode {
        Normal,
        BackgroundPosition,
    };
    RefPtr<PositionStyleValue> parse_position_value(TokenStream<ComponentValue>&, PositionParsingMode = PositionParsingMode::Normal);
    RefPtr<StyleValue> parse_filter_value_list_value(TokenStream<ComponentValue>&);

    template<typename ParseFunction>
    RefPtr<StyleValue> parse_comma_separated_value_list(TokenStream<ComponentValue>&, ParseFunction);
    RefPtr<StyleValue> parse_simple_comma_separated_value_list(PropertyID, TokenStream<ComponentValue>&);

    RefPtr<StyleValue> parse_aspect_ratio_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_background_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_single_background_position_x_or_y_value(TokenStream<ComponentValue>&, PropertyID);
    RefPtr<StyleValue> parse_single_background_repeat_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_single_background_size_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_border_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_border_radius_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_border_radius_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_content_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_display_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_flex_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_flex_flow_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_font_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_font_family_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_list_style_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_math_depth_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_overflow_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_place_content_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_place_items_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_place_self_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_quotes_value(TokenStream<ComponentValue>&);
    enum class AllowInsetKeyword {
        No,
        Yes,
    };
    RefPtr<StyleValue> parse_shadow_value(TokenStream<ComponentValue>&, AllowInsetKeyword);
    RefPtr<StyleValue> parse_single_shadow_value(TokenStream<ComponentValue>&, AllowInsetKeyword);
    RefPtr<StyleValue> parse_text_decoration_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_text_decoration_line_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_easing_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_transform_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_transform_origin_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_transition_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_grid_track_size_list(TokenStream<ComponentValue>&, bool allow_separate_line_name_blocks = false);
    RefPtr<StyleValue> parse_grid_auto_track_sizes(TokenStream<ComponentValue>&);
    RefPtr<GridAutoFlowStyleValue> parse_grid_auto_flow_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_grid_track_size_list_shorthand_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_grid_track_placement(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_grid_track_placement_shorthand_value(PropertyID, TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_grid_template_areas_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_grid_area_shorthand_value(TokenStream<ComponentValue>&);
    RefPtr<StyleValue> parse_grid_shorthand_value(TokenStream<ComponentValue>&);

    OwnPtr<CalculationNode> parse_a_calculation(Vector<ComponentValue> const&);

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
    OwnPtr<MediaCondition> parse_media_condition(TokenStream<ComponentValue>&, MediaCondition::AllowOr allow_or);
    Optional<MediaFeature> parse_media_feature(TokenStream<ComponentValue>&);
    Optional<MediaQuery::MediaType> parse_media_type(TokenStream<ComponentValue>&);
    OwnPtr<MediaCondition> parse_media_in_parens(TokenStream<ComponentValue>&);
    Optional<MediaFeatureValue> parse_media_feature_value(MediaFeatureID, TokenStream<ComponentValue>&);

    OwnPtr<Supports::Condition> parse_supports_condition(TokenStream<ComponentValue>&);
    Optional<Supports::InParens> parse_supports_in_parens(TokenStream<ComponentValue>&);
    Optional<Supports::Feature> parse_supports_feature(TokenStream<ComponentValue>&);

    NonnullRefPtr<StyleValue> resolve_unresolved_style_value(DOM::Element&, Optional<Selector::PseudoElement::Type>, PropertyID, UnresolvedStyleValue const&);
    bool expand_variables(DOM::Element&, Optional<Selector::PseudoElement::Type>, FlyString const& property_name, HashMap<FlyString, NonnullRefPtr<PropertyDependencyNode>>& dependencies, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest);
    bool expand_unresolved_values(DOM::Element&, FlyString const& property_name, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest);
    bool substitute_attr_function(DOM::Element& element, FlyString const& property_name, Function const& attr_function, Vector<ComponentValue>& dest);

    static bool has_ignored_vendor_prefix(StringView);

    struct PropertiesAndCustomProperties {
        Vector<StyleProperty> properties;
        HashMap<FlyString, StyleProperty> custom_properties;
    };

    PropertiesAndCustomProperties extract_properties(Vector<DeclarationOrAtRule> const&);

    ParsingContext m_context;

    Vector<Token> m_tokens;
    TokenStream<Token> m_token_stream;
};

}

namespace Web {

CSS::CSSStyleSheet* parse_css_stylesheet(CSS::Parser::ParsingContext const&, StringView, Optional<URL::URL> location = {});
CSS::ElementInlineCSSStyleDeclaration* parse_css_style_attribute(CSS::Parser::ParsingContext const&, StringView, DOM::Element&);
RefPtr<CSS::StyleValue> parse_css_value(CSS::Parser::ParsingContext const&, StringView, CSS::PropertyID property_id = CSS::PropertyID::Invalid);
Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingContext const&, StringView);
CSS::CSSRule* parse_css_rule(CSS::Parser::ParsingContext const&, StringView);
RefPtr<CSS::MediaQuery> parse_media_query(CSS::Parser::ParsingContext const&, StringView);
Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(CSS::Parser::ParsingContext const&, StringView);
RefPtr<CSS::Supports> parse_css_supports(CSS::Parser::ParsingContext const&, StringView);
Optional<CSS::StyleProperty> parse_css_supports_condition(CSS::Parser::ParsingContext const&, StringView);

}
