/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontVariant.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/InlineLevelIterator.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

InlineLevelIterator::InlineLevelIterator(Layout::InlineFormattingContext& inline_formatting_context, Layout::LayoutState& layout_state, Layout::BlockContainer const& containing_block, LayoutState::UsedValues const& containing_block_used_values, LayoutMode layout_mode)
    : m_inline_formatting_context(inline_formatting_context)
    , m_layout_state(layout_state)
    , m_containing_block(containing_block)
    , m_containing_block_used_values(containing_block_used_values)
    , m_next_node(containing_block.first_child())
    , m_layout_mode(layout_mode)
{
    skip_to_next();
}

void InlineLevelIterator::enter_node_with_box_model_metrics(Layout::NodeWithStyleAndBoxModelMetrics const& node)
{
    if (!m_extra_leading_metrics.has_value())
        m_extra_leading_metrics = ExtraBoxMetrics {};

    // FIXME: It's really weird that *this* is where we assign box model metrics for these layout nodes..

    auto& used_values = m_layout_state.get_mutable(node);
    auto const& computed_values = node.computed_values();

    used_values.margin_left = computed_values.margin().left().to_px(node, m_containing_block_used_values.content_width());
    used_values.border_left = computed_values.border_left().width;
    used_values.padding_left = computed_values.padding().left().to_px(node, m_containing_block_used_values.content_width());

    used_values.border_top = computed_values.border_top().width;
    used_values.border_bottom = computed_values.border_bottom().width;
    used_values.padding_bottom = computed_values.padding().bottom().to_px(node, m_containing_block_used_values.content_width());
    used_values.padding_top = computed_values.padding().top().to_px(node, m_containing_block_used_values.content_width());

    m_extra_leading_metrics->margin += used_values.margin_left;
    m_extra_leading_metrics->border += used_values.border_left;
    m_extra_leading_metrics->padding += used_values.padding_left;

    // Now's our chance to resolve the inset properties for this node.
    m_inline_formatting_context.compute_inset(node, m_inline_formatting_context.content_box_rect(m_containing_block_used_values).size());

    m_box_model_node_stack.append(node);
}

void InlineLevelIterator::exit_node_with_box_model_metrics()
{
    if (!m_extra_trailing_metrics.has_value())
        m_extra_trailing_metrics = ExtraBoxMetrics {};

    auto& node = m_box_model_node_stack.last();
    auto& used_values = m_layout_state.get_mutable(node);
    auto const& computed_values = node->computed_values();

    used_values.margin_right = computed_values.margin().right().to_px(node, m_containing_block_used_values.content_width());
    used_values.border_right = computed_values.border_right().width;
    used_values.padding_right = computed_values.padding().right().to_px(node, m_containing_block_used_values.content_width());

    m_extra_trailing_metrics->margin += used_values.margin_right;
    m_extra_trailing_metrics->border += used_values.border_right;
    m_extra_trailing_metrics->padding += used_values.padding_right;

    m_box_model_node_stack.take_last();
}

// This is similar to Layout::Node::next_in_pre_order() but will not descend into inline-block nodes.
Layout::Node const* InlineLevelIterator::next_inline_node_in_pre_order(Layout::Node const& current, Layout::Node const* stay_within)
{
    if (current.first_child()
        && current.first_child()->display().is_inline_outside()
        && current.display().is_flow_inside()
        && !current.is_replaced_box()) {
        if (!current.is_box() || !static_cast<Box const&>(current).is_out_of_flow(m_inline_formatting_context))
            return current.first_child();
    }

    Layout::Node const* node = &current;
    Layout::Node const* next = nullptr;
    while (!(next = node->next_sibling())) {
        node = node->parent();

        // If node is the last node on the "box model node stack", pop it off.
        if (!m_box_model_node_stack.is_empty()
            && m_box_model_node_stack.last() == node) {
            exit_node_with_box_model_metrics();
        }
        if (!node || node == stay_within)
            return nullptr;
    }

    // If node is the last node on the "box model node stack", pop it off.
    if (!m_box_model_node_stack.is_empty()
        && m_box_model_node_stack.last() == node) {
        exit_node_with_box_model_metrics();
    }

    return next;
}

