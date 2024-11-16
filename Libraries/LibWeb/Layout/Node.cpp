/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundRepeatStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Layout {

Node::Node(DOM::Document& document, DOM::Node* node)
    : m_dom_node(node ? *node : document)
    , m_anonymous(node == nullptr)
{
    if (node)
        node->set_layout_node({}, *this);
}

Node::~Node() = default;

void Node::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_dom_node);
    for (auto const& paintable : m_paintable) {
        visitor.visit(GC::Ptr { &paintable });
    }
    visitor.visit(m_pseudo_element_generator);
    TreeNode::visit_edges(visitor);
}

// https://www.w3.org/TR/css-display-3/#out-of-flow
bool Node::is_out_of_flow(FormattingContext const& formatting_context) const
{
    // A layout node is out of flow if either:

    // 1. It is floated (which requires that floating is not inhibited).
    if (!formatting_context.inhibits_floating() && computed_values().float_() != CSS::Float::None)
        return true;

    // 2. It is "absolutely positioned".
    if (is_absolutely_positioned())
        return true;

    return false;
}

bool Node::can_contain_boxes_with_position_absolute() const
{
    if (computed_values().position() != CSS::Positioning::Static)
        return true;

    if (is<Viewport>(*this))
        return true;

    // https://w3c.github.io/csswg-drafts/css-transforms-1/#propdef-transform
    // Any computed value other than none for the transform affects containing block and stacking context
    if (!computed_values().transformations().is_empty())
        return true;

    return false;
}

static Box const* nearest_ancestor_capable_of_forming_a_containing_block(Node const& node)
{
    for (auto const* ancestor = node.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->is_block_container()
            || ancestor->display().is_flex_inside()
            || ancestor->display().is_grid_inside()
            || ancestor->is_svg_svg_box()) {
            return verify_cast<Box>(ancestor);
        }
    }
    return nullptr;
}

Box const* Node::containing_block() const
{
    if (is<TextNode>(*this))
        return nearest_ancestor_capable_of_forming_a_containing_block(*this);

    auto position = computed_values().position();

    // https://drafts.csswg.org/css-position-3/#absolute-cb
    if (position == CSS::Positioning::Absolute) {
        auto const* ancestor = parent();
        while (ancestor && !ancestor->can_contain_boxes_with_position_absolute())
            ancestor = ancestor->parent();
        while (ancestor && ancestor->is_anonymous())
            ancestor = nearest_ancestor_capable_of_forming_a_containing_block(*ancestor);
        return static_cast<Box const*>(ancestor);
    }

    if (position == CSS::Positioning::Fixed)
        return &root();

    return nearest_ancestor_capable_of_forming_a_containing_block(*this);
}

Box const* Node::static_position_containing_block() const
{
    return nearest_ancestor_capable_of_forming_a_containing_block(*this);
}

Box const* Node::non_anonymous_containing_block() const
{
    auto nearest_ancestor_box = containing_block();
    VERIFY(nearest_ancestor_box);
    while (nearest_ancestor_box->is_anonymous()) {
        nearest_ancestor_box = nearest_ancestor_box->containing_block();
        VERIFY(nearest_ancestor_box);
    }
    return nearest_ancestor_box;
}

// https://developer.mozilla.org/en-US/docs/Web/CSS/CSS_Positioning/Understanding_z_index/The_stacking_context
bool Node::establishes_stacking_context() const
{
    // NOTE: While MDN is not authoritative, there isn't a single convenient location
    //       in the CSS specifications where the rules for stacking contexts is described.
    //       That's why the "spec link" here points to MDN.

    if (!has_style())
        return false;

    if (is_svg_box() || is_svg_svg_box())
        return false;

    // We make a stacking context for the viewport. Painting and hit testing starts from here.
    if (is_viewport())
        return true;

    // Root element of the document (<html>).
    if (is_root_element())
        return true;

    auto position = computed_values().position();

    // Element with a position value absolute or relative and z-index value other than auto.
    if (position == CSS::Positioning::Absolute || position == CSS::Positioning::Relative) {
        if (computed_values().z_index().has_value()) {
            return true;
        }
    }

    // Element with a position value fixed or sticky.
    if (position == CSS::Positioning::Fixed || position == CSS::Positioning::Sticky)
        return true;

    if (!computed_values().transformations().is_empty())
        return true;

    // Element that is a child of a flex container, with z-index value other than auto.
    if (parent() && parent()->display().is_flex_inside() && computed_values().z_index().has_value())
        return true;

    // Element that is a child of a grid container, with z-index value other than auto.
    if (parent() && parent()->display().is_grid_inside() && computed_values().z_index().has_value())
        return true;

    // https://drafts.fxtf.org/filter-effects/#FilterProperty
    // https://drafts.fxtf.org/filter-effects-2/#backdrop-filter-operation
    // A computed value of other than none results in the creation of both a stacking context
    // [CSS21] and a Containing Block for absolute and fixed position descendants, unless the
    // element it applies to is a document root element in the current browsing context.
    // Spec Note: This rule works in the same way as for the filter property.
    if (!computed_values().backdrop_filter().is_none() || !computed_values().filter().is_none())
        return true;

    // Element with any of the following properties with value other than none:
    // - transform
    // - filter
    // - backdrop-filter
    // - perspective
    // - clip-path
    // - mask / mask-image / mask-border
    if (computed_values().mask().has_value() || computed_values().clip_path().has_value() || computed_values().mask_image())
        return true;

    return computed_values().opacity() < 1.0f;
}

GC::Ptr<HTML::Navigable> Node::navigable() const
{
    return document().navigable();
}

Viewport const& Node::root() const
{
    VERIFY(document().layout_node());
    return *document().layout_node();
}

