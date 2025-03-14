/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Layout/AvailableSpace.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/LayoutState.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Layout {

LayoutState::~LayoutState()
{
}

LayoutState::UsedValues& LayoutState::get_mutable(NodeWithStyle const& node)
{
    if (auto* used_values = used_values_per_layout_node.get(node).value_or(nullptr))
        return *used_values;

    auto const* containing_block_used_values = node.is_viewport() ? nullptr : &get(*node.containing_block());

    auto new_used_values = adopt_own(*new UsedValues);
    auto* new_used_values_ptr = new_used_values.ptr();
    new_used_values->set_node(const_cast<NodeWithStyle&>(node), containing_block_used_values);
    used_values_per_layout_node.set(node, move(new_used_values));
    return *new_used_values_ptr;
}

LayoutState::UsedValues const& LayoutState::get(NodeWithStyle const& node) const
{
    if (auto const* used_values = used_values_per_layout_node.get(node).value_or(nullptr))
        return *used_values;

    auto const* containing_block_used_values = node.is_viewport() ? nullptr : &get(*node.containing_block());

    auto new_used_values = adopt_own(*new UsedValues);
    auto* new_used_values_ptr = new_used_values.ptr();
    new_used_values->set_node(const_cast<NodeWithStyle&>(node), containing_block_used_values);
    const_cast<LayoutState*>(this)->used_values_per_layout_node.set(node, move(new_used_values));
    return *new_used_values_ptr;
}

// https://www.w3.org/TR/css-overflow-3/#scrollable-overflow
static CSSPixelRect measure_scrollable_overflow(Box const& box)
{
    if (!box.paintable_box())
        return {};

    auto& paintable_box = const_cast<Painting::PaintableBox&>(*box.paintable_box());

    if (paintable_box.scrollable_overflow_rect().has_value())
        return paintable_box.scrollable_overflow_rect().value();

    // The scrollable overflow area is the union of:

    // - The scroll container’s own padding box.
    auto scrollable_overflow_rect = paintable_box.absolute_padding_box_rect();

    // - All line boxes directly contained by the scroll container.
    if (is<Painting::PaintableWithLines>(box.first_paintable())) {
        for (auto const& fragment : static_cast<Painting::PaintableWithLines const&>(*box.first_paintable()).fragments())
            scrollable_overflow_rect.unite(fragment.absolute_rect());
    }

    auto content_overflow_rect = scrollable_overflow_rect;

    // - The border boxes of all boxes for which it is the containing block
    //   and whose border boxes are positioned not wholly in the negative scrollable overflow region,
    //   FIXME: accounting for transforms by projecting each box onto the plane of the element that establishes its 3D rendering context. [CSS3-TRANSFORMS]
    if (!box.children_are_inline()) {
        box.for_each_in_subtree_of_type<Box>([&box, &scrollable_overflow_rect, &content_overflow_rect](Box const& child) {
            if (!child.paintable_box())
                return TraversalDecision::Continue;

            if (child.containing_block() != &box)
                return TraversalDecision::Continue;

            auto child_border_box = child.paintable_box()->absolute_border_box_rect();

            // Border boxes with zero area do not affect the scrollable overflow area.
            if (child_border_box.is_empty())
                return TraversalDecision::Continue;

            // NOTE: Here we check that the child is not wholly in the negative scrollable overflow region.
            if (child_border_box.bottom() < 0 || child_border_box.right() < 0)
                return TraversalDecision::Continue;

            scrollable_overflow_rect.unite(child_border_box);
            content_overflow_rect.unite(child_border_box);

            // - The scrollable overflow areas of all of the above boxes
            //   (including zero-area boxes and accounting for transforms as described above),
            //   provided they themselves have overflow: visible (i.e. do not themselves trap the overflow)
            //   and that scrollable overflow is not already clipped (e.g. by the clip property or the contain property).
            if (child.computed_values().overflow_x() == CSS::Overflow::Visible || child.computed_values().overflow_y() == CSS::Overflow::Visible) {
                auto child_scrollable_overflow = measure_scrollable_overflow(child);
                if (child.computed_values().overflow_x() == CSS::Overflow::Visible)
                    scrollable_overflow_rect.unite_horizontally(child_scrollable_overflow);
                if (child.computed_values().overflow_y() == CSS::Overflow::Visible)
                    scrollable_overflow_rect.unite_vertically(child_scrollable_overflow);
            }

            return TraversalDecision::Continue;
        });
    }

    // FIXME: - The margin areas of grid item and flex item boxes for which the box establishes a containing block.

    // - Additional padding added to the end-side of the scrollable overflow rectangle as necessary
    //   to enable a scroll position that satisfies the requirements of place-content: end alignment.
    auto has_scrollable_overflow = !paintable_box.absolute_padding_box_rect().contains(scrollable_overflow_rect);
    if (has_scrollable_overflow) {
        scrollable_overflow_rect.set_height(max(scrollable_overflow_rect.height(), content_overflow_rect.height() + paintable_box.box_model().padding.bottom));
    }

    paintable_box.set_overflow_data(Painting::PaintableBox::OverflowData {
        .scrollable_overflow_rect = scrollable_overflow_rect,
        .has_scrollable_overflow = has_scrollable_overflow,
    });

    return scrollable_overflow_rect;
}

void LayoutState::resolve_relative_positions()
{
    // This function resolves relative position offsets of fragments that belong to inline paintables.
    // It runs *after* the paint tree has been constructed, so it modifies paintable node & fragment offsets directly.
    for (auto& it : used_values_per_layout_node) {
        auto& used_values = *it.value;
        auto& node = const_cast<NodeWithStyle&>(used_values.node());

        for (auto& paintable : node.paintables()) {
            if (!(is<Painting::PaintableWithLines>(paintable) && is<Layout::InlineNode>(paintable.layout_node())))
                continue;

            auto const& inline_paintable = static_cast<Painting::PaintableWithLines&>(paintable);
            for (auto& fragment : inline_paintable.fragments()) {
                auto const& fragment_node = fragment.layout_node();
                if (!is<Layout::NodeWithStyleAndBoxModelMetrics>(*fragment_node.parent()))
                    continue;
                // Collect effective relative position offset from inline-flow parent chain.
                CSSPixelPoint offset;
                for (auto* ancestor = fragment_node.parent(); ancestor; ancestor = ancestor->parent()) {
                    if (!is<Layout::NodeWithStyleAndBoxModelMetrics>(*ancestor))
                        break;
                    if (!ancestor->display().is_inline_outside() || !ancestor->display().is_flow_inside())
                        break;
                    if (ancestor->computed_values().position() == CSS::Positioning::Relative) {
                        VERIFY(ancestor->first_paintable());
                        auto const& ancestor_node = as<Painting::PaintableBox>(*ancestor->first_paintable());
                        auto const& inset = ancestor_node.box_model().inset;
                        offset.translate_by(inset.left, inset.top);
                    }
                }
                const_cast<Painting::PaintableFragment&>(fragment).set_offset(fragment.offset().translated(offset));
            }
        }
    }
}

static void build_paint_tree(Node& node, Painting::Paintable* parent_paintable = nullptr)
{
    for (auto& paintable : node.paintables()) {
        if (parent_paintable && !paintable.forms_unconnected_subtree()) {
            VERIFY(!paintable.parent());
            parent_paintable->append_child(paintable);
        }
        paintable.set_dom_node(node.dom_node());
        if (node.dom_node())
            node.dom_node()->set_paintable(paintable);
    }
    for (auto* child = node.first_child(); child; child = child->next_sibling()) {
        build_paint_tree(*child, node.first_paintable());
    }
}

void LayoutState::commit(Box& root)
{
    // NOTE: In case this is a relayout of an existing tree, we start by detaching the old paint tree
    //       from the layout tree. This is done to ensure that we don't end up with any old-tree pointers
    //       when text paintables shift around in the tree.
    root.for_each_in_inclusive_subtree([&](Layout::Node& node) {
        node.clear_paintables();
        return TraversalDecision::Continue;
    });

    HashTable<Layout::InlineNode*> inline_nodes;

    root.document().for_each_shadow_including_inclusive_descendant([&](DOM::Node& node) {
        node.clear_paintable();
        if (node.layout_node() && is<InlineNode>(node.layout_node())) {
            // Inline nodes might have a continuation chain; add all inline nodes that are part of it.
            for (GC::Ptr inline_node = static_cast<NodeWithStyleAndBoxModelMetrics*>(node.layout_node());
                inline_node; inline_node = inline_node->continuation_of_node()) {
                if (is<InlineNode>(*inline_node))
                    inline_nodes.set(static_cast<InlineNode*>(inline_node.ptr()));
            }
        }
        return TraversalDecision::Continue;
    });

    HashTable<Layout::TextNode*> text_nodes;
    HashTable<Painting::PaintableWithLines*> inline_node_paintables;

    auto transfer_box_model_metrics = [&](Painting::BoxModelMetrics& box_model, UsedValues const& used_values) {
        box_model.inset = { used_values.inset_top, used_values.inset_right, used_values.inset_bottom, used_values.inset_left };
        box_model.padding = { used_values.padding_top, used_values.padding_right, used_values.padding_bottom, used_values.padding_left };
        box_model.border = { used_values.border_top, used_values.border_right, used_values.border_bottom, used_values.border_left };
        box_model.margin = { used_values.margin_top, used_values.margin_right, used_values.margin_bottom, used_values.margin_left };
    };

    auto try_to_relocate_fragment_in_inline_node = [&](auto& fragment, size_t line_index) -> bool {
        for (auto const* parent = fragment.layout_node().parent(); parent; parent = parent->parent()) {
            if (is<InlineNode>(*parent)) {
                auto& inline_node = const_cast<InlineNode&>(static_cast<InlineNode const&>(*parent));
                auto line_paintable = inline_node.create_paintable_for_line_with_index(line_index);
                line_paintable->add_fragment(fragment);
                if (auto const* used_values = used_values_per_layout_node.get(inline_node).value_or(nullptr))
                    transfer_box_model_metrics(line_paintable->box_model(), *used_values);
                if (!inline_node_paintables.contains(line_paintable.ptr())) {
                    inline_node_paintables.set(line_paintable.ptr());
                    inline_node.add_paintable(line_paintable);
                }
                return true;
            }
        }
        return false;
    };

    for (auto& it : used_values_per_layout_node) {
        auto& used_values = *it.value;
        auto& node = const_cast<NodeWithStyle&>(used_values.node());

        auto paintable = node.create_paintable();
        node.add_paintable(paintable);

        // For boxes, transfer all the state needed for painting.
        if (paintable && is<Painting::PaintableBox>(*paintable)) {
            auto& paintable_box = static_cast<Painting::PaintableBox&>(*paintable);

            transfer_box_model_metrics(paintable_box.box_model(), used_values);

            paintable_box.set_offset(used_values.offset);
            paintable_box.set_content_size(used_values.content_width(), used_values.content_height());
            if (used_values.override_borders_data().has_value()) {
                paintable_box.set_override_borders_data(used_values.override_borders_data().value());
            }
            if (used_values.table_cell_coordinates().has_value()) {
                paintable_box.set_table_cell_coordinates(used_values.table_cell_coordinates().value());
            }

            if (is<Painting::PaintableWithLines>(paintable_box)) {
                auto& paintable_with_lines = static_cast<Painting::PaintableWithLines&>(paintable_box);
                for (size_t line_index = 0; line_index < used_values.line_boxes.size(); ++line_index) {
                    auto& line_box = used_values.line_boxes[line_index];
                    for (auto& fragment : line_box.fragments()) {
                        if (fragment.layout_node().is_text_node())
                            text_nodes.set(static_cast<Layout::TextNode*>(const_cast<Layout::Node*>(&fragment.layout_node())));
                        auto did_relocate_fragment = try_to_relocate_fragment_in_inline_node(fragment, line_index);
                        if (!did_relocate_fragment) {
                            paintable_with_lines.add_fragment(fragment);
                        }
                    }
                }
            }

            if (used_values.computed_svg_transforms().has_value() && is<Painting::SVGGraphicsPaintable>(paintable_box)) {
                auto& svg_graphics_paintable = static_cast<Painting::SVGGraphicsPaintable&>(paintable_box);
                svg_graphics_paintable.set_computed_transforms(*used_values.computed_svg_transforms());
            }

            if (used_values.computed_svg_path().has_value() && is<Painting::SVGPathPaintable>(paintable_box)) {
                auto& svg_geometry_paintable = static_cast<Painting::SVGPathPaintable&>(paintable_box);
                svg_geometry_paintable.set_computed_path(move(*used_values.computed_svg_path()));
            }

            if (node.display().is_grid_inside()) {
                paintable_box.set_used_values_for_grid_template_columns(used_values.grid_template_columns());
                paintable_box.set_used_values_for_grid_template_rows(used_values.grid_template_rows());
            }
        }
    }

    // Create paintables for inline nodes without fragments to make possible querying their geometry.
    for (auto& inline_node : inline_nodes) {
        auto* paintable = inline_node->first_paintable();
        if (paintable)
            continue;
        auto line_paintable = inline_node->create_paintable_for_line_with_index(0);
        inline_node->add_paintable(line_paintable);
        inline_node_paintables.set(line_paintable.ptr());
        if (auto const* used_values = used_values_per_layout_node.get(*inline_node).value_or(nullptr))
            transfer_box_model_metrics(line_paintable->box_model(), *used_values);
    }

    // Resolve relative positions for regular boxes (not line box fragments):
    // NOTE: This needs to occur before fragments are transferred into the corresponding inline paintables, because
    //       after this transfer, the containing_line_box_fragment will no longer be valid.
    for (auto& it : used_values_per_layout_node) {
        auto& used_values = *it.value;
        auto& node = const_cast<NodeWithStyle&>(used_values.node());

        if (!node.is_box())
            continue;

        auto& paintable = as<Painting::PaintableBox>(*node.first_paintable());
        CSSPixelPoint offset;

        if (used_values.containing_line_box_fragment.has_value()) {
            // Atomic inline case:
            // We know that `node` is an atomic inline because `containing_line_box_fragments` refers to the
            // line box fragment in the parent block container that contains it.
            auto const& containing_line_box_fragment = used_values.containing_line_box_fragment.value();
            auto const& containing_block = *node.containing_block();
            auto const& containing_block_used_values = get(containing_block);
            auto const& fragment = containing_block_used_values.line_boxes[containing_line_box_fragment.line_box_index].fragments()[containing_line_box_fragment.fragment_index];

            // The fragment has the final offset for the atomic inline, so we just need to copy it from there.
            offset = fragment.offset();
        } else {
            // Not an atomic inline, much simpler case.
            offset = used_values.offset;
        }
        // Apply relative position inset if appropriate.
        if (node.computed_values().position() == CSS::Positioning::Relative && is<NodeWithStyleAndBoxModelMetrics>(node)) {
            auto const& inset = paintable.box_model().inset;
            offset.translate_by(inset.left, inset.top);
        }
        paintable.set_offset(offset);
    }

    for (auto* text_node : text_nodes) {
        text_node->add_paintable(text_node->create_paintable());
    }

    build_paint_tree(root);

    resolve_relative_positions();

    // Measure size of paintables created for inline nodes.
    for (auto& paintable_with_lines : inline_node_paintables) {
        if (!is<InlineNode>(paintable_with_lines->layout_node())) {
            continue;
        }

        Optional<CSSPixelPoint> offset;
        CSSPixelSize size;
        auto line_index = paintable_with_lines->line_index();
        paintable_with_lines->for_each_in_inclusive_subtree_of_type<Painting::PaintableWithLines>([&offset, &size, &line_index](auto& paintable) {
            if (paintable.line_index() == line_index) {
                auto const& fragments = paintable.fragments();
                if (!fragments.is_empty()) {
                    if (!offset.has_value() || (fragments.first().offset().x() < offset.value().x())) {
                        offset = fragments.first().offset();
                    }
                }
                for (auto const& fragment : fragments) {
                    // FIXME: Padding and margin of nested inlines not included in fragment width
                    size.set_width(size.width() + fragment.width());
                }
            }
            return TraversalDecision::Continue;
        });

        if (offset.has_value()) {
            if (!paintable_with_lines->fragments().is_empty()) {
                offset.value().set_y(paintable_with_lines->fragments().first().offset().y());
            }
            // FIXME: If this paintable does not have any fragment we do no know the y offset. It should be where text should
            // start if there had been any for this node. Pick y offset of the leftmost fragment in the inclusive subtree in the meantime.
            paintable_with_lines->set_offset(offset.value());
        }

        if (!paintable_with_lines->fragments().is_empty()) {
            for (auto const& fragment : paintable_with_lines->fragments()) {
                size.set_height(max(size.height(), fragment.height()));
            }
        } else {
            size.set_height(paintable_with_lines->layout_node().computed_values().line_height());
        }

        paintable_with_lines->set_content_size(size.width(), size.height());
    }

    // Measure overflow in scroll containers.
    for (auto& it : used_values_per_layout_node) {
        auto& used_values = *it.value;
        if (!used_values.node().is_box())
            continue;
        auto const& box = static_cast<Layout::Box const&>(used_values.node());
        measure_scrollable_overflow(box);

        // The scroll offset can become invalid if the scrollable overflow rectangle has changed after layout.
        // For example, if the scroll container has been scrolled to the very end and is then resized to become larger
        // (scrollable overflow rect become smaller), the scroll offset would be out of bounds.
        auto& paintable_box = const_cast<Painting::PaintableBox&>(*box.paintable_box());
        if (!paintable_box.scroll_offset().is_zero())
            paintable_box.set_scroll_offset(paintable_box.scroll_offset());
    }

    for (auto& it : used_values_per_layout_node) {
        auto& used_values = *it.value;
        auto& node = used_values.node();
        for (auto& paintable : node.paintables()) {
            Painting::PaintableBox* paintable_box = nullptr;
            if (is<Painting::PaintableBox>(paintable))
                paintable_box = &static_cast<Painting::PaintableBox&>(paintable);

            if (!paintable_box)
                continue;

            if (node.is_sticky_position()) {
                // https://drafts.csswg.org/css-position/#insets
                // For sticky positioned boxes, the inset is instead relative to the relevant scrollport’s size. Negative values are allowed.

                auto sticky_insets = make<Painting::PaintableBox::StickyInsets>();
                auto const& inset = node.computed_values().inset();

                auto const* nearest_scrollable_ancestor = paintable_box->nearest_scrollable_ancestor();
                CSSPixelSize scrollport_size;
                if (nearest_scrollable_ancestor) {
                    scrollport_size = nearest_scrollable_ancestor->absolute_rect().size();
                }

                if (!inset.top().is_auto()) {
                    sticky_insets->top = inset.top().to_px(node, scrollport_size.height());
                }
                if (!inset.right().is_auto()) {
                    sticky_insets->right = inset.right().to_px(node, scrollport_size.width());
                }
                if (!inset.bottom().is_auto()) {
                    sticky_insets->bottom = inset.bottom().to_px(node, scrollport_size.height());
                }
                if (!inset.left().is_auto()) {
                    sticky_insets->left = inset.left().to_px(node, scrollport_size.width());
                }
                paintable_box->set_sticky_insets(move(sticky_insets));
            }
        }
    }
}

void LayoutState::UsedValues::set_node(NodeWithStyle& node, UsedValues const* containing_block_used_values)
{
    m_node = &node;
    m_containing_block_used_values = containing_block_used_values;

    // NOTE: In the code below, we decide if `node` has definite width and/or height.
    //       This attempts to cover all the *general* cases where CSS considers sizes to be definite.
    //       If `node` has definite values for min/max-width or min/max-height and a definite
    //       preferred size in the same axis, we clamp the preferred size here as well.
    //
    //       There are additional cases where CSS considers values to be definite. We model all of
    //       those by having our engine consider sizes to be definite *once they are assigned to
    //       the UsedValues by calling set_content_width() or set_content_height().

    auto const& computed_values = node.computed_values();

    auto adjust_for_box_sizing = [&](CSSPixels unadjusted_pixels, CSS::Size const& computed_size, bool width) -> CSSPixels {
        // box-sizing: content-box and/or automatic size don't require any adjustment.
        if (computed_values.box_sizing() == CSS::BoxSizing::ContentBox || computed_size.is_auto())
            return unadjusted_pixels;

        // box-sizing: border-box requires us to subtract the relevant border and padding from the size.
        CSSPixels border_and_padding;

        if (width) {
            border_and_padding = computed_values.border_left().width
                + computed_values.padding().left().to_px(*m_node, containing_block_used_values->content_width())
                + computed_values.border_right().width
                + computed_values.padding().right().to_px(*m_node, containing_block_used_values->content_width());
        } else {
            border_and_padding = computed_values.border_top().width
                + computed_values.padding().top().to_px(*m_node, containing_block_used_values->content_width())
                + computed_values.border_bottom().width
                + computed_values.padding().bottom().to_px(*m_node, containing_block_used_values->content_width());
        }

        return unadjusted_pixels - border_and_padding;
    };

    auto is_definite_size = [&](CSS::Size const& size, CSSPixels& resolved_definite_size, bool width) {
        // A size that can be determined without performing layout; that is,
        // a <length>,
        // a measure of text (without consideration of line-wrapping),
        // a size of the initial containing block,
        // or a <percentage> or other formula (such as the “stretch-fit” sizing of non-replaced blocks [CSS2]) that is resolved solely against definite sizes.

        auto containing_block_has_definite_size = containing_block_used_values ? (width ? containing_block_used_values->has_definite_width() : containing_block_used_values->has_definite_height()) : false;

        if (size.is_auto()) {
            // NOTE: The width of a non-flex-item block is considered definite if it's auto and the containing block has definite width.
            if (width
                && !node.is_floating()
                && !node.is_absolutely_positioned()
                && node.display().is_block_outside()
                && node.parent()
                && !node.parent()->is_floating()
                && (node.parent()->display().is_flow_root_inside()
                    || node.parent()->display().is_flow_inside())) {
                if (containing_block_has_definite_size) {
                    CSSPixels available_width = containing_block_used_values->content_width();
                    resolved_definite_size = clamp_to_max_dimension_value(
                        available_width
                        - margin_left
                        - margin_right
                        - padding_left
                        - padding_right
                        - border_left
                        - border_right);
                    return true;
                }
                return false;
            }
            return false;
        }

        if (size.is_calculated()) {
            CSS::CalculationResolutionContext context {
                .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(node),
            };
            if (size.calculated().contains_percentage()) {
                if (!containing_block_has_definite_size)
                    return false;
                auto containing_block_size_as_length = width ? containing_block_used_values->content_width() : containing_block_used_values->content_height();
                context.percentage_basis = CSS::Length::make_px(containing_block_size_as_length);
            }
            resolved_definite_size = clamp_to_max_dimension_value(adjust_for_box_sizing(size.calculated().resolve_length(context)->to_px(node), size, width));
            return true;
        }

        if (size.is_length()) {
            VERIFY(!size.is_auto()); // This should have been covered by the Size::is_auto() branch above.
            resolved_definite_size = clamp_to_max_dimension_value(adjust_for_box_sizing(size.length().to_px(node), size, width));
            return true;
        }
        if (size.is_percentage()) {
            if (containing_block_has_definite_size) {
                auto containing_block_size = width ? containing_block_used_values->content_width() : containing_block_used_values->content_height();
                resolved_definite_size = clamp_to_max_dimension_value(adjust_for_box_sizing(containing_block_size.scaled(size.percentage().as_fraction()), size, width));
                return true;
            }
            return false;
        }
        return false;
    };

    CSSPixels min_width = 0;
    bool has_definite_min_width = is_definite_size(computed_values.min_width(), min_width, true);
    CSSPixels max_width = 0;
    bool has_definite_max_width = is_definite_size(computed_values.max_width(), max_width, true);

    CSSPixels min_height = 0;
    bool has_definite_min_height = is_definite_size(computed_values.min_height(), min_height, false);
    CSSPixels max_height = 0;
    bool has_definite_max_height = is_definite_size(computed_values.max_height(), max_height, false);

    m_has_definite_width = is_definite_size(computed_values.width(), m_content_width, true);
    m_has_definite_height = is_definite_size(computed_values.height(), m_content_height, false);

    // For boxes with a preferred aspect ratio and one definite size, we can infer the other size
    // and consider it definite since this did not require performing layout.
    if (is<Box>(node)) {
        auto const& box = static_cast<Box const&>(node);
        if (auto aspect_ratio = box.preferred_aspect_ratio(); aspect_ratio.has_value()) {
            if (m_has_definite_width && m_has_definite_height) {
                // Both width and height are definite.
            } else if (m_has_definite_width) {
                m_content_height = clamp_to_max_dimension_value(m_content_width / *aspect_ratio);
                m_has_definite_height = true;
            } else if (m_has_definite_height) {
                m_content_width = clamp_to_max_dimension_value(m_content_height * *aspect_ratio);
                m_has_definite_width = true;
            }
        }
    }

    if (m_has_definite_width) {
        if (has_definite_min_width)
            m_content_width = clamp_to_max_dimension_value(max(min_width, m_content_width));
        if (has_definite_max_width)
            m_content_width = clamp_to_max_dimension_value(min(max_width, m_content_width));
    }

    if (m_has_definite_height) {
        if (has_definite_min_height)
            m_content_height = clamp_to_max_dimension_value(max(min_height, m_content_height));
        if (has_definite_max_height)
            m_content_height = clamp_to_max_dimension_value(min(max_height, m_content_height));
    }
}

void LayoutState::UsedValues::set_content_width(CSSPixels width)
{
    if (width < 0) {
        // Negative widths are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Layout calculated a negative width for {}: {}", m_node->debug_description(), width);
        width = 0;
    }
    m_content_width = clamp_to_max_dimension_value(width);
    // FIXME: We should not do this! Definiteness of widths should be determined early,
    //        and not changed later (except for some special cases in flex layout..)
    m_has_definite_width = true;
}

void LayoutState::UsedValues::set_content_height(CSSPixels height)
{
    if (height < 0) {
        // Negative heights are not allowed in CSS. We have a bug somewhere! Clamp to 0 to avoid doing too much damage.
        dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Layout calculated a negative height for {}: {}", m_node->debug_description(), height);
        height = 0;
    }
    m_content_height = clamp_to_max_dimension_value(height);
}

AvailableSize LayoutState::UsedValues::available_width_inside() const
{
    if (width_constraint == SizeConstraint::MinContent)
        return AvailableSize::make_min_content();
    if (width_constraint == SizeConstraint::MaxContent)
        return AvailableSize::make_max_content();
    if (has_definite_width())
        return AvailableSize::make_definite(m_content_width);
    return AvailableSize::make_indefinite();
}

AvailableSize LayoutState::UsedValues::available_height_inside() const
{
    if (height_constraint == SizeConstraint::MinContent)
        return AvailableSize::make_min_content();
    if (height_constraint == SizeConstraint::MaxContent)
        return AvailableSize::make_max_content();
    if (has_definite_height())
        return AvailableSize::make_definite(m_content_height);
    return AvailableSize::make_indefinite();
}

AvailableSpace LayoutState::UsedValues::available_inner_space_or_constraints_from(AvailableSpace const& outer_space) const
{
    auto inner_width = available_width_inside();
    auto inner_height = available_height_inside();

    if (inner_width.is_indefinite() && outer_space.width.is_intrinsic_sizing_constraint())
        inner_width = outer_space.width;
    if (inner_height.is_indefinite() && outer_space.height.is_intrinsic_sizing_constraint())
        inner_height = outer_space.height;
    return AvailableSpace(inner_width, inner_height);
}

void LayoutState::UsedValues::set_indefinite_content_width()
{
    m_has_definite_width = false;
}

void LayoutState::UsedValues::set_indefinite_content_height()
{
    m_has_definite_height = false;
}
}