void InlineLevelIterator::compute_next()
{
    if (m_next_node == nullptr)
        return;
    do {
        m_next_node = next_inline_node_in_pre_order(*m_next_node, m_containing_block);
        if (m_next_node && m_next_node->is_svg_mask_box()) {
            // NOTE: It is possible to encounter SVGMaskBox nodes while doing layout of formatting context established by <foreignObject> with a mask.
            //       We should skip and let SVGFormattingContext take care of them.
            m_next_node = m_next_node->next_sibling();
        }
    } while (m_next_node && (!m_next_node->is_inline() && !m_next_node->is_out_of_flow(m_inline_formatting_context)));
}

void InlineLevelIterator::skip_to_next()
{
    if (m_next_node
        && is<Layout::NodeWithStyleAndBoxModelMetrics>(*m_next_node)
        && m_next_node->display().is_flow_inside()
        && !m_next_node->is_out_of_flow(m_inline_formatting_context)
        && !m_next_node->is_replaced_box())
        enter_node_with_box_model_metrics(static_cast<Layout::NodeWithStyleAndBoxModelMetrics const&>(*m_next_node));

    m_current_node = m_next_node;
    compute_next();
}

Optional<InlineLevelIterator::Item> InlineLevelIterator::next()
{
    if (m_lookahead_items.is_empty())
        return next_without_lookahead();
    return m_lookahead_items.dequeue();
}

CSSPixels InlineLevelIterator::next_non_whitespace_sequence_width()
{
    CSSPixels next_width = 0;
    for (;;) {
        auto next_item_opt = next_without_lookahead();
        if (!next_item_opt.has_value())
            break;
        m_lookahead_items.enqueue(next_item_opt.release_value());
        auto& next_item = m_lookahead_items.tail();
        if (next_item.type == InlineLevelIterator::Item::Type::ForcedBreak)
            break;
        if (next_item.node->computed_values().white_space() != CSS::WhiteSpace::Nowrap) {
            if (next_item.type != InlineLevelIterator::Item::Type::Text)
                break;
            if (next_item.is_collapsible_whitespace)
                break;
            auto& next_text_node = as<Layout::TextNode>(*(next_item.node));
            auto next_view = next_text_node.text_for_rendering().bytes_as_string_view().substring_view(next_item.offset_in_node, next_item.length_in_node);
            if (next_view.is_whitespace())
                break;
        }
        next_width += next_item.border_box_width();
    }
    return next_width;
}

Gfx::GlyphRun::TextType InlineLevelIterator::resolve_text_direction_from_context()
{
    VERIFY(m_text_node_context.has_value());

    Optional<Gfx::GlyphRun::TextType> next_known_direction;
    for (size_t i = 0;; ++i) {
        auto peek = m_text_node_context->chunk_iterator.peek(i);
        if (!peek.has_value())
            break;
        if (peek->text_type == Gfx::GlyphRun::TextType::Ltr || peek->text_type == Gfx::GlyphRun::TextType::Rtl) {
            next_known_direction = peek->text_type;
            break;
        }
    }

    auto last_known_direction = m_text_node_context->last_known_direction;
    if (last_known_direction.has_value() && next_known_direction.has_value() && *last_known_direction != *next_known_direction) {
        switch (m_containing_block->computed_values().direction()) {
        case CSS::Direction::Ltr:
            return Gfx::GlyphRun::TextType::Ltr;
        case CSS::Direction::Rtl:
            return Gfx::GlyphRun::TextType::Rtl;
        }
    }

    if (last_known_direction.has_value())
        return *last_known_direction;
    if (next_known_direction.has_value())
        return *next_known_direction;

    return Gfx::GlyphRun::TextType::ContextDependent;
}