Viewport& Node::root()
{
    VERIFY(document().layout_node());
    return *document().layout_node();
}

bool Node::is_floating() const
{
    if (!has_style())
        return false;
    // flex-items don't float.
    if (is_flex_item())
        return false;
    return computed_values().float_() != CSS::Float::None;
}

bool Node::is_positioned() const
{
    return has_style() && computed_values().position() != CSS::Positioning::Static;
}

bool Node::is_absolutely_positioned() const
{
    if (!has_style())
        return false;
    auto position = computed_values().position();
    return position == CSS::Positioning::Absolute || position == CSS::Positioning::Fixed;
}

bool Node::is_fixed_position() const
{
    if (!has_style())
        return false;
    auto position = computed_values().position();
    return position == CSS::Positioning::Fixed;
}

bool Node::is_sticky_position() const
{
    if (!has_style())
        return false;
    auto position = computed_values().position();
    return position == CSS::Positioning::Sticky;
}

NodeWithStyle::NodeWithStyle(DOM::Document& document, DOM::Node* node, CSS::StyleProperties computed_style)
    : Node(document, node)
    , m_computed_values(make<CSS::ComputedValues>())
{
    m_has_style = true;
    apply_style(computed_style);
}

NodeWithStyle::NodeWithStyle(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : Node(document, node)
    , m_computed_values(move(computed_values))
{
    m_has_style = true;
}

void NodeWithStyle::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& layer : computed_values().background_layers()) {
        if (layer.background_image && layer.background_image->is_image())
            layer.background_image->as_image().visit_edges(visitor);
    }
    if (m_list_style_image && m_list_style_image->is_image())
        m_list_style_image->as_image().visit_edges(visitor);
}

// https://www.w3.org/TR/css-values-4/#snap-a-length-as-a-border-width
static CSSPixels snap_a_length_as_a_border_width(double device_pixels_per_css_pixel, CSSPixels length)
{
    // 1. Assert: len is non-negative.
    VERIFY(length >= 0);

    // 2. If len is an integer number of device pixels, do nothing.
    auto device_pixels = length.to_double() * device_pixels_per_css_pixel;
    if (device_pixels == trunc(device_pixels))
        return length;

    // 3. If len is greater than zero, but less than 1 device pixel, round len up to 1 device pixel.
    if (device_pixels > 0 && device_pixels < 1)
        return CSSPixels::nearest_value_for(1 / device_pixels_per_css_pixel);

    // 4. If len is greater than 1 device pixel, round it down to the nearest integer number of device pixels.
    if (device_pixels > 1)
        return CSSPixels::nearest_value_for(floor(device_pixels) / device_pixels_per_css_pixel);

    return length;
}

