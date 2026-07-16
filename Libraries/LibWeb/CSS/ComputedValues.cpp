/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OverflowClipMarginStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

NonnullRefPtr<ComputedValues const> ComputedValues::create(ComputedProperties const& computed_style, DOM::Document const& document, StyleScope const& style_scope, ColorResolutionContext color_resolution_context)
{
    Builder builder;
    auto& computed_values = *builder.operator->();

    auto custom_ident_list = [&](PropertyID property_id) {
        Vector<Utf16FlyString> names;
        auto append_name = [&](StyleValue const& value) {
            if (value.is_custom_ident())
                names.append(value.as_custom_ident().custom_ident());
        };
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append_name(item);
        } else {
            append_name(value);
        }
        return names;
    };
    auto for_each_comma_separated_value = [&](CSS::PropertyID property_id, auto callback) {
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list() && value.as_value_list().separator() == CSS::StyleValueList::Separator::Comma) {
            for (auto const& item : value.as_value_list().values())
                callback(item);
        } else {
            callback(value);
        }
    };

    // NOTE: color-scheme must be set first to ensure system colors can be resolved correctly.
    auto const& color_scheme_style_value = computed_style.property(PropertyID::ColorScheme).as_color_scheme();
    computed_values.set_color_schemes(color_scheme_style_value.schemes(), color_scheme_style_value.only());
    auto color_scheme = computed_style.color_scheme(document.page().preferred_color_scheme(), document.supported_color_schemes());
    computed_values.set_color_scheme(color_scheme);
    color_resolution_context.color_scheme = color_scheme;

    computed_values.set_anchor_names(custom_ident_list(PropertyID::AnchorName));
    auto const& anchor_scope = computed_style.property(PropertyID::AnchorScope);
    computed_values.set_anchor_scope(AnchorScopeData {
        .all = anchor_scope.to_keyword() == Keyword::All,
        .names = custom_ident_list(PropertyID::AnchorScope),
    });

    // NOTE: We have to be careful that font-related properties get set in the right order.
    //       m_font is used by Length::to_px() when resolving sizes against this layout node.
    //       That's why it has to be set before everything else.
    computed_values.set_font_list(computed_style.computed_font_list(document.font_computer()));
    Vector<ComputedFontFamily> font_families;
    for (auto const& family : computed_style.property(PropertyID::FontFamily).as_value_list().values()) {
        if (family->is_keyword()) {
            font_families.append(keyword_to_generic_font_family(family->to_keyword()).value());
        } else {
            font_families.append(ComputedFontFamilyName {
                .name = string_from_style_value(family),
                .syntax = family->is_string() ? ComputedFontFamilySyntax::String : ComputedFontFamilySyntax::CustomIdent,
            });
        }
    }
    computed_values.set_font_families(move(font_families));
    computed_values.set_font_size(computed_style.font_size());
    computed_values.set_font_weight(computed_style.font_weight());
    computed_values.set_font_width(computed_style.font_width());
    auto const& font_style = computed_style.property(PropertyID::FontStyle).as_font_style();
    Optional<Variant<Angle, NonnullRefPtr<CalculatedStyleValue const>>> font_style_angle;
    if (font_style.angle()) {
        if (font_style.angle()->is_angle())
            font_style_angle = font_style.angle()->as_angle().angle();
        else
            font_style_angle = NonnullRefPtr { font_style.angle()->as_calculated() };
    }
    computed_values.set_font_style({ font_style.font_style(), move(font_style_angle) });
    computed_values.set_font_optical_sizing(computed_style.font_optical_sizing());
    computed_values.set_font_feature_data(computed_style.font_feature_data());
    computed_values.set_line_height(computed_style.line_height_data(document.font_computer()));
    computed_values.set_font_variant_emoji(computed_style.font_variant_emoji());

    Vector<ComputedAnimationName> animation_names;
    for (auto const& name : computed_style.property(PropertyID::AnimationName).as_value_list().values()) {
        if (name->to_keyword() == Keyword::None) {
            animation_names.empend();
        } else {
            animation_names.append(ComputedAnimationName {
                .name = string_from_style_value(name),
                .syntax = name->is_string() ? ComputedAnimationNameSyntax::String : ComputedAnimationNameSyntax::CustomIdent,
            });
        }
    }
    computed_values.set_animation_names(move(animation_names));
    Vector<AnimationComposition> animation_compositions;
    for_each_comma_separated_value(PropertyID::AnimationComposition, [&](StyleValue const& value) { animation_compositions.append(keyword_to_animation_composition(value.to_keyword()).release_value()); });
    computed_values.set_animation_compositions(move(animation_compositions));
    Vector<Time> animation_delays;
    for_each_comma_separated_value(PropertyID::AnimationDelay, [&](StyleValue const& value) { animation_delays.append(Time::from_style_value(NonnullRefPtr<StyleValue const> { value }, {})); });
    computed_values.set_animation_delays(move(animation_delays));
    Vector<AnimationDirection> animation_directions;
    for_each_comma_separated_value(PropertyID::AnimationDirection, [&](StyleValue const& value) { animation_directions.append(keyword_to_animation_direction(value.to_keyword()).release_value()); });
    computed_values.set_animation_directions(move(animation_directions));
    Vector<Optional<Time>> animation_durations;
    for_each_comma_separated_value(PropertyID::AnimationDuration, [&](StyleValue const& value) {
        animation_durations.append(value.to_keyword() == Keyword::Auto ? Optional<Time> {} : Time::from_style_value(NonnullRefPtr<StyleValue const> { value }, {}));
    });
    computed_values.set_animation_durations(move(animation_durations));
    Vector<AnimationFillMode> animation_fill_modes;
    for_each_comma_separated_value(PropertyID::AnimationFillMode, [&](StyleValue const& value) { animation_fill_modes.append(keyword_to_animation_fill_mode(value.to_keyword()).release_value()); });
    computed_values.set_animation_fill_modes(move(animation_fill_modes));
    Vector<double> animation_iteration_counts;
    for_each_comma_separated_value(PropertyID::AnimationIterationCount, [&](StyleValue const& value) {
        animation_iteration_counts.append(value.to_keyword() == Keyword::Infinite ? AK::Infinity<double> : number_from_style_value(value, {}));
    });
    computed_values.set_animation_iteration_counts(move(animation_iteration_counts));
    Vector<AnimationPlayState> animation_play_states;
    for_each_comma_separated_value(PropertyID::AnimationPlayState, [&](StyleValue const& value) { animation_play_states.append(keyword_to_animation_play_state(value.to_keyword()).release_value()); });
    computed_values.set_animation_play_states(move(animation_play_states));
    Vector<AnimationTimelineData> animation_timelines;
    for_each_comma_separated_value(PropertyID::AnimationTimeline, [&](StyleValue const& value) {
        AnimationTimelineData timeline;
        if (value.to_keyword() == Keyword::Auto) {
            timeline.type = AnimationTimelineData::Type::Auto;
        } else if (value.to_keyword() == Keyword::None) {
            timeline.type = AnimationTimelineData::Type::None;
        } else if (value.is_custom_ident()) {
            timeline.type = AnimationTimelineData::Type::Name;
            timeline.name = value.as_custom_ident().custom_ident();
        } else {
            VERIFY(value.is_function());
            auto const& function = value.as_function();
            auto const& arguments = function.value()->as_tuple().tuple();
            if (function.name() == "scroll"_utf16_fly_string) {
                timeline.type = AnimationTimelineData::Type::Scroll;
                if (arguments[TupleStyleValue::Indices::ScrollFunction::Scroller])
                    timeline.scroller = keyword_to_scroller(arguments[TupleStyleValue::Indices::ScrollFunction::Scroller]->to_keyword()).release_value();
                if (arguments[TupleStyleValue::Indices::ScrollFunction::Axis])
                    timeline.axis = keyword_to_axis(arguments[TupleStyleValue::Indices::ScrollFunction::Axis]->to_keyword()).release_value();
            } else {
                VERIFY(function.name() == "view"_utf16_fly_string);
                timeline.type = AnimationTimelineData::Type::View;
                if (arguments[TupleStyleValue::Indices::ViewFunction::Axis])
                    timeline.axis = keyword_to_axis(arguments[TupleStyleValue::Indices::ViewFunction::Axis]->to_keyword()).release_value();
                if (auto const& inset = arguments[TupleStyleValue::Indices::ViewFunction::Inset]) {
                    VERIFY(inset->is_value_list());
                    auto const& values = inset->as_value_list().values();
                    VERIFY(values.size() == 2);
                    timeline.inset = {
                        .start = LengthPercentageOrAuto::from_style_value(values[0]),
                        .end = LengthPercentageOrAuto::from_style_value(values[1]),
                    };
                }
            }
        }
        animation_timelines.append(move(timeline));
    });
    computed_values.set_animation_timelines(move(animation_timelines));
    Vector<EasingFunction> animation_timing_functions;
    StyleValueVector animation_timing_function_style_values;
    for_each_comma_separated_value(PropertyID::AnimationTimingFunction, [&](StyleValue const& value) {
        animation_timing_functions.append(EasingFunction::from_style_value(value));
        animation_timing_function_style_values.append(value);
    });
    computed_values.set_animation_timing_functions(move(animation_timing_functions));
    computed_values.set_animation_timing_function_style_values(move(animation_timing_function_style_values));

    // NOTE: color must be set after color-scheme to ensure currentColor can be resolved in other properties (e.g. background-color).
    // NOTE: color must be set after font_size as `CalculatedStyleValue`s can rely on it being set for resolving lengths.
    auto color = computed_style.color(CSS::PropertyID::Color, color_resolution_context);
    computed_values.set_color(color);
    computed_values.set_color_style_value(&computed_style.property(CSS::PropertyID::Color));

    // NOTE: This color resolution context must be created after we set color above so that currentColor resolves correctly
    // FIXME: We should resolve colors to their absolute forms at compute time (i.e. by implementing the relevant absolutized methods)
    color_resolution_context.current_color = color;

    auto const& accent_color_value = computed_style.property(CSS::PropertyID::AccentColor);
    CSS::AccentColor accent_color;
    accent_color.used_value = computed_style.accent_color(color_resolution_context);
    if (accent_color_value.to_keyword() != CSS::Keyword::Auto)
        accent_color.computed_value = accent_color.used_value;
    computed_values.set_accent_color(move(accent_color));

    computed_values.set_vertical_align(computed_style.vertical_align());

    auto background_layers = computed_style.background_layers();
    computed_values.set_background_layers(move(background_layers));

    auto mask_layers = computed_style.mask_layers();
    computed_values.set_mask_layers(move(mask_layers));
    Vector<CSS::Position> mask_positions;
    for_each_comma_separated_value(CSS::PropertyID::MaskPosition, [&](CSS::StyleValue const& value) {
        auto const& position = value.as_position();
        mask_positions.append({
            .offset_x = CSS::LengthPercentage::from_style_value(position.edge_x()->as_edge().offset()),
            .offset_y = CSS::LengthPercentage::from_style_value(position.edge_y()->as_edge().offset()),
        });
    });
    computed_values.set_mask_positions(move(mask_positions));

    auto border_image = computed_style.border_image();
    computed_values.set_border_image(move(border_image));

    auto const& content = computed_style.property(CSS::PropertyID::Content);
    ComputedContentData computed_content;
    if (content.to_keyword() == Keyword::None) {
        computed_content.type = ComputedContentData::Type::None;
    } else if (content.is_content()) {
        computed_content.type = ComputedContentData::Type::List;
        auto append_item = [](StyleValue const& item, Vector<ComputedContentItem>& items) {
            if (item.is_string()) {
                items.append(item.as_string().string_value().to_utf16_string());
            } else if (item.is_keyword()) {
                items.append(item.to_keyword());
            } else if (item.is_counter()) {
                auto const& counter = item.as_counter();
                ComputedContentCounter computed_counter {
                    .function = counter.function_type() == CounterStyleValue::CounterFunction::Counters ? ComputedContentCounter::Function::Counters : ComputedContentCounter::Function::Counter,
                    .name = counter.counter_name(),
                    .join_string = counter.join_string(),
                    .style = counter.counter_style()->as_counter_style().value().visit(
                        [](Utf16FlyString const& name) -> Variant<Utf16FlyString, ComputedContentCounter::SymbolsFunction> { return name; },
                        [](CounterStyleStyleValue::SymbolsFunction const& symbols) -> Variant<Utf16FlyString, ComputedContentCounter::SymbolsFunction> {
                            return ComputedContentCounter::SymbolsFunction { .type = symbols.type, .symbols = symbols.symbols };
                        }),
                };
                items.append(move(computed_counter));
            } else {
                VERIFY(item.is_abstract_image());
                items.append(NonnullRefPtr<AbstractImageStyleValue const> { item.as_abstract_image() });
            }
        };
        auto const& content_style_value = content.as_content();
        for (auto const& item : content_style_value.content().values())
            append_item(item, computed_content.items);
        if (auto const* alt_text = content_style_value.alt_text()) {
            for (auto const& item : alt_text->values())
                append_item(item, computed_content.alt_text);
        }
    }
    computed_values.set_computed_content(move(computed_content));

    computed_values.set_background_color(computed_style.color(CSS::PropertyID::BackgroundColor, color_resolution_context));
    computed_values.set_background_color_style_value(computed_style.property(CSS::PropertyID::BackgroundColor));
    computed_values.set_background_color_clip(computed_style.background_color_clip());

    computed_values.set_box_sizing(computed_style.box_sizing());

    if (auto maybe_font_language_override = computed_style.font_language_override(); maybe_font_language_override.has_value())
        computed_values.set_font_language_override(maybe_font_language_override.release_value());
    computed_values.set_font_variation_settings(computed_style.font_variation_settings());

    auto border_radius_data_from_style_value = [](CSS::StyleValue const& value) -> CSS::BorderRadiusData {
        return CSS::BorderRadiusData {
            CSS::LengthPercentage::from_style_value(value.as_border_radius().horizontal_radius()),
            CSS::LengthPercentage::from_style_value(value.as_border_radius().vertical_radius())
        };
    };

    computed_values.set_border_bottom_left_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderBottomLeftRadius)));
    computed_values.set_border_bottom_right_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderBottomRightRadius)));
    computed_values.set_border_top_left_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderTopLeftRadius)));
    computed_values.set_border_top_right_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderTopRightRadius)));
    computed_values.set_corner_bottom_left_shape(computed_style.property(CSS::PropertyID::CornerBottomLeftShape).as_superellipse().parameter());
    computed_values.set_corner_bottom_right_shape(computed_style.property(CSS::PropertyID::CornerBottomRightShape).as_superellipse().parameter());
    computed_values.set_corner_top_left_shape(computed_style.property(CSS::PropertyID::CornerTopLeftShape).as_superellipse().parameter());
    computed_values.set_corner_top_right_shape(computed_style.property(CSS::PropertyID::CornerTopRightShape).as_superellipse().parameter());
    computed_values.set_display(computed_style.display());
    computed_values.set_display_before_box_type_transformation(computed_style.display_before_box_type_transformation());

    computed_values.set_flex_direction(computed_style.flex_direction());
    computed_values.set_flex_wrap(computed_style.flex_wrap());
    computed_values.set_flex_basis(computed_style.flex_basis());
    computed_values.set_flex_grow(computed_style.flex_grow());
    computed_values.set_flex_shrink(computed_style.flex_shrink());
    computed_values.set_order(computed_style.order());
    computed_values.set_clip(computed_style.clip());

    computed_values.set_backdrop_filter(computed_style.backdrop_filter());
    computed_values.set_filter(computed_style.filter());

    computed_values.set_flood_color(computed_style.color(CSS::PropertyID::FloodColor, color_resolution_context));
    computed_values.set_flood_opacity(computed_style.flood_opacity());

    computed_values.set_justify_content(computed_style.justify_content());
    computed_values.set_justify_items(computed_style.justify_items());
    computed_values.set_justify_self(computed_style.justify_self());

    computed_values.set_align_content(computed_style.align_content());
    computed_values.set_align_items(computed_style.align_items());
    computed_values.set_align_self(computed_style.align_self());

    computed_values.set_appearance(computed_style.appearance());
    computed_values.set_computed_appearance(keyword_to_appearance(computed_style.property(PropertyID::Appearance).to_keyword()).release_value());

    computed_values.set_position(computed_style.position());

    // https://drafts.csswg.org/css-anchor-position-1/#position-anchor
    auto const& position_anchor_value = computed_style.property(CSS::PropertyID::PositionAnchor);
    CSS::PositionAnchor position_anchor;
    if (position_anchor_value.is_custom_ident()) {
        position_anchor.type = CSS::PositionAnchor::Type::Name;
        position_anchor.name = position_anchor_value.as_custom_ident().custom_ident();
    } else {
        switch (position_anchor_value.to_keyword()) {
        case CSS::Keyword::Normal:
            position_anchor.type = CSS::PositionAnchor::Type::Normal;
            break;
        case CSS::Keyword::None:
            position_anchor.type = CSS::PositionAnchor::Type::None;
            break;
        case CSS::Keyword::Auto:
            position_anchor.type = CSS::PositionAnchor::Type::Auto;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    computed_values.set_position_anchor(move(position_anchor));

    auto position_area_from_style_value = [](CSS::StyleValue const& value) {
        CSS::PositionAreaData area;
        auto append_keyword = [&](CSS::StyleValue const& item) {
            if (auto keyword = CSS::keyword_to_position_area(item.to_keyword()); keyword.has_value())
                area.keywords.append(*keyword);
        };
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append_keyword(item);
        } else {
            append_keyword(value);
        }
        return area;
    };
    computed_values.set_position_area(position_area_from_style_value(computed_style.property(CSS::PropertyID::PositionArea)));

    Vector<CSS::PositionTryFallbackData> position_try_fallbacks;
    auto append_position_try_fallback = [&](CSS::StyleValue const& value) {
        CSS::PositionTryFallbackData fallback;
        auto apply_item = [&](CSS::StyleValue const& item) {
            if (item.is_custom_ident()) {
                fallback.name = item.as_custom_ident().custom_ident();
            } else if (auto tactic = CSS::keyword_to_try_tactic(item.to_keyword()); tactic.has_value()) {
                fallback.tactics.append(*tactic);
            }
        };
        auto area = position_area_from_style_value(value);
        if (!area.keywords.is_empty()) {
            fallback.position_area = move(area);
        } else if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values()) {
                if (item->is_value_list()) {
                    for (auto const& child : item->as_value_list().values())
                        apply_item(child);
                } else {
                    apply_item(item);
                }
            }
        } else {
            apply_item(value);
        }
        position_try_fallbacks.append(move(fallback));
    };
    auto const& position_try_fallbacks_value = computed_style.property(CSS::PropertyID::PositionTryFallbacks);
    if (position_try_fallbacks_value.to_keyword() != CSS::Keyword::None) {
        if (position_try_fallbacks_value.is_value_list() && position_try_fallbacks_value.as_value_list().separator() == CSS::StyleValueList::Separator::Comma) {
            for (auto const& item : position_try_fallbacks_value.as_value_list().values())
                append_position_try_fallback(item);
        } else {
            append_position_try_fallback(position_try_fallbacks_value);
        }
    }
    computed_values.set_position_try_fallbacks(move(position_try_fallbacks));

    auto const& position_try_order = computed_style.property(CSS::PropertyID::PositionTryOrder);
    computed_values.set_position_try_order(position_try_order.to_keyword() == CSS::Keyword::Normal
            ? Optional<CSS::TryOrder> {}
            : CSS::keyword_to_try_order(position_try_order.to_keyword()));

    auto const& position_visibility = computed_style.property(CSS::PropertyID::PositionVisibility);
    CSS::PositionVisibilityData position_visibility_data {
        .always = position_visibility.to_keyword() == CSS::Keyword::Always,
        .anchors_visible = false,
    };
    auto apply_position_visibility_keyword = [&](CSS::Keyword keyword) {
        switch (keyword) {
        case CSS::Keyword::AnchorsValid:
            position_visibility_data.anchors_valid = true;
            break;
        case CSS::Keyword::AnchorsVisible:
            position_visibility_data.anchors_visible = true;
            break;
        case CSS::Keyword::NoOverflow:
            position_visibility_data.no_overflow = true;
            break;
        default:
            break;
        }
    };
    if (position_visibility.is_value_list()) {
        for (auto const& item : position_visibility.as_value_list().values())
            apply_position_visibility_keyword(item->to_keyword());
    } else {
        apply_position_visibility_keyword(position_visibility.to_keyword());
    }
    computed_values.set_position_visibility(position_visibility_data);

    auto timeline_names = [&](CSS::PropertyID property_id) {
        Vector<Optional<Utf16FlyString>> names;
        auto append = [&](CSS::StyleValue const& item) {
            names.append(item.is_custom_ident() ? Optional<Utf16FlyString> { item.as_custom_ident().custom_ident() } : Optional<Utf16FlyString> {});
        };
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append(item);
        } else {
            append(value);
        }
        return names;
    };
    auto timeline_axes = [&](CSS::PropertyID property_id) {
        Vector<CSS::Axis> axes;
        auto append = [&](CSS::StyleValue const& item) { axes.append(CSS::keyword_to_axis(item.to_keyword()).release_value()); };
        auto const& value = computed_style.property(property_id);
        if (value.is_value_list()) {
            for (auto const& item : value.as_value_list().values())
                append(item);
        } else {
            append(value);
        }
        return axes;
    };
    computed_values.set_scroll_timeline_names(timeline_names(CSS::PropertyID::ScrollTimelineName));
    computed_values.set_scroll_timeline_axes(timeline_axes(CSS::PropertyID::ScrollTimelineAxis));
    computed_values.set_view_timeline_names(timeline_names(CSS::PropertyID::ViewTimelineName));
    computed_values.set_view_timeline_axes(timeline_axes(CSS::PropertyID::ViewTimelineAxis));

    auto const& timeline_scope = computed_style.property(CSS::PropertyID::TimelineScope);
    computed_values.set_timeline_scope(CSS::TimelineScopeData {
        .all = timeline_scope.to_keyword() == CSS::Keyword::All,
        .names = custom_ident_list(CSS::PropertyID::TimelineScope),
    });

    Vector<CSS::ViewTimelineInsetData> view_timeline_insets;
    auto append_view_timeline_inset = [&](CSS::StyleValue const& value) {
        VERIFY(value.is_value_list());
        auto const& values = value.as_value_list().values();
        VERIFY(values.size() == 2);
        view_timeline_insets.append({
            .start = CSS::LengthPercentageOrAuto::from_style_value(values[0]),
            .end = CSS::LengthPercentageOrAuto::from_style_value(values[1]),
        });
    };
    auto const& view_timeline_inset = computed_style.property(CSS::PropertyID::ViewTimelineInset);
    if (view_timeline_inset.as_value_list().separator() == CSS::StyleValueList::Separator::Comma) {
        for (auto const& item : view_timeline_inset.as_value_list().values())
            append_view_timeline_inset(item);
    } else {
        append_view_timeline_inset(view_timeline_inset);
    }
    computed_values.set_view_timeline_insets(move(view_timeline_insets));

    Vector<Optional<Utf16FlyString>> transition_properties;
    for_each_comma_separated_value(CSS::PropertyID::TransitionProperty, [&](CSS::StyleValue const& value) {
        transition_properties.append(value.is_custom_ident() ? Optional<Utf16FlyString> { value.as_custom_ident().custom_ident() } : Optional<Utf16FlyString> {});
    });
    computed_values.set_transition_properties(move(transition_properties));
    Vector<CSS::Time> transition_durations;
    for_each_comma_separated_value(CSS::PropertyID::TransitionDuration, [&](CSS::StyleValue const& value) {
        transition_durations.append(CSS::Time::from_style_value(NonnullRefPtr<CSS::StyleValue const> { value }, {}));
    });
    computed_values.set_transition_durations(move(transition_durations));
    Vector<CSS::EasingFunction> transition_timing_functions;
    CSS::StyleValueVector transition_timing_function_style_values;
    for_each_comma_separated_value(CSS::PropertyID::TransitionTimingFunction, [&](CSS::StyleValue const& value) {
        transition_timing_functions.append(CSS::EasingFunction::from_style_value(value));
        transition_timing_function_style_values.append(value);
    });
    computed_values.set_transition_timing_functions(move(transition_timing_functions));
    computed_values.set_transition_timing_function_style_values(move(transition_timing_function_style_values));
    Vector<CSS::Time> transition_delays;
    for_each_comma_separated_value(CSS::PropertyID::TransitionDelay, [&](CSS::StyleValue const& value) {
        transition_delays.append(CSS::Time::from_style_value(NonnullRefPtr<CSS::StyleValue const> { value }, {}));
    });
    computed_values.set_transition_delays(move(transition_delays));
    Vector<CSS::TransitionBehavior> transition_behaviors;
    for_each_comma_separated_value(CSS::PropertyID::TransitionBehavior, [&](CSS::StyleValue const& value) {
        transition_behaviors.append(CSS::keyword_to_transition_behavior(value.to_keyword()).release_value());
    });
    computed_values.set_transition_behaviors(move(transition_behaviors));

    computed_values.set_text_align(computed_style.text_align());
    computed_values.set_text_justify(computed_style.text_justify());
    computed_values.set_text_overflow(computed_style.text_overflow());
    auto const& text_underline_offset_value = computed_style.property(CSS::PropertyID::TextUnderlineOffset);
    CSS::TextUnderlineOffset text_underline_offset;
    text_underline_offset.used_value = computed_style.text_underline_offset();
    if (text_underline_offset_value.to_keyword() != CSS::Keyword::Auto)
        text_underline_offset.computed_value = CSS::LengthPercentage::from_style_value(text_underline_offset_value);
    computed_values.set_text_underline_offset(move(text_underline_offset));
    computed_values.set_text_underline_position(computed_style.text_underline_position());

    computed_values.set_text_indent(computed_style.text_indent());
    computed_values.set_text_wrap_mode(computed_style.text_wrap_mode());
    computed_values.set_text_wrap_style(CSS::keyword_to_text_wrap_style(computed_style.property(CSS::PropertyID::TextWrapStyle).to_keyword()).release_value());
    computed_values.set_tab_size(computed_style.tab_size());

    computed_values.set_white_space_collapse(computed_style.white_space_collapse());
    computed_values.set_white_space_trim(computed_style.white_space_trim());
    computed_values.set_word_break(computed_style.word_break());
    switch (computed_style.property(CSS::PropertyID::OverflowWrap).to_keyword()) {
    case CSS::Keyword::Normal:
        computed_values.set_overflow_wrap(CSS::OverflowWrap::Normal);
        break;
    case CSS::Keyword::BreakWord:
        computed_values.set_overflow_wrap(CSS::OverflowWrap::BreakWord);
        break;
    case CSS::Keyword::Anywhere:
        computed_values.set_overflow_wrap(CSS::OverflowWrap::Anywhere);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    auto integer_from_style_value = [](CSS::StyleValue const& value) -> u64 {
        i32 integer;
        if (value.is_integer())
            integer = value.as_integer().integer();
        else
            integer = value.as_calculated().resolve_integer({}).value();
        VERIFY(integer >= 0);
        return integer;
    };
    computed_values.set_orphans(integer_from_style_value(computed_style.property(CSS::PropertyID::Orphans)));
    computed_values.set_widows(integer_from_style_value(computed_style.property(CSS::PropertyID::Widows)));

    computed_values.set_word_spacing(computed_style.word_spacing());
    computed_values.set_letter_spacing(computed_style.letter_spacing());
    computed_values.set_word_spacing_style_value(computed_style.property(PropertyID::WordSpacing));
    computed_values.set_letter_spacing_style_value(computed_style.property(PropertyID::LetterSpacing));

    computed_values.set_float(computed_style.float_());

    computed_values.set_border_spacing_horizontal(computed_style.border_spacing_horizontal());
    computed_values.set_border_spacing_vertical(computed_style.border_spacing_vertical());

    computed_values.set_caption_side(computed_style.caption_side());
    computed_values.set_clear(computed_style.clear());
    computed_values.set_overflow_x(computed_style.overflow_x());
    computed_values.set_overflow_y(computed_style.overflow_y());
    computed_values.set_content_visibility(computed_style.content_visibility());
    auto cursor = computed_style.cursor();
    computed_values.set_cursor(move(cursor));
    computed_values.set_image_rendering(computed_style.image_rendering());
    computed_values.set_pointer_events(computed_style.pointer_events());
    computed_values.set_text_decoration_line(computed_style.text_decoration_line());
    computed_values.set_text_decoration_skip_ink(computed_style.text_decoration_skip_ink());
    computed_values.set_text_decoration_style(computed_style.text_decoration_style());
    computed_values.set_text_transform(computed_style.text_transform());

    auto list_style_type = computed_style.list_style_type(style_scope);
    auto const& list_style_type_value = computed_style.property(PropertyID::ListStyleType);
    if (list_style_type_value.is_counter_style() && list_style_type_value.as_counter_style().value().has<CounterStyleStyleValue::SymbolsFunction>()) {
        auto const& symbols = list_style_type_value.as_counter_style().value().get<CounterStyleStyleValue::SymbolsFunction>();
        auto counter_style = list_style_type.get<RefPtr<CounterStyle const>>();
        VERIFY(counter_style);
        list_style_type = ListStyleSymbols {
            .counter_style = counter_style.release_nonnull(),
            .type = symbols.type,
            .symbols = symbols.symbols,
        };
    }
    computed_values.set_list_style_type(move(list_style_type));
    computed_values.set_list_style_position(computed_style.list_style_position());
    auto const& list_style_image = computed_style.property(PropertyID::ListStyleImage);
    if (list_style_image.is_abstract_image())
        computed_values.set_list_style_image(list_style_image.as_abstract_image());

    computed_values.set_text_decoration_color(computed_style.color(CSS::PropertyID::TextDecorationColor, color_resolution_context));
    computed_values.set_text_decoration_thickness(computed_style.text_decoration_thickness());

    auto const& webkit_text_fill_color = computed_style.property(CSS::PropertyID::WebkitTextFillColor);
    computed_values.set_webkit_text_fill_color(
        webkit_text_fill_color.to_color(color_resolution_context).value(),
        webkit_text_fill_color.to_keyword() == Keyword::Currentcolor);

    computed_values.set_text_shadow(computed_style.text_shadow(color_resolution_context));

    computed_values.set_z_index(computed_style.z_index());
    computed_values.set_opacity(computed_style.opacity());

    computed_values.set_visibility(computed_style.visibility());

    computed_values.set_width(computed_style.size_value(CSS::PropertyID::Width));
    computed_values.set_min_width(computed_style.size_value(CSS::PropertyID::MinWidth));
    computed_values.set_max_width(computed_style.size_value(CSS::PropertyID::MaxWidth));

    computed_values.set_height(computed_style.size_value(CSS::PropertyID::Height));
    computed_values.set_min_height(computed_style.size_value(CSS::PropertyID::MinHeight));
    computed_values.set_max_height(computed_style.size_value(CSS::PropertyID::MaxHeight));

    computed_values.set_inset(computed_style.length_box(CSS::PropertyID::Left, CSS::PropertyID::Top, CSS::PropertyID::Right, CSS::PropertyID::Bottom, CSS::LengthPercentageOrAuto::make_auto()));
    for (auto property_id : { PropertyID::Top, PropertyID::Right, PropertyID::Bottom, PropertyID::Left }) {
        auto const& inset = computed_style.property(property_id);
        if (inset.is_anchor())
            computed_values.set_anchor_inset(property_id, inset);
    }
    computed_values.set_margin(computed_style.length_box(CSS::PropertyID::MarginLeft, CSS::PropertyID::MarginTop, CSS::PropertyID::MarginRight, CSS::PropertyID::MarginBottom, CSS::Length::make_px(0)));
    computed_values.set_padding(computed_style.length_box(CSS::PropertyID::PaddingLeft, CSS::PropertyID::PaddingTop, CSS::PropertyID::PaddingRight, CSS::PropertyID::PaddingBottom, CSS::Length::make_px(0)));
    computed_values.set_scroll_margin(computed_style.length_box(CSS::PropertyID::ScrollMarginLeft, CSS::PropertyID::ScrollMarginTop, CSS::PropertyID::ScrollMarginRight, CSS::PropertyID::ScrollMarginBottom, CSS::Length::make_px(0)));
    computed_values.set_scroll_padding(computed_style.length_box(CSS::PropertyID::ScrollPaddingLeft, CSS::PropertyID::ScrollPaddingTop, CSS::PropertyID::ScrollPaddingRight, CSS::PropertyID::ScrollPaddingBottom, CSS::LengthPercentageOrAuto::make_auto()));
    {
        auto extract_side = [&](CSS::PropertyID property_id) -> CSS::OverflowClipMarginSide {
            auto const& value = computed_style.property(property_id);
            if (value.is_overflow_clip_margin()) {
                auto const& overflow_clip_margin = value.as_overflow_clip_margin();
                CSSPixels offset = 0;
                if (overflow_clip_margin.offset().is_calculated())
                    offset = overflow_clip_margin.offset().as_calculated().resolve_length(color_resolution_context.calculation_resolution_context).value().absolute_length_to_px();
                else if (overflow_clip_margin.offset().is_length())
                    offset = overflow_clip_margin.offset().as_length().length().absolute_length_to_px();
                return { overflow_clip_margin.visual_box(), offset };
            }
            return {};
        };
        CSS::OverflowClipMarginData data;
        data.left = extract_side(CSS::PropertyID::OverflowClipMarginLeft);
        data.top = extract_side(CSS::PropertyID::OverflowClipMarginTop);
        data.right = extract_side(CSS::PropertyID::OverflowClipMarginRight);
        data.bottom = extract_side(CSS::PropertyID::OverflowClipMarginBottom);
        computed_values.set_overflow_clip_margin(data);
    }

    computed_values.set_box_shadow(computed_style.box_shadow(color_resolution_context));

    computed_values.set_rotate(computed_style.rotate());
    computed_values.set_translate(computed_style.translate());
    computed_values.set_scale(computed_style.scale());
    computed_values.set_transformations(computed_style.transformations());
    computed_values.set_transform_box(computed_style.transform_box());
    computed_values.set_transform_origin(computed_style.transform_origin());
    computed_values.set_transform_style(computed_style.transform_style());
    computed_values.set_perspective(computed_style.perspective());
    computed_values.set_perspective_origin(computed_style.perspective_origin());

    auto do_border_style = [&](CSS::BorderData& border, CSS::PropertyID width_property, CSS::PropertyID color_property, CSS::PropertyID style_property) {
        // FIXME: Support <image-1d>
        border.color = computed_style.color(color_property, color_resolution_context);
        border.line_style = computed_style.line_style(style_property);
        // FIXME: Interpolation can cause negative values - we clamp here but should instead clamp as part of interpolation
        auto computed_width = max(CSSPixels { 0 }, computed_style.length(width_property).absolute_length_to_px());

        // If the border-style corresponding to a given border-width is none or hidden, then the used width is 0.
        // https://drafts.csswg.org/css-backgrounds/#border-width
        if (border.line_style == CSS::LineStyle::None || border.line_style == CSS::LineStyle::Hidden) {
            border.width = 0;
        } else {
            border.width = computed_width;
        }
        return computed_width;
    };

    computed_values.set_border_left_computed_width(do_border_style(computed_values.border_left(), CSS::PropertyID::BorderLeftWidth, CSS::PropertyID::BorderLeftColor, CSS::PropertyID::BorderLeftStyle));
    computed_values.set_border_top_computed_width(do_border_style(computed_values.border_top(), CSS::PropertyID::BorderTopWidth, CSS::PropertyID::BorderTopColor, CSS::PropertyID::BorderTopStyle));
    computed_values.set_border_right_computed_width(do_border_style(computed_values.border_right(), CSS::PropertyID::BorderRightWidth, CSS::PropertyID::BorderRightColor, CSS::PropertyID::BorderRightStyle));
    computed_values.set_border_bottom_computed_width(do_border_style(computed_values.border_bottom(), CSS::PropertyID::BorderBottomWidth, CSS::PropertyID::BorderBottomColor, CSS::PropertyID::BorderBottomStyle));
    computed_values.set_border_left_color_style_value(computed_style.property(CSS::PropertyID::BorderLeftColor));
    computed_values.set_border_top_color_style_value(computed_style.property(CSS::PropertyID::BorderTopColor));
    computed_values.set_border_right_color_style_value(computed_style.property(CSS::PropertyID::BorderRightColor));
    computed_values.set_border_bottom_color_style_value(computed_style.property(CSS::PropertyID::BorderBottomColor));

    if (auto const& outline_color = computed_style.property(CSS::PropertyID::OutlineColor); outline_color.has_color())
        computed_values.set_outline_color(outline_color.to_color(color_resolution_context).value());
    auto const& outline_offset = computed_style.property(CSS::PropertyID::OutlineOffset);
    auto resolved_outline_offset = outline_offset.is_calculated()
        ? outline_offset.as_calculated().resolve_length(color_resolution_context.calculation_resolution_context).value()
        : outline_offset.as_length().length();
    computed_values.set_outline_offset(resolved_outline_offset.absolute_length_to_px());
    computed_values.set_outline_offset_style_value(outline_offset);
    computed_values.set_outline_style(computed_style.outline_style());

    // FIXME: Interpolation can cause negative values - we clamp here but should instead clamp as part of interpolation.
    computed_values.set_outline_width(max(CSSPixels { 0 }, computed_style.length(CSS::PropertyID::OutlineWidth).absolute_length_to_px()));

    computed_values.set_grid_auto_columns(computed_style.grid_auto_columns());
    computed_values.set_grid_auto_rows(computed_style.grid_auto_rows());
    computed_values.set_grid_template_columns(computed_style.grid_template_columns());
    computed_values.set_grid_template_rows(computed_style.grid_template_rows());
    computed_values.set_grid_column_end(computed_style.grid_column_end());
    computed_values.set_grid_column_start(computed_style.grid_column_start());
    computed_values.set_grid_row_end(computed_style.grid_row_end());
    computed_values.set_grid_row_start(computed_style.grid_row_start());
    computed_values.set_grid_template_areas(computed_style.grid_template_areas());
    computed_values.set_grid_auto_flow(computed_style.grid_auto_flow());

    computed_values.set_cx(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Cx)));
    computed_values.set_cy(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Cy)));
    computed_values.set_r(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::R)));
    computed_values.set_rx(CSS::LengthPercentageOrAuto::from_style_value(computed_style.property(CSS::PropertyID::Rx)));
    computed_values.set_ry(CSS::LengthPercentageOrAuto::from_style_value(computed_style.property(CSS::PropertyID::Ry)));
    computed_values.set_x(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::X)));
    computed_values.set_y(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Y)));

    computed_values.set_fill(computed_style.fill(color_resolution_context));
    computed_values.set_stroke(computed_style.stroke(color_resolution_context));

    computed_values.set_stop_color(computed_style.color(CSS::PropertyID::StopColor, color_resolution_context));

    auto const& stroke_width = computed_style.property(CSS::PropertyID::StrokeWidth);
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    if (stroke_width.is_number())
        computed_values.set_stroke_width(CSS::Length::make_px(CSSPixels::nearest_value_for(stroke_width.as_number().number())));
    else
        computed_values.set_stroke_width(CSS::LengthPercentage::from_style_value(stroke_width));
    computed_values.set_shape_rendering(computed_style.shape_rendering());
    computed_values.set_paint_order(computed_style.paint_order());
    auto const& paint_order = computed_style.property(PropertyID::PaintOrder);
    computed_values.set_paint_order_serialization(
        paint_order.is_value_list() ? paint_order.as_value_list().size() : paint_order.to_keyword() == Keyword::Normal ? 0
                                                                                                                       : 1,
        paint_order.to_keyword() == Keyword::Normal);

    // FIXME: Remove this once we support URL values in mask_layers and can therefore use it in
    //        `establishes_stacking_context()`
    auto const& mask_image = [&] -> CSS::StyleValue const& {
        auto const& value = computed_style.property(CSS::PropertyID::MaskImage);

        if (value.is_value_list())
            return value.as_value_list().values()[0];

        return value;
    }();
    if (mask_image.is_url()) {
        computed_values.set_mask(mask_image.as_url().url());
    } else if (mask_image.is_abstract_image()) {
        auto const& abstract_image = mask_image.as_abstract_image();
        computed_values.set_mask_image(abstract_image);
    }

    computed_values.set_mask_type(computed_style.mask_type());

    auto const& clip_path = computed_style.property(CSS::PropertyID::ClipPath);
    if (clip_path.is_url())
        computed_values.set_clip_path(clip_path.as_url().url());
    else if (clip_path.is_basic_shape())
        computed_values.set_clip_path(clip_path.as_basic_shape());
    computed_values.set_clip_rule(computed_style.clip_rule());
    computed_values.set_fill_rule(computed_style.fill_rule());

    computed_values.set_fill_opacity(computed_style.fill_opacity());
    computed_values.set_stroke_dasharray(computed_style.stroke_dasharray());

    auto const& stroke_dashoffset = computed_style.property(CSS::PropertyID::StrokeDashoffset);
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    if (stroke_dashoffset.is_number())
        computed_values.set_stroke_dashoffset(CSS::Length::make_px(CSSPixels::nearest_value_for(stroke_dashoffset.as_number().number())));
    else
        computed_values.set_stroke_dashoffset(CSS::LengthPercentage::from_style_value(stroke_dashoffset));

    computed_values.set_stroke_linecap(computed_style.stroke_linecap());
    computed_values.set_stroke_linejoin(computed_style.stroke_linejoin());
    computed_values.set_vector_effect(computed_style.vector_effect());
    computed_values.set_stroke_miterlimit(computed_style.stroke_miterlimit());

    computed_values.set_stroke_opacity(computed_style.stroke_opacity());
    computed_values.set_stop_opacity(computed_style.stop_opacity());

    computed_values.set_text_anchor(computed_style.text_anchor());
    computed_values.set_dominant_baseline(computed_style.dominant_baseline());

    if (auto const& column_count = computed_style.property(CSS::PropertyID::ColumnCount); column_count.to_keyword() != Keyword::Auto)
        computed_values.set_column_count(CSS::ColumnCount::make_integer(int_from_style_value(NonnullRefPtr<StyleValue const> { column_count })));

    computed_values.set_column_span(computed_style.column_span());

    computed_values.set_column_width(computed_style.size_value(CSS::PropertyID::ColumnWidth));
    computed_values.set_column_height(computed_style.size_value(CSS::PropertyID::ColumnHeight));

    computed_values.set_column_gap(computed_style.gap_value(CSS::PropertyID::ColumnGap));
    computed_values.set_row_gap(computed_style.gap_value(CSS::PropertyID::RowGap));

    computed_values.set_border_collapse(computed_style.border_collapse());

    computed_values.set_empty_cells(computed_style.empty_cells());

    computed_values.set_table_layout(computed_style.table_layout());

    auto const& aspect_ratio = computed_style.property(CSS::PropertyID::AspectRatio);
    if (aspect_ratio.is_value_list()) {
        auto const& values_list = aspect_ratio.as_value_list().values();
        if (values_list.size() == 2
            && values_list[0]->is_keyword() && values_list[0]->as_keyword().keyword() == CSS::Keyword::Auto
            && values_list[1]->is_ratio()) {
            auto ratio = values_list[1]->as_ratio().resolved();
            if (ratio.is_degenerate())
                computed_values.set_aspect_ratio({ true, {}, true, ratio });
            else
                computed_values.set_aspect_ratio({ true, ratio, true, ratio });
        }
    } else if (aspect_ratio.is_keyword() && aspect_ratio.as_keyword().keyword() == CSS::Keyword::Auto) {
        computed_values.set_aspect_ratio({ true, {}, true, {} });
    } else if (aspect_ratio.is_ratio()) {
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio
        // If the <ratio> is degenerate, the property instead behaves as auto.
        if (aspect_ratio.as_ratio().resolved().is_degenerate())
            computed_values.set_aspect_ratio({ true, {}, false, aspect_ratio.as_ratio().resolved() });
        else
            computed_values.set_aspect_ratio({ false, aspect_ratio.as_ratio().resolved(), false, aspect_ratio.as_ratio().resolved() });
    }

    computed_values.set_touch_action(computed_style.touch_action());

    auto const& math_shift_value = computed_style.property(CSS::PropertyID::MathShift);
    if (auto math_shift = keyword_to_math_shift(math_shift_value.to_keyword()); math_shift.has_value())
        computed_values.set_math_shift(math_shift.value());

    auto const& math_style_value = computed_style.property(CSS::PropertyID::MathStyle);
    if (auto math_style = keyword_to_math_style(math_style_value.to_keyword()); math_style.has_value())
        computed_values.set_math_style(math_style.value());

    computed_values.set_math_depth(computed_style.math_depth());
    computed_values.set_quotes(computed_style.quotes());
    computed_values.set_counter_increment(computed_style.counter_data(CSS::PropertyID::CounterIncrement));
    computed_values.set_counter_reset(computed_style.counter_data(CSS::PropertyID::CounterReset));
    computed_values.set_counter_set(computed_style.counter_data(CSS::PropertyID::CounterSet));

    computed_values.set_object_fit(computed_style.object_fit());
    computed_values.set_object_position(computed_style.object_position());
    computed_values.set_direction(computed_style.direction());
    computed_values.set_unicode_bidi(computed_style.unicode_bidi());
    computed_values.set_scroll_behavior(CSS::keyword_to_scroll_behavior(computed_style.property(CSS::PropertyID::ScrollBehavior).to_keyword()).release_value());
    computed_values.set_scrollbar_color(computed_style.scrollbar_color(color_resolution_context));
    computed_values.set_scrollbar_gutter(computed_style.property(CSS::PropertyID::ScrollbarGutter).as_scrollbar_gutter().value());
    computed_values.set_scrollbar_width(computed_style.scrollbar_width());
    computed_values.set_shape_image_threshold(computed_style.property(CSS::PropertyID::ShapeImageThreshold).as_opacity_value().resolved());
    computed_values.set_shape_margin(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::ShapeMargin)));
    CSS::ShapeOutsideData shape_outside;
    auto apply_shape_outside_item = [&](CSS::StyleValue const& item) {
        if (item.is_url())
            shape_outside.image = item.as_url().url();
        else if (item.is_abstract_image())
            shape_outside.image = NonnullRefPtr<CSS::AbstractImageStyleValue const> { item.as_abstract_image() };
        else if (item.is_basic_shape())
            shape_outside.basic_shape = item.as_basic_shape();
        else if (auto shape_box = CSS::keyword_to_shape_box(item.to_keyword()); shape_box.has_value())
            shape_outside.shape_box = *shape_box;
    };
    auto const& shape_outside_value = computed_style.property(CSS::PropertyID::ShapeOutside);
    if (shape_outside_value.is_value_list()) {
        for (auto const& item : shape_outside_value.as_value_list().values())
            apply_shape_outside_item(item);
    } else {
        apply_shape_outside_item(shape_outside_value);
    }
    computed_values.set_shape_outside(move(shape_outside));
    computed_values.set_writing_mode(computed_style.writing_mode());
    computed_values.set_user_select(computed_style.user_select());
    computed_values.set_isolation(computed_style.isolation());
    computed_values.set_mix_blend_mode(computed_style.mix_blend_mode());
    computed_values.set_view_transition_name(computed_style.view_transition_name());
    computed_values.set_contain(computed_style.contain());
    computed_values.set_container_name(computed_style.container_name());
    computed_values.set_container_type(computed_style.container_type());
    computed_values.set_will_change(computed_style.will_change());

    computed_values.set_caret_color(computed_style.caret_color(color_resolution_context));
    computed_values.set_color_interpolation(computed_style.color_interpolation());
    computed_values.set_color_interpolation_filters(computed_style.color_interpolation_filters());
    computed_values.set_resize(computed_style.resize());

    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        computed_values.set_property_important(property_id, computed_style.is_property_important(property_id));
        computed_values.set_property_inherited(property_id, computed_style.is_property_inherited(property_id));
    }
    computed_values.set_depends_on_viewport_metrics(computed_style.depends_on_viewport_metrics());
    computed_values.set_font_metrics_depend_on_viewport_metrics(computed_style.font_metrics_depend_on_viewport_metrics());
    computed_values.set_in_display_none_subtree(computed_style.in_display_none_subtree());
    u64 pseudo_element_styles = 0;
    for (auto i = 0; i < to_underlying(PseudoElement::KnownPseudoElementCount); ++i) {
        auto pseudo_element = static_cast<PseudoElement>(i);
        if (computed_style.has_pseudo_element_style(pseudo_element))
            pseudo_element_styles |= 1ull << i;
    }
    computed_values.set_pseudo_element_styles(pseudo_element_styles);
    computed_values.set_inheritance_dependent_specified_values(computed_style.inheritance_dependent_specified_values());
    computed_values.set_raw_cascaded_font_size(computed_style.raw_cascaded_font_size());

    return move(builder).build();
}

}