HashMap<StringView, u8> InlineLevelIterator::shape_features_map() const
{
    HashMap<StringView, u8> features;

    auto& computed_values = m_current_node->computed_values();

    // 6.4 https://drafts.csswg.org/css-fonts/#font-variant-ligatures-prop
    auto ligature_or_null = computed_values.font_variant_ligatures();
    if (ligature_or_null.has_value()) {
        auto ligature = ligature_or_null.release_value();
        if (ligature.none) {
            /* nothing */
        } else {
            switch (ligature.common) {
            case Gfx::FontVariantLigatures::Common::Common:
                // Enables display of common ligatures (OpenType features: liga, clig).
                features.set("liga"sv, 1);
                features.set("clig"sv, 1);
                break;
            case Gfx::FontVariantLigatures::Common::NoCommon:
                // Disables display of common ligatures (OpenType features: liga, clig).
                features.set("liga"sv, 0);
                features.set("clig"sv, 0);
                break;
            case Gfx::FontVariantLigatures::Common::Unset:
                break;
            }

            switch (ligature.discretionary) {
            case Gfx::FontVariantLigatures::Discretionary::Discretionary:
                // Enables display of discretionary ligatures (OpenType feature: dlig).
                features.set("dlig"sv, 1);
                break;
            case Gfx::FontVariantLigatures::Discretionary::NoDiscretionary:
                // Disables display of discretionary ligatures (OpenType feature: dlig).
                features.set("dlig"sv, 0);
                break;
            case Gfx::FontVariantLigatures::Discretionary::Unset:
                break;
            }

            switch (ligature.historical) {
            case Gfx::FontVariantLigatures::Historical::Historical:
                // Enables display of historical ligatures (OpenType feature: hlig).
                features.set("hlig"sv, 1);
                break;
            case Gfx::FontVariantLigatures::Historical::NoHistorical:
                // Disables display of historical ligatures (OpenType feature: hlig).
                features.set("hlig"sv, 0);
                break;
            case Gfx::FontVariantLigatures::Historical::Unset:
                break;
            }

            switch (ligature.contextual) {
            case Gfx::FontVariantLigatures::Contextual::Contextual:
                // Enables display of contextual ligatures (OpenType feature: calt).
                features.set("calt"sv, 1);
                break;
            case Gfx::FontVariantLigatures::Contextual::NoContextual:
                // Disables display of contextual ligatures (OpenType feature: calt).
                features.set("calt"sv, 0);
                break;
            case Gfx::FontVariantLigatures::Contextual::Unset:
                break;
            }
        }
    } else {
        // A value of normal specifies that common default features are enabled, as described in detail in the next section.
        features.set("liga"sv, 1);
        features.set("clig"sv, 1);
    }

    // 6.5 https://drafts.csswg.org/css-fonts/#font-variant-position-prop
    switch (computed_values.font_variant_position()) {
    case CSS::FontVariantPosition::Normal:
        // None of the features listed below are enabled.
        break;
    case CSS::FontVariantPosition::Sub:
        // Enables display of subscripts (OpenType feature: subs).
        features.set("subs"sv, 1);
        break;
    case CSS::FontVariantPosition::Super:
        // Enables display of superscripts (OpenType feature: sups).
        features.set("sups"sv, 1);
        break;
    default:
        break;
    }

    // 6.6 https://drafts.csswg.org/css-fonts/#font-variant-caps-prop
    switch (computed_values.font_variant_caps()) {
    case CSS::FontVariantCaps::Normal:
        // None of the features listed below are enabled.
        break;
    case CSS::FontVariantCaps::SmallCaps:
        // Enables display of small capitals (OpenType feature: smcp). Small-caps glyphs typically use the form of uppercase letters but are reduced to the size of lowercase letters.
        features.set("smcp"sv, 1);
        break;
    case CSS::FontVariantCaps::AllSmallCaps:
        // Enables display of small capitals for both upper and lowercase letters (OpenType features: c2sc, smcp).
        features.set("c2sc"sv, 1);
        features.set("smcp"sv, 1);
        break;
    case CSS::FontVariantCaps::PetiteCaps:
        // Enables display of petite capitals (OpenType feature: pcap).
        features.set("pcap"sv, 1);
        break;
    case CSS::FontVariantCaps::AllPetiteCaps:
        // Enables display of petite capitals for both upper and lowercase letters (OpenType features: c2pc, pcap).
        features.set("c2pc"sv, 1);
        features.set("pcap"sv, 1);
        break;
    case CSS::FontVariantCaps::Unicase:
        // Enables display of mixture of small capitals for uppercase letters with normal lowercase letters (OpenType feature: unic).
        features.set("unic"sv, 1);
        break;
    case CSS::FontVariantCaps::TitlingCaps:
        // Enables display of titling capitals (OpenType feature: titl).
        features.set("titl"sv, 1);
        break;
    default:
        break;
    }

    // 6.7 https://drafts.csswg.org/css-fonts/#font-variant-numeric-prop
    auto numeric_or_null = computed_values.font_variant_numeric();
    if (numeric_or_null.has_value()) {
        auto numeric = numeric_or_null.release_value();
        if (numeric.figure == Gfx::FontVariantNumeric::Figure::Oldstyle) {
            // Enables display of old-style numerals (OpenType feature: onum).
            features.set("onum"sv, 1);
        } else if (numeric.figure == Gfx::FontVariantNumeric::Figure::Lining) {
            // Enables display of lining numerals (OpenType feature: lnum).
            features.set("lnum"sv, 1);
        }

        if (numeric.spacing == Gfx::FontVariantNumeric::Spacing::Proportional) {
            // Enables display of proportional numerals (OpenType feature: pnum).
            features.set("pnum"sv, 1);
        } else if (numeric.spacing == Gfx::FontVariantNumeric::Spacing::Tabular) {
            // Enables display of tabular numerals (OpenType feature: tnum).
            features.set("tnum"sv, 1);
        }

        if (numeric.fraction == Gfx::FontVariantNumeric::Fraction::Diagonal) {
            // Enables display of diagonal fractions (OpenType feature: frac).
            features.set("frac"sv, 1);
        } else if (numeric.fraction == Gfx::FontVariantNumeric::Fraction::Stacked) {
            // Enables display of stacked fractions (OpenType feature: afrc).
            features.set("afrc"sv, 1);
            features.set("afrc"sv, 1);
        }

        if (numeric.ordinal) {
            // Enables display of letter forms used with ordinal numbers (OpenType feature: ordn).
            features.set("ordn"sv, 1);
        }
        if (numeric.slashed_zero) {
            // Enables display of slashed zeros (OpenType feature: zero).
            features.set("zero"sv, 1);
        }
    }

    // 6.10 https://drafts.csswg.org/css-fonts/#font-variant-east-asian-prop
    auto east_asian_or_null = computed_values.font_variant_east_asian();
    if (east_asian_or_null.has_value()) {
        auto east_asian = east_asian_or_null.release_value();
        switch (east_asian.variant) {
        case Gfx::FontVariantEastAsian::Variant::Jis78:
            // Enables display of JIS78 forms (OpenType feature: jp78).
            features.set("jp78"sv, 1);
            break;
        case Gfx::FontVariantEastAsian::Variant::Jis83:
            // Enables display of JIS83 forms (OpenType feature: jp83).
            features.set("jp83"sv, 1);
            break;
        case Gfx::FontVariantEastAsian::Variant::Jis90:
            // Enables display of JIS90 forms (OpenType feature: jp90).
            features.set("jp90"sv, 1);
            break;
        case Gfx::FontVariantEastAsian::Variant::Jis04:
            // Enables display of JIS04 forms (OpenType feature: jp04).
            features.set("jp04"sv, 1);
            break;
        case Gfx::FontVariantEastAsian::Variant::Simplified:
            // Enables display of simplified forms (OpenType feature: smpl).
            features.set("smpl"sv, 1);
            break;
        case Gfx::FontVariantEastAsian::Variant::Traditional:
            // Enables display of traditional forms (OpenType feature: trad).
            features.set("trad"sv, 1);
            break;
        default:
            break;
        }
        switch (east_asian.width) {
        case Gfx::FontVariantEastAsian::Width::FullWidth:
            // Enables display of full-width forms (OpenType feature: fwid).
            features.set("fwid"sv, 1);
            break;
        case Gfx::FontVariantEastAsian::Width::Proportional:
            // Enables display of proportional-width forms (OpenType feature: pwid).
            features.set("pwid"sv, 1);
            break;
        default:
            break;
        }
        if (east_asian.ruby) {
            // Enables display of ruby forms (OpenType feature: ruby).
            features.set("ruby"sv, 1);
        }
    }

    return features;
}