void NodeWithStyle::apply_style(const CSS::StyleProperties& computed_style)
{
    auto& computed_values = mutable_computed_values();

    // NOTE: color must be set first to ensure currentColor can be resolved in other properties (e.g. background-color).
    computed_values.set_color(computed_style.color_or_fallback(CSS::PropertyID::Color, *this, CSS::InitialValues::color()));

    // NOTE: We have to be careful that font-related properties get set in the right order.
    //       m_font is used by Length::to_px() when resolving sizes against this layout node.
    //       That's why it has to be set before everything else.
    computed_values.set_font_list(computed_style.computed_font_list());
    computed_values.set_font_size(computed_style.property(CSS::PropertyID::FontSize).as_length().length().to_px(*this));
    computed_values.set_font_weight(round_to<int>(computed_style.property(CSS::PropertyID::FontWeight).as_number().number()));
    computed_values.set_line_height(computed_style.line_height());

    computed_values.set_vertical_align(computed_style.vertical_align());

    {
        auto const& attachments = computed_style.property(CSS::PropertyID::BackgroundAttachment);
        auto const& clips = computed_style.property(CSS::PropertyID::BackgroundClip);
        auto const& images = computed_style.property(CSS::PropertyID::BackgroundImage);
        auto const& origins = computed_style.property(CSS::PropertyID::BackgroundOrigin);
        auto const& x_positions = computed_style.property(CSS::PropertyID::BackgroundPositionX);
        auto const& y_positions = computed_style.property(CSS::PropertyID::BackgroundPositionY);
        auto const& repeats = computed_style.property(CSS::PropertyID::BackgroundRepeat);
        auto const& sizes = computed_style.property(CSS::PropertyID::BackgroundSize);

        auto count_layers = [](auto const& maybe_style_value) -> size_t {
            if (maybe_style_value.is_value_list())
                return maybe_style_value.as_value_list().size();
            else
                return 1;
        };

        auto value_for_layer = [](auto const& style_value, size_t layer_index) -> RefPtr<CSS::CSSStyleValue const> {
            if (style_value.is_value_list())
                return style_value.as_value_list().value_at(layer_index, true);
            return style_value;
        };

        size_t layer_count = 1;
        layer_count = max(layer_count, count_layers(attachments));
        layer_count = max(layer_count, count_layers(clips));
        layer_count = max(layer_count, count_layers(images));
        layer_count = max(layer_count, count_layers(origins));
        layer_count = max(layer_count, count_layers(x_positions));
        layer_count = max(layer_count, count_layers(y_positions));
        layer_count = max(layer_count, count_layers(repeats));
        layer_count = max(layer_count, count_layers(sizes));

        Vector<CSS::BackgroundLayerData> layers;
        layers.ensure_capacity(layer_count);

        for (size_t layer_index = 0; layer_index < layer_count; layer_index++) {
            CSS::BackgroundLayerData layer;

            if (auto image_value = value_for_layer(images, layer_index); image_value) {
                if (image_value->is_abstract_image()) {
                    layer.background_image = image_value->as_abstract_image();
                    const_cast<CSS::AbstractImageStyleValue&>(*layer.background_image).load_any_resources(document());
                }
            }

            if (auto attachment_value = value_for_layer(attachments, layer_index); attachment_value && attachment_value->is_keyword()) {
                switch (attachment_value->to_keyword()) {
                case CSS::Keyword::Fixed:
                    layer.attachment = CSS::BackgroundAttachment::Fixed;
                    break;
                case CSS::Keyword::Local:
                    layer.attachment = CSS::BackgroundAttachment::Local;
                    break;
                case CSS::Keyword::Scroll:
                    layer.attachment = CSS::BackgroundAttachment::Scroll;
                    break;
                default:
                    break;
                }
            }

            auto as_box = [](auto keyword) {
                switch (keyword) {
                case CSS::Keyword::BorderBox:
                    return CSS::BackgroundBox::BorderBox;
                case CSS::Keyword::ContentBox:
                    return CSS::BackgroundBox::ContentBox;
                case CSS::Keyword::PaddingBox:
                    return CSS::BackgroundBox::PaddingBox;
                case CSS::Keyword::Text:
                    return CSS::BackgroundBox::Text;
                default:
                    VERIFY_NOT_REACHED();
                }
            };

            if (auto origin_value = value_for_layer(origins, layer_index); origin_value && origin_value->is_keyword()) {
                layer.origin = as_box(origin_value->to_keyword());
            }

            if (auto clip_value = value_for_layer(clips, layer_index); clip_value && clip_value->is_keyword()) {
                layer.clip = as_box(clip_value->to_keyword());
            }

            if (auto position_value = value_for_layer(x_positions, layer_index); position_value && position_value->is_edge()) {
                auto& position = position_value->as_edge();
                layer.position_edge_x = position.edge();
                layer.position_offset_x = position.offset();
            }

            if (auto position_value = value_for_layer(y_positions, layer_index); position_value && position_value->is_edge()) {
                auto& position = position_value->as_edge();
                layer.position_edge_y = position.edge();
                layer.position_offset_y = position.offset();
            };

            if (auto size_value = value_for_layer(sizes, layer_index); size_value) {
                if (size_value->is_background_size()) {
                    auto& size = size_value->as_background_size();
                    layer.size_type = CSS::BackgroundSize::LengthPercentage;
                    layer.size_x = size.size_x();
                    layer.size_y = size.size_y();
                } else if (size_value->is_keyword()) {
                    switch (size_value->to_keyword()) {
                    case CSS::Keyword::Contain:
                        layer.size_type = CSS::BackgroundSize::Contain;
                        break;
                    case CSS::Keyword::Cover:
                        layer.size_type = CSS::BackgroundSize::Cover;
                        break;
                    default:
                        break;
                    }
                }
            }

            if (auto repeat_value = value_for_layer(repeats, layer_index); repeat_value && repeat_value->is_background_repeat()) {
                layer.repeat_x = repeat_value->as_background_repeat().repeat_x();
                layer.repeat_y = repeat_value->as_background_repeat().repeat_y();
            }

            layers.append(move(layer));
        }

        computed_values.set_background_layers(move(layers));
    }
    computed_values.set_background_color(computed_style.color_or_fallback(CSS::PropertyID::BackgroundColor, *this, CSS::InitialValues::background_color()));

    if (auto box_sizing = computed_style.box_sizing(); box_sizing.has_value())
        computed_values.set_box_sizing(box_sizing.release_value());

    if (auto maybe_font_variant = computed_style.font_variant(); maybe_font_variant.has_value())
        computed_values.set_font_variant(maybe_font_variant.release_value());
    if (auto maybe_font_language_override = computed_style.font_language_override(); maybe_font_language_override.has_value())
        computed_values.set_font_language_override(maybe_font_language_override.release_value());
    if (auto maybe_font_feature_settings = computed_style.font_feature_settings(); maybe_font_feature_settings.has_value())
        computed_values.set_font_feature_settings(maybe_font_feature_settings.release_value());
    if (auto maybe_font_variation_settings = computed_style.font_variation_settings(); maybe_font_variation_settings.has_value())
        computed_values.set_font_variation_settings(maybe_font_variation_settings.release_value());

    auto const& border_bottom_left_radius = computed_style.property(CSS::PropertyID::BorderBottomLeftRadius);
    if (border_bottom_left_radius.is_border_radius()) {
        computed_values.set_border_bottom_left_radius(
            CSS::BorderRadiusData {
                border_bottom_left_radius.as_border_radius().horizontal_radius(),
                border_bottom_left_radius.as_border_radius().vertical_radius() });
    }
    auto const& border_bottom_right_radius = computed_style.property(CSS::PropertyID::BorderBottomRightRadius);
    if (border_bottom_right_radius.is_border_radius()) {
        computed_values.set_border_bottom_right_radius(
            CSS::BorderRadiusData {
                border_bottom_right_radius.as_border_radius().horizontal_radius(),
                border_bottom_right_radius.as_border_radius().vertical_radius() });
    }
    auto const& border_top_left_radius = computed_style.property(CSS::PropertyID::BorderTopLeftRadius);
    if (border_top_left_radius.is_border_radius()) {
        computed_values.set_border_top_left_radius(
            CSS::BorderRadiusData {
                border_top_left_radius.as_border_radius().horizontal_radius(),
                border_top_left_radius.as_border_radius().vertical_radius() });
    }
    auto const& border_top_right_radius = computed_style.property(CSS::PropertyID::BorderTopRightRadius);
    if (border_top_right_radius.is_border_radius()) {
        computed_values.set_border_top_right_radius(
            CSS::BorderRadiusData {
                border_top_right_radius.as_border_radius().horizontal_radius(),
                border_top_right_radius.as_border_radius().vertical_radius() });
    }
    computed_values.set_display(computed_style.display());

    auto flex_direction = computed_style.flex_direction();
    if (flex_direction.has_value())
        computed_values.set_flex_direction(flex_direction.value());

    auto flex_wrap = computed_style.flex_wrap();
    if (flex_wrap.has_value())
        computed_values.set_flex_wrap(flex_wrap.value());

    auto flex_basis = computed_style.flex_basis();
    if (flex_basis.has_value())
        computed_values.set_flex_basis(flex_basis.value());

    computed_values.set_flex_grow(computed_style.flex_grow());
    computed_values.set_flex_shrink(computed_style.flex_shrink());
    computed_values.set_order(computed_style.order());
    computed_values.set_clip(computed_style.clip());

    auto resolve_filter = [this](CSS::Filter const& computed_filter) -> CSS::ResolvedFilter {
        CSS::ResolvedFilter resolved_filter;
        for (auto const& filter : computed_filter.filters()) {
            filter.visit(
                [&](CSS::FilterOperation::Blur const& blur) {
                    resolved_filter.filters.append(CSS::ResolvedFilter::Blur {
                        .radius = blur.resolved_radius(*this) });
                },
                [&](CSS::FilterOperation::DropShadow const& drop_shadow) {
                    auto context = CSS::Length::ResolutionContext::for_layout_node(*this);
                    // The default value for omitted values is missing length values set to 0
                    // and the missing used color is taken from the color property.
                    resolved_filter.filters.append(CSS::ResolvedFilter::DropShadow {
                        .offset_x = drop_shadow.offset_x.resolved(context).to_px(*this).to_double(),
                        .offset_y = drop_shadow.offset_y.resolved(context).to_px(*this).to_double(),
                        .radius = drop_shadow.radius.has_value() ? drop_shadow.radius->resolved(context).to_px(*this).to_double() : 0.0,
                        .color = drop_shadow.color.has_value() ? *drop_shadow.color : this->computed_values().color() });
                },
                [&](CSS::FilterOperation::Color const& color_operation) {
                    resolved_filter.filters.append(CSS::ResolvedFilter::Color {
                        .type = color_operation.operation,
                        .amount = color_operation.resolved_amount() });
                },
                [&](CSS::FilterOperation::HueRotate const& hue_rotate) {
                    resolved_filter.filters.append(CSS::ResolvedFilter::HueRotate { .angle_degrees = hue_rotate.angle_degrees(*this) });
                });
        }
        return resolved_filter;
    };
    if (computed_style.backdrop_filter().has_filters())
        computed_values.set_backdrop_filter(resolve_filter(computed_style.backdrop_filter()));
    if (computed_style.filter().has_filters())
        computed_values.set_filter(resolve_filter(computed_style.filter()));

    auto justify_content = computed_style.justify_content();
    if (justify_content.has_value())
        computed_values.set_justify_content(justify_content.value());

    auto justify_items = computed_style.justify_items();
    if (justify_items.has_value())
        computed_values.set_justify_items(justify_items.value());

    auto justify_self = computed_style.justify_self();
    if (justify_self.has_value())
        computed_values.set_justify_self(justify_self.value());

    auto accent_color = computed_style.accent_color(*this);
    if (accent_color.has_value())
        computed_values.set_accent_color(accent_color.value());

    auto align_content = computed_style.align_content();
    if (align_content.has_value())
        computed_values.set_align_content(align_content.value());

    auto align_items = computed_style.align_items();
    if (align_items.has_value())
        computed_values.set_align_items(align_items.value());

    auto align_self = computed_style.align_self();
    if (align_self.has_value())
        computed_values.set_align_self(align_self.value());

    auto appearance = computed_style.appearance();
    if (appearance.has_value())
        computed_values.set_appearance(appearance.value());

    auto position = computed_style.position();
    if (position.has_value())
        computed_values.set_position(position.value());

    auto text_align = computed_style.text_align();
    if (text_align.has_value())
        computed_values.set_text_align(text_align.value());

    auto text_justify = computed_style.text_justify();
    if (text_align.has_value())
        computed_values.set_text_justify(text_justify.value());

    if (auto text_indent = computed_style.length_percentage(CSS::PropertyID::TextIndent); text_indent.has_value())
        computed_values.set_text_indent(text_indent.release_value());

    if (auto text_overflow = computed_style.text_overflow(); text_overflow.has_value())
        computed_values.set_text_overflow(text_overflow.release_value());

    auto tab_size = computed_style.tab_size();
    computed_values.set_tab_size(tab_size);

    auto white_space = computed_style.white_space();
    if (white_space.has_value())
        computed_values.set_white_space(white_space.value());

    auto word_break = computed_style.word_break();
    if (word_break.has_value())
        computed_values.set_word_break(word_break.value());

    auto word_spacing = computed_style.word_spacing();
    if (word_spacing.has_value())
        computed_values.set_word_spacing(word_spacing.value());

    auto letter_spacing = computed_style.letter_spacing();
    if (letter_spacing.has_value())
        computed_values.set_letter_spacing(letter_spacing.value());

    auto float_ = computed_style.float_();
    if (float_.has_value())
        computed_values.set_float(float_.value());

    computed_values.set_border_spacing_horizontal(computed_style.border_spacing_horizontal(*this));
    computed_values.set_border_spacing_vertical(computed_style.border_spacing_vertical(*this));

    auto caption_side = computed_style.caption_side();
    if (caption_side.has_value())
        computed_values.set_caption_side(caption_side.value());

    auto clear = computed_style.clear();
    if (clear.has_value())
        computed_values.set_clear(clear.value());

    auto overflow_x = computed_style.overflow_x();
    if (overflow_x.has_value())
        computed_values.set_overflow_x(overflow_x.value());

    auto overflow_y = computed_style.overflow_y();
    if (overflow_y.has_value())
        computed_values.set_overflow_y(overflow_y.value());

    auto content_visibility = computed_style.content_visibility();
    if (content_visibility.has_value())
        computed_values.set_content_visibility(content_visibility.value());

    auto cursor = computed_style.cursor();
    if (cursor.has_value())
        computed_values.set_cursor(cursor.value());

    auto image_rendering = computed_style.image_rendering();
    if (image_rendering.has_value())
        computed_values.set_image_rendering(image_rendering.value());

    auto pointer_events = computed_style.pointer_events();
    if (pointer_events.has_value())
        computed_values.set_pointer_events(pointer_events.value());

    computed_values.set_text_decoration_line(computed_style.text_decoration_line());

    auto text_decoration_style = computed_style.text_decoration_style();
    if (text_decoration_style.has_value())
        computed_values.set_text_decoration_style(text_decoration_style.value());

    auto text_transform = computed_style.text_transform();
    if (text_transform.has_value())
        computed_values.set_text_transform(text_transform.value());

    if (auto list_style_type = computed_style.list_style_type(); list_style_type.has_value())
        computed_values.set_list_style_type(list_style_type.value());

    auto const& list_style_image = computed_style.property(CSS::PropertyID::ListStyleImage);
    if (list_style_image.is_abstract_image()) {
        m_list_style_image = list_style_image.as_abstract_image();
        const_cast<CSS::AbstractImageStyleValue&>(*m_list_style_image).load_any_resources(document());
    }

    if (auto list_style_position = computed_style.list_style_position(); list_style_position.has_value())
        computed_values.set_list_style_position(list_style_position.value());

    // FIXME: The default text decoration color value is `currentcolor`, but since we can't resolve that easily,
    //        we just manually grab the value from `color`. This makes it dependent on `color` being
    //        specified first, so it's far from ideal.
    computed_values.set_text_decoration_color(computed_style.color_or_fallback(CSS::PropertyID::TextDecorationColor, *this, computed_values.color()));
    if (auto maybe_text_decoration_thickness = computed_style.length_percentage(CSS::PropertyID::TextDecorationThickness); maybe_text_decoration_thickness.has_value())
        computed_values.set_text_decoration_thickness(maybe_text_decoration_thickness.release_value());

    computed_values.set_webkit_text_fill_color(computed_style.color_or_fallback(CSS::PropertyID::WebkitTextFillColor, *this, computed_values.color()));

    computed_values.set_text_shadow(computed_style.text_shadow(*this));

    computed_values.set_z_index(computed_style.z_index());
    computed_values.set_opacity(computed_style.opacity());

    if (auto maybe_visibility = computed_style.visibility(); maybe_visibility.has_value())
        computed_values.set_visibility(maybe_visibility.release_value());

    computed_values.set_width(computed_style.size_value(CSS::PropertyID::Width));
    computed_values.set_min_width(computed_style.size_value(CSS::PropertyID::MinWidth));
    computed_values.set_max_width(computed_style.size_value(CSS::PropertyID::MaxWidth));

    computed_values.set_height(computed_style.size_value(CSS::PropertyID::Height));
    computed_values.set_min_height(computed_style.size_value(CSS::PropertyID::MinHeight));
    computed_values.set_max_height(computed_style.size_value(CSS::PropertyID::MaxHeight));

    computed_values.set_inset(computed_style.length_box(CSS::PropertyID::Left, CSS::PropertyID::Top, CSS::PropertyID::Right, CSS::PropertyID::Bottom, CSS::Length::make_auto()));
    computed_values.set_margin(computed_style.length_box(CSS::PropertyID::MarginLeft, CSS::PropertyID::MarginTop, CSS::PropertyID::MarginRight, CSS::PropertyID::MarginBottom, CSS::Length::make_px(0)));
    computed_values.set_padding(computed_style.length_box(CSS::PropertyID::PaddingLeft, CSS::PropertyID::PaddingTop, CSS::PropertyID::PaddingRight, CSS::PropertyID::PaddingBottom, CSS::Length::make_px(0)));

    computed_values.set_box_shadow(computed_style.box_shadow(*this));

    if (auto rotate_value = computed_style.rotate(*this); rotate_value.has_value())
        computed_values.set_rotate(rotate_value.value());

    computed_values.set_transformations(computed_style.transformations());
    if (auto transform_box = computed_style.transform_box(); transform_box.has_value())
        computed_values.set_transform_box(transform_box.value());
    computed_values.set_transform_origin(computed_style.transform_origin());

    auto const& transition_delay_property = computed_style.property(CSS::PropertyID::TransitionDelay);
    if (transition_delay_property.is_time()) {
        auto const& transition_delay = transition_delay_property.as_time();
        computed_values.set_transition_delay(transition_delay.time());
    } else if (transition_delay_property.is_math()) {
        auto const& transition_delay = transition_delay_property.as_math();
        computed_values.set_transition_delay(transition_delay.resolve_time().value());
    }

    auto do_border_style = [&](CSS::BorderData& border, CSS::PropertyID width_property, CSS::PropertyID color_property, CSS::PropertyID style_property) {
        // FIXME: The default border color value is `currentcolor`, but since we can't resolve that easily,
        //        we just manually grab the value from `color`. This makes it dependent on `color` being
        //        specified first, so it's far from ideal.
        border.color = computed_style.color_or_fallback(color_property, *this, computed_values.color());
        border.line_style = computed_style.line_style(style_property).value_or(CSS::LineStyle::None);

        // https://w3c.github.io/csswg-drafts/css-backgrounds/#border-style
        // none
        //    No border. Color and width are ignored (i.e., the border has width 0). Note this means that the initial value of border-image-width will also resolve to zero.
        // hidden
        //    Same as none, but has different behavior in the border conflict resolution rules for border-collapsed tables [CSS2].
        if (border.line_style == CSS::LineStyle::None || border.line_style == CSS::LineStyle::Hidden) {
            border.width = 0;
        } else {
            auto resolve_border_width = [&]() -> CSSPixels {
                auto const& value = computed_style.property(width_property);
                if (value.is_math())
                    return max(CSSPixels { 0 }, value.as_math().resolve_length(*this)->to_px(*this));
                if (value.is_length())
                    return value.as_length().length().to_px(*this);
                if (value.is_keyword()) {
                    // https://www.w3.org/TR/css-backgrounds-3/#valdef-line-width-thin
                    switch (value.to_keyword()) {
                    case CSS::Keyword::Thin:
                        return 1;
                    case CSS::Keyword::Medium:
                        return 3;
                    case CSS::Keyword::Thick:
                        return 5;
                    default:
                        VERIFY_NOT_REACHED();
                    }
                }
                VERIFY_NOT_REACHED();
            };

            border.width = snap_a_length_as_a_border_width(document().page().client().device_pixels_per_css_pixel(), resolve_border_width());
        }
    };

    do_border_style(computed_values.border_left(), CSS::PropertyID::BorderLeftWidth, CSS::PropertyID::BorderLeftColor, CSS::PropertyID::BorderLeftStyle);
    do_border_style(computed_values.border_top(), CSS::PropertyID::BorderTopWidth, CSS::PropertyID::BorderTopColor, CSS::PropertyID::BorderTopStyle);
    do_border_style(computed_values.border_right(), CSS::PropertyID::BorderRightWidth, CSS::PropertyID::BorderRightColor, CSS::PropertyID::BorderRightStyle);
    do_border_style(computed_values.border_bottom(), CSS::PropertyID::BorderBottomWidth, CSS::PropertyID::BorderBottomColor, CSS::PropertyID::BorderBottomStyle);

    if (auto const& outline_color = computed_style.property(CSS::PropertyID::OutlineColor); outline_color.has_color())
        computed_values.set_outline_color(outline_color.to_color(*this));
    if (auto const& outline_offset = computed_style.property(CSS::PropertyID::OutlineOffset); outline_offset.is_length())
        computed_values.set_outline_offset(outline_offset.as_length().length());
    if (auto const& outline_style = computed_style.outline_style(); outline_style.has_value())
        computed_values.set_outline_style(outline_style.value());
    if (auto const& outline_width = computed_style.property(CSS::PropertyID::OutlineWidth); outline_width.is_length())
        computed_values.set_outline_width(outline_width.as_length().length());

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

    if (auto cx_value = computed_style.length_percentage(CSS::PropertyID::Cx); cx_value.has_value())
        computed_values.set_cx(*cx_value);
    if (auto cy_value = computed_style.length_percentage(CSS::PropertyID::Cy); cy_value.has_value())
        computed_values.set_cy(*cy_value);
    if (auto r_value = computed_style.length_percentage(CSS::PropertyID::R); r_value.has_value())
        computed_values.set_r(*r_value);
    if (auto rx_value = computed_style.length_percentage(CSS::PropertyID::Rx); rx_value.has_value())
        computed_values.set_rx(*rx_value);
    if (auto ry_value = computed_style.length_percentage(CSS::PropertyID::Ry); ry_value.has_value())
        computed_values.set_ry(*ry_value);
    if (auto x_value = computed_style.length_percentage(CSS::PropertyID::X); x_value.has_value())
        computed_values.set_x(*x_value);
    if (auto y_value = computed_style.length_percentage(CSS::PropertyID::Y); y_value.has_value())
        computed_values.set_y(*y_value);

    auto const& fill = computed_style.property(CSS::PropertyID::Fill);
    if (fill.has_color())
        computed_values.set_fill(fill.to_color(*this));
    else if (fill.is_url())
        computed_values.set_fill(fill.as_url().url());
    auto const& stroke = computed_style.property(CSS::PropertyID::Stroke);
    if (stroke.has_color())
        computed_values.set_stroke(stroke.to_color(*this));
    else if (stroke.is_url())
        computed_values.set_stroke(stroke.as_url().url());
    if (auto const& stop_color = computed_style.property(CSS::PropertyID::StopColor); stop_color.has_color())
        computed_values.set_stop_color(stop_color.to_color(*this));
    auto const& stroke_width = computed_style.property(CSS::PropertyID::StrokeWidth);
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    if (stroke_width.is_number())
        computed_values.set_stroke_width(CSS::Length::make_px(CSSPixels::nearest_value_for(stroke_width.as_number().number())));
    else if (stroke_width.is_length())
        computed_values.set_stroke_width(stroke_width.as_length().length());
    else if (stroke_width.is_percentage())
        computed_values.set_stroke_width(CSS::LengthPercentage { stroke_width.as_percentage().percentage() });

    if (auto const& mask_image = computed_style.property(CSS::PropertyID::MaskImage); mask_image.is_abstract_image()) {
        auto const& abstract_image = mask_image.as_abstract_image();
        computed_values.set_mask_image(abstract_image);
        const_cast<CSS::AbstractImageStyleValue&>(abstract_image).load_any_resources(document());
    }

    if (auto mask_type = computed_style.mask_type(); mask_type.has_value())
        computed_values.set_mask_type(*mask_type);

    if (auto const& mask = computed_style.property(CSS::PropertyID::Mask); mask.is_url())
        computed_values.set_mask(mask.as_url().url());

    auto const& clip_path = computed_style.property(CSS::PropertyID::ClipPath);
    if (clip_path.is_url())
        computed_values.set_clip_path(clip_path.as_url().url());
    else if (clip_path.is_basic_shape())
        computed_values.set_clip_path(clip_path.as_basic_shape());

    if (auto clip_rule = computed_style.clip_rule(); clip_rule.has_value())
        computed_values.set_clip_rule(*clip_rule);

    if (auto fill_rule = computed_style.fill_rule(); fill_rule.has_value())
        computed_values.set_fill_rule(*fill_rule);

    computed_values.set_fill_opacity(computed_style.fill_opacity());
    if (auto stroke_linecap = computed_style.stroke_linecap(); stroke_linecap.has_value())
        computed_values.set_stroke_linecap(stroke_linecap.value());
    if (auto stroke_linejoin = computed_style.stroke_linejoin(); stroke_linejoin.has_value())
        computed_values.set_stroke_linejoin(stroke_linejoin.value());

    computed_values.set_stroke_miterlimit(computed_style.stroke_miterlimit());

    computed_values.set_stroke_opacity(computed_style.stroke_opacity());
    computed_values.set_stop_opacity(computed_style.stop_opacity());

    if (auto text_anchor = computed_style.text_anchor(); text_anchor.has_value())
        computed_values.set_text_anchor(*text_anchor);

    if (auto const& column_count = computed_style.property(CSS::PropertyID::ColumnCount); column_count.is_integer())
        computed_values.set_column_count(CSS::ColumnCount::make_integer(column_count.as_integer().integer()));

    if (auto column_span = computed_style.column_span(); column_span.has_value())
        computed_values.set_column_span(column_span.value());

    computed_values.set_column_width(computed_style.size_value(CSS::PropertyID::ColumnWidth));

    computed_values.set_column_gap(computed_style.gap_value(CSS::PropertyID::ColumnGap));
    computed_values.set_row_gap(computed_style.gap_value(CSS::PropertyID::RowGap));

    if (auto border_collapse = computed_style.border_collapse(); border_collapse.has_value())
        computed_values.set_border_collapse(border_collapse.value());

    if (auto table_layout = computed_style.table_layout(); table_layout.has_value())
        computed_values.set_table_layout(table_layout.value());

    auto const& aspect_ratio = computed_style.property(CSS::PropertyID::AspectRatio);
    if (aspect_ratio.is_value_list()) {
        auto const& values_list = aspect_ratio.as_value_list().values();
        if (values_list.size() == 2
            && values_list[0]->is_keyword() && values_list[0]->as_keyword().keyword() == CSS::Keyword::Auto
            && values_list[1]->is_ratio()) {
            computed_values.set_aspect_ratio({ true, values_list[1]->as_ratio().ratio() });
        }
    } else if (aspect_ratio.is_keyword() && aspect_ratio.as_keyword().keyword() == CSS::Keyword::Auto) {
        computed_values.set_aspect_ratio({ true, {} });
    } else if (aspect_ratio.is_ratio()) {
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio
        // If the <ratio> is degenerate, the property instead behaves as auto.
        if (aspect_ratio.as_ratio().ratio().is_degenerate())
            computed_values.set_aspect_ratio({ true, {} });
        else
            computed_values.set_aspect_ratio({ false, aspect_ratio.as_ratio().ratio() });
    }

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

    if (auto object_fit = computed_style.object_fit(); object_fit.has_value())
        computed_values.set_object_fit(object_fit.value());

    computed_values.set_object_position(computed_style.object_position());

    if (auto direction = computed_style.direction(); direction.has_value())
        computed_values.set_direction(direction.value());

    if (auto unicode_bidi = computed_style.unicode_bidi(); unicode_bidi.has_value())
        computed_values.set_unicode_bidi(unicode_bidi.value());

    if (auto scrollbar_width = computed_style.scrollbar_width(); scrollbar_width.has_value())
        computed_values.set_scrollbar_width(scrollbar_width.value());

    if (auto writing_mode = computed_style.writing_mode(); writing_mode.has_value())
        computed_values.set_writing_mode(writing_mode.value());

    propagate_style_to_anonymous_wrappers();
}

void NodeWithStyle::propagate_style_to_anonymous_wrappers()
{
    // Update the style of any anonymous wrappers that inherit from this node.
    // FIXME: This is pretty hackish. It would be nicer if they shared the inherited style
    //        data structure somehow, so this wasn't necessary.

    // If this is a `display:table` box with an anonymous wrapper parent,
    // the parent inherits style from *this* node, not the other way around.
    if (display().is_table_inside() && is<TableWrapper>(parent())) {
        auto& table_wrapper = *static_cast<TableWrapper*>(parent());
        static_cast<CSS::MutableComputedValues&>(static_cast<CSS::ComputedValues&>(const_cast<CSS::ImmutableComputedValues&>(table_wrapper.computed_values()))).inherit_from(computed_values());
        transfer_table_box_computed_values_to_wrapper_computed_values(table_wrapper.mutable_computed_values());
    }

    // Propagate style to all anonymous children (except table wrappers!)
    for_each_child_of_type<NodeWithStyle>([&](NodeWithStyle& child) {
        if (child.is_anonymous() && !is<TableWrapper>(child)) {
            auto& child_computed_values = static_cast<CSS::MutableComputedValues&>(static_cast<CSS::ComputedValues&>(const_cast<CSS::ImmutableComputedValues&>(child.computed_values())));
            child_computed_values.inherit_from(computed_values());
        }
        return IterationDecision::Continue;
    });
}