Gfx::ShapeFeatures InlineLevelIterator::create_and_merge_font_features() const
{
    HashMap<StringView, u8> merged_features;
    auto& computed_values = m_inline_formatting_context.containing_block().computed_values();

    // https://www.w3.org/TR/css-fonts-3/#feature-precedence

    // FIXME 1. Font features enabled by default, including features required for a given script.

    // FIXME 2. If the font is defined via an @font-face rule, the font features implied by the font-feature-settings descriptor in the @font-face rule.

    // 3. Font features implied by the value of the ‘font-variant’ property, the related ‘font-variant’ subproperties and any other CSS property that uses OpenType features (e.g. the ‘font-kerning’ property).
    for (auto& it : shape_features_map()) {
        merged_features.set(it.key, it.value);
    }

    // FIXME 4. Feature settings determined by properties other than ‘font-variant’ or ‘font-feature-settings’. For example, setting a non-default value for the ‘letter-spacing’ property disables common ligatures.

    // 5. Font features implied by the value of ‘font-feature-settings’ property.
    CSS::CalculationResolutionContext calculation_context { .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(*m_current_node.ptr()) };
    auto font_feature_settings = computed_values.font_feature_settings();
    if (font_feature_settings.has_value()) {
        auto const& feature_settings = font_feature_settings.value();
        for (auto const& [key, feature_value] : feature_settings) {
            merged_features.set(key, feature_value.resolved(calculation_context).value_or(0));
        }
    }

    Gfx::ShapeFeatures shape_features;
    shape_features.ensure_capacity(merged_features.size());

    for (auto& it : merged_features) {
        shape_features.append({ { it.key[0], it.key[1], it.key[2], it.key[3] }, static_cast<u32>(it.value) });
    }

    return shape_features;
}