bool Node::is_root_element() const
{
    if (is_anonymous())
        return false;
    return is<HTML::HTMLHtmlElement>(*dom_node());
}

String Node::debug_description() const
{
    StringBuilder builder;
    builder.append(class_name());
    if (dom_node()) {
        builder.appendff("<{}>", dom_node()->node_name());
        if (dom_node()->is_element()) {
            auto& element = static_cast<DOM::Element const&>(*dom_node());
            if (element.id().has_value())
                builder.appendff("#{}", element.id().value());
            for (auto const& class_name : element.class_names())
                builder.appendff(".{}", class_name);
        }
    } else {
        builder.append("(anonymous)"sv);
    }
    return MUST(builder.to_string());
}

CSS::Display Node::display() const
{
    if (!has_style()) {
        // NOTE: No style means this is dumb text content.
        return CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow);
    }

    return computed_values().display();
}

bool Node::is_inline() const
{
    return display().is_inline_outside();
}

bool Node::is_inline_block() const
{
    auto display = this->display();
    return display.is_inline_outside() && display.is_flow_root_inside();
}

bool Node::is_inline_table() const
{
    auto display = this->display();
    return display.is_inline_outside() && display.is_table_inside();
}

GC::Ref<NodeWithStyle> NodeWithStyle::create_anonymous_wrapper() const
{
    auto wrapper = heap().allocate<BlockContainer>(const_cast<DOM::Document&>(document()), nullptr, computed_values().clone_inherited_values());
    wrapper->mutable_computed_values().set_display(CSS::Display(CSS::DisplayOutside::Block, CSS::DisplayInside::Flow));

    // NOTE: These properties are not inherited, but we still have to propagate them to anonymous wrappers.
    wrapper->mutable_computed_values().set_text_decoration_line(computed_values().text_decoration_line());
    wrapper->mutable_computed_values().set_text_decoration_thickness(computed_values().text_decoration_thickness());
    wrapper->mutable_computed_values().set_text_decoration_color(computed_values().text_decoration_color());
    wrapper->mutable_computed_values().set_text_decoration_style(computed_values().text_decoration_style());
    return *wrapper;
}

void NodeWithStyle::reset_table_box_computed_values_used_by_wrapper_to_init_values()
{
    VERIFY(this->display().is_table_inside());

    auto& mutable_computed_values = this->mutable_computed_values();
    mutable_computed_values.set_position(CSS::InitialValues::position());
    mutable_computed_values.set_float(CSS::InitialValues::float_());
    mutable_computed_values.set_clear(CSS::InitialValues::clear());
    mutable_computed_values.set_inset(CSS::InitialValues::inset());
    mutable_computed_values.set_margin(CSS::InitialValues::margin());
}

void NodeWithStyle::transfer_table_box_computed_values_to_wrapper_computed_values(CSS::ComputedValues& wrapper_computed_values)
{
    // The computed values of properties 'position', 'float', 'margin-*', 'top', 'right', 'bottom', and 'left' on the table element are used on the table wrapper box and not the table box;
    // all other values of non-inheritable properties are used on the table box and not the table wrapper box.
    // (Where the table element's values are not used on the table and table wrapper boxes, the initial values are used instead.)
    auto& mutable_wrapper_computed_values = static_cast<CSS::MutableComputedValues&>(wrapper_computed_values);
    if (display().is_inline_outside())
        mutable_wrapper_computed_values.set_display(CSS::Display::from_short(CSS::Display::Short::InlineBlock));
    else
        mutable_wrapper_computed_values.set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));
    mutable_wrapper_computed_values.set_position(computed_values().position());
    mutable_wrapper_computed_values.set_inset(computed_values().inset());
    mutable_wrapper_computed_values.set_float(computed_values().float_());
    mutable_wrapper_computed_values.set_clear(computed_values().clear());
    mutable_wrapper_computed_values.set_margin(computed_values().margin());
    reset_table_box_computed_values_used_by_wrapper_to_init_values();
}