Optional<InlineLevelIterator::Item> InlineLevelIterator::next_without_lookahead()
{
    if (!m_current_node)
        return {};

    if (is<Layout::TextNode>(*m_current_node)) {
        auto& text_node = static_cast<Layout::TextNode const&>(*m_current_node);

        if (!m_text_node_context.has_value())
            enter_text_node(text_node);

        auto chunk_opt = m_text_node_context->chunk_iterator.next();
        if (!chunk_opt.has_value()) {
            m_text_node_context = {};
            skip_to_next();
            return next_without_lookahead();
        }

        if (!m_text_node_context->chunk_iterator.peek(0).has_value())
            m_text_node_context->is_last_chunk = true;

        auto& chunk = chunk_opt.value();
        auto text_type = chunk.text_type;
        if (text_type == Gfx::GlyphRun::TextType::Ltr || text_type == Gfx::GlyphRun::TextType::Rtl)
            m_text_node_context->last_known_direction = text_type;

        if (m_text_node_context->do_respect_linebreaks && chunk.has_breaking_newline) {
            m_text_node_context->is_last_chunk = true;
            if (chunk.is_all_whitespace)
                text_type = Gfx::GlyphRun::TextType::EndPadding;
        }

        if (text_type == Gfx::GlyphRun::TextType::ContextDependent)
            text_type = resolve_text_direction_from_context();

        if (m_text_node_context->do_respect_linebreaks && chunk.has_breaking_newline) {
            return Item {
                .type = Item::Type::ForcedBreak,
            };
        }

        CSS::CalculationResolutionContext calculation_context { .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(text_node) };
        auto letter_spacing = text_node.computed_values().letter_spacing().resolved(calculation_context).map([&](auto& it) { return it.to_px(text_node); }).value_or(0);
        auto word_spacing = text_node.computed_values().word_spacing().resolved(calculation_context).map([&](auto& it) { return it.to_px(text_node); }).value_or(0);

        auto x = 0.0f;
        if (chunk.has_breaking_tab) {
            CSSPixels accumulated_width;

            // make sure to account for any fragments that take up a portion of the measured tab stop distance
            auto fragments = m_containing_block_used_values.line_boxes.last().fragments();
            for (auto const& frag : fragments) {
                accumulated_width += frag.width();
            }

            // https://drafts.csswg.org/css-text/#tab-size-property
            auto tab_size = text_node.computed_values().tab_size();
            CSSPixels tab_width;
            tab_width = tab_size.visit(
                [&](CSS::LengthOrCalculated const& t) -> CSSPixels {
                    return t.resolved(calculation_context)
                        .map([&](auto& it) { return it.to_px(text_node); })
                        .value_or(0);
                },
                [&](CSS::NumberOrCalculated const& n) -> CSSPixels {
                    auto tab_number = n.resolved(calculation_context).value_or(0);

                    return CSSPixels::nearest_value_for(tab_number * (chunk.font->glyph_width(' ') + word_spacing.to_float() + letter_spacing.to_float()));
                });

            // https://drafts.csswg.org/css-text/#white-space-phase-2
            // if fragments have added to the width, calculate the net distance to the next tab stop, otherwise the shift will just be the tab width
            auto tab_stop_dist = accumulated_width > 0 ? (ceil((accumulated_width / tab_width)) * tab_width) - accumulated_width : tab_width;
            auto ch_width = chunk.font->glyph_width('0');

            // If this distance is less than 0.5ch, then the subsequent tab stop is used instead
            if (tab_stop_dist < ch_width * 0.5)
                tab_stop_dist += tab_width;

            // account for consecutive tabs
            auto num_of_tabs = 0;
            for (auto code_point : chunk.view) {
                if (code_point != '\t')
                    break;
                num_of_tabs++;
            }
            tab_stop_dist = tab_stop_dist * num_of_tabs;

            // remove tabs, we don't want to render them when we shape the text
            chunk.view = chunk.view.substring_view(num_of_tabs);
            x = tab_stop_dist.to_float();
        }

        auto shape_features = create_and_merge_font_features();
        auto glyph_run = Gfx::shape_text({ x, 0 }, letter_spacing.to_float(), chunk.view, chunk.font, text_type, shape_features);

        CSSPixels chunk_width = CSSPixels::nearest_value_for(glyph_run->width());

        // NOTE: We never consider `content: ""` to be collapsible whitespace.
        bool is_generated_empty_string = text_node.is_generated() && chunk.length == 0;

        Item item {
            .type = Item::Type::Text,
            .node = &text_node,
            .glyph_run = move(glyph_run),
            .offset_in_node = chunk.start,
            .length_in_node = chunk.length,
            .width = chunk_width,
            .is_collapsible_whitespace = m_text_node_context->do_collapse && chunk.is_all_whitespace && !is_generated_empty_string,
        };

        add_extra_box_model_metrics_to_item(item, m_text_node_context->is_first_chunk, m_text_node_context->is_last_chunk);
        return item;
    }

    if (m_current_node->is_absolutely_positioned()) {
        auto& node = *m_current_node;
        skip_to_next();
        return Item {
            .type = Item::Type::AbsolutelyPositionedElement,
            .node = &node,
        };
    }

    if (m_current_node->is_floating()) {
        auto& node = *m_current_node;
        skip_to_next();
        return Item {
            .type = Item::Type::FloatingElement,
            .node = &node,
        };
    }

    if (is<Layout::BreakNode>(*m_current_node)) {
        auto& node = *m_current_node;
        skip_to_next();
        return Item {
            .type = Item::Type::ForcedBreak,
            .node = &node,
        };
    }

    if (is<Layout::ListItemMarkerBox>(*m_current_node)) {
        skip_to_next();
        return next_without_lookahead();
    }

    if (!is<Layout::Box>(*m_current_node)) {
        skip_to_next();
        return next_without_lookahead();
    }

    if (is<Layout::ReplacedBox>(*m_current_node)) {
        auto& replaced_box = static_cast<Layout::ReplacedBox const&>(*m_current_node);
        // FIXME: This const_cast is gross.
        const_cast<Layout::ReplacedBox&>(replaced_box).prepare_for_replaced_layout();
    }

    auto& box = as<Layout::Box>(*m_current_node);
    auto& box_state = m_layout_state.get(box);
    m_inline_formatting_context.dimension_box_on_line(box, m_layout_mode);

    skip_to_next();
    auto item = Item {
        .type = Item::Type::Element,
        .node = &box,
        .offset_in_node = 0,
        .length_in_node = 0,
        .width = box_state.content_width(),
        .padding_start = box_state.padding_left,
        .padding_end = box_state.padding_right,
        .border_start = box_state.border_left,
        .border_end = box_state.border_right,
        .margin_start = box_state.margin_left,
        .margin_end = box_state.margin_right,
    };
    add_extra_box_model_metrics_to_item(item, true, true);
    return item;
}

void InlineLevelIterator::enter_text_node(Layout::TextNode const& text_node)
{
    bool do_collapse = true;
    bool do_wrap_lines = true;
    bool do_respect_linebreaks = false;

    if (text_node.computed_values().white_space() == CSS::WhiteSpace::Nowrap) {
        do_collapse = true;
        do_wrap_lines = false;
        do_respect_linebreaks = false;
    } else if (text_node.computed_values().white_space() == CSS::WhiteSpace::Pre) {
        do_collapse = false;
        do_wrap_lines = false;
        do_respect_linebreaks = true;
    } else if (text_node.computed_values().white_space() == CSS::WhiteSpace::PreLine) {
        do_collapse = true;
        do_wrap_lines = true;
        do_respect_linebreaks = true;
    } else if (text_node.computed_values().white_space() == CSS::WhiteSpace::PreWrap) {
        do_collapse = false;
        do_wrap_lines = true;
        do_respect_linebreaks = true;
    }

    if (text_node.dom_node().is_editable() && !text_node.dom_node().is_uninteresting_whitespace_node())
        do_collapse = false;

    m_text_node_context = TextNodeContext {
        .do_collapse = do_collapse,
        .do_wrap_lines = do_wrap_lines,
        .do_respect_linebreaks = do_respect_linebreaks,
        .is_first_chunk = true,
        .is_last_chunk = false,
        .chunk_iterator = TextNode::ChunkIterator { text_node, do_wrap_lines, do_respect_linebreaks },
    };
}

void InlineLevelIterator::add_extra_box_model_metrics_to_item(Item& item, bool add_leading_metrics, bool add_trailing_metrics)
{
    if (add_leading_metrics && m_extra_leading_metrics.has_value()) {
        item.margin_start += m_extra_leading_metrics->margin;
        item.border_start += m_extra_leading_metrics->border;
        item.padding_start += m_extra_leading_metrics->padding;
        m_extra_leading_metrics = {};
    }

    if (add_trailing_metrics && m_extra_trailing_metrics.has_value()) {
        item.margin_end += m_extra_trailing_metrics->margin;
        item.border_end += m_extra_trailing_metrics->border;
        item.padding_end += m_extra_trailing_metrics->padding;
        m_extra_trailing_metrics = {};
    }
}

}