bool NodeWithStyle::is_body() const
{
    return dom_node() && dom_node() == document().body();
}

static bool overflow_value_makes_box_a_scroll_container(CSS::Overflow overflow)
{
    switch (overflow) {
    case CSS::Overflow::Clip:
    case CSS::Overflow::Visible:
        return false;
    case CSS::Overflow::Auto:
    case CSS::Overflow::Hidden:
    case CSS::Overflow::Scroll:
        return true;
    }
    VERIFY_NOT_REACHED();
}

bool NodeWithStyle::is_scroll_container() const
{
    // NOTE: This isn't in the spec, but we want the viewport to behave like a scroll container.
    if (is_viewport())
        return true;

    return overflow_value_makes_box_a_scroll_container(computed_values().overflow_x())
        || overflow_value_makes_box_a_scroll_container(computed_values().overflow_y());
}

void Node::add_paintable(GC::Ptr<Painting::Paintable> paintable)
{
    if (!paintable)
        return;
    m_paintable.append(*paintable);
}

void Node::clear_paintables()
{
    m_paintable.clear();
}

GC::Ptr<Painting::Paintable> Node::create_paintable() const
{
    return nullptr;
}

bool Node::is_anonymous() const
{
    return m_anonymous;
}

DOM::Node const* Node::dom_node() const
{
    if (m_anonymous)
        return nullptr;
    return m_dom_node.ptr();
}

DOM::Node* Node::dom_node()
{
    if (m_anonymous)
        return nullptr;
    return m_dom_node.ptr();
}

DOM::Element const* Node::pseudo_element_generator() const
{
    VERIFY(m_generated_for != GeneratedFor::NotGenerated);
    return m_pseudo_element_generator.ptr();
}

DOM::Element* Node::pseudo_element_generator()
{
    VERIFY(m_generated_for != GeneratedFor::NotGenerated);
    return m_pseudo_element_generator.ptr();
}

DOM::Document& Node::document()
{
    return m_dom_node->document();
}

DOM::Document const& Node::document() const
{
    return m_dom_node->document();
}

}
