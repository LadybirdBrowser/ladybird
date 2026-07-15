/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Layout/AvailableSpace.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/LayoutState.h>
#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/SVGForeignObjectPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Layout {

LayoutState::LayoutState(NodeWithStyle const& subtree_root, Purpose purpose)
    : m_subtree_root(&subtree_root)
    , m_purpose(purpose)
{
}

LayoutState::~LayoutState()
{
}

static CSSPixelRect united_fragment_rect_for_line_box(LineBox const& line_box)
{
    CSSPixelRect rect;
    bool saw_fragment = false;
    for (auto const& fragment : line_box.fragments()) {
        auto fragment_rect = CSSPixelRect { fragment.offset(), fragment.size() };
        if (saw_fragment)
            rect.unite(fragment_rect);
        else
            rect = fragment_rect;
        saw_fragment = true;
    }

    auto writing_mode = line_box.fragments().first().writing_mode();
    if (writing_mode == CSS::WritingMode::HorizontalTb) {
        rect.set_y(line_box.bottom() - line_box.height());
        rect.set_height(line_box.height());
    }

    return rect;
}

static CSSPixelRect rect_for_line_box(LineBox const& line_box, CSSPixels containing_block_content_width)
{
    if (line_box.fragments().is_empty())
        return { 0, line_box.bottom() - line_box.height(), containing_block_content_width, line_box.height() };
    return united_fragment_rect_for_line_box(line_box);
}

void LayoutState::ensure_capacity(u32 node_count)
{
    m_used_values_store.ensure_capacity(node_count);
}

LayoutState::UsedValues& LayoutState::get_mutable(NodeWithStyle const& node)
{
    auto* used_values = m_used_values_store.get(node.layout_index());
    if (!used_values) {
        dbgln("LayoutState::get_mutable: no used values for {}; boxes must be created before their state is read", node.debug_description());
        VERIFY_NOT_REACHED();
    }
    return *used_values;
}

LayoutState::UsedValues const& LayoutState::get(NodeWithStyle const& node) const
{
    auto const* used_values = m_used_values_store.get(node.layout_index());
    if (!used_values) {
        dbgln("LayoutState::get: no used values for {}; boxes must be created before their state is read", node.debug_description());
        VERIFY_NOT_REACHED();
    }
    return *used_values;
}

LayoutState::UsedValues& LayoutState::create(NodeWithStyle const& node, Optional<CSSPixels> percentage_basis_width, Optional<CSSPixels> percentage_basis_height)
{
    auto index = node.layout_index();
    if (m_used_values_store.get(index)) {
        dbgln("LayoutState::create: used values for {} already exist", node.debug_description());
        VERIFY_NOT_REACHED();
    }

    VERIFY(!m_subtree_root || m_subtree_root == &node || m_subtree_root->is_inclusive_ancestor_of(node));

    auto& used_values = m_used_values_store.allocate(index);
    used_values.set_node(node, percentage_basis_width, percentage_basis_height);

    if (auto const* list_item_box = as_if<ListItemBox>(node); list_item_box && list_item_box->marker()) {
        auto const& marker = *list_item_box->marker();
        if (!m_used_values_store.get(marker.layout_index())) {
            auto& marker_used_values = m_used_values_store.allocate(marker.layout_index());
            marker_used_values.set_node(marker,
                used_values.has_definite_width() ? Optional<CSSPixels> { used_values.content_width() } : Optional<CSSPixels> {},
                used_values.has_definite_height() ? Optional<CSSPixels> { used_values.content_height() } : Optional<CSSPixels> {});
        }
    }

    return used_values;
}

LayoutState::UsedValues& LayoutState::populate_from_paintable(NodeWithStyle const& node, Painting::Paintable const& paintable)
{
    VERIFY(m_subtree_root);
    auto index = node.layout_index();

    // NOTE: We skip set_node() here since it performs size resolution that requires percentage bases,
    //       and materialize_from_paintable() overwrites all computed sizes immediately after.
    auto& used_values = m_used_values_store.allocate(index);
    used_values.m_node = &node;
    used_values.materialize_from_paintable(paintable);
    return used_values;
}

LayoutState::UsedValues const* LayoutState::try_get(NodeWithStyle const& node) const
{
    return m_used_values_store.get(node.layout_index());
}

LayoutState::UsedValues* LayoutState::try_get_mutable(NodeWithStyle const& node)
{
    return m_used_values_store.get(node.layout_index());
}

LayoutState::UsedValues const* LayoutState::try_get(Node const& node) const
{
    auto* node_with_style = as_if<NodeWithStyle>(node);
    if (!node_with_style)
        return nullptr;
    return try_get(*node_with_style);
}

CSSPixelPoint LayoutState::cumulative_offset(UsedValues const& used_values) const
{
    if (used_values.m_cumulative_offset.has_value())
        return *used_values.m_cumulative_offset;
    if (auto const* containing_block = used_values.node().containing_block())
        return cumulative_offset(get(*containing_block)) + used_values.content_offset();
    return used_values.content_offset();
}

struct InlineAncestorChainRelativeOffset {
    CSSPixelPoint offset;
    bool found_fragmented_inline_node { false };
};

// Accumulates relative position insets from a chain of inline-flow ancestors, starting at first_ancestor
// and walking up until stop_at or the first ancestor that is not inline-flow.
static InlineAncestorChainRelativeOffset accumulated_relative_insets_from_inline_ancestor_chain(Node const* first_ancestor, Node const* stop_at)
{
    InlineAncestorChainRelativeOffset result;
    for (auto const* ancestor = first_ancestor; ancestor && ancestor != stop_at; ancestor = ancestor->parent()) {
        if (!is<Layout::NodeWithStyleAndBoxModelMetrics>(*ancestor))
            break;
        auto const& ancestor_with_style = static_cast<Layout::NodeWithStyleAndBoxModelMetrics const&>(*ancestor);
        if (!ancestor_with_style.display().is_inline_outside() || !ancestor_with_style.display().is_flow_inside())
            break;
        result.found_fragmented_inline_node |= ancestor->is_fragmented_inline();
        if (ancestor_with_style.computed_values().position() == CSS::Positioning::Relative) {
            VERIFY(ancestor->paintable());
            auto const& ancestor_paintable_box = *ancestor->paintable();
            auto const& inset = ancestor_paintable_box.box_model().inset;
            result.offset.translate_by(inset.left, inset.top);
        }
    }
    return result;
}

void LayoutState::resolve_relative_positions()
{
    // This function resolves relative position offsets contributed by inline-flow ancestor chains, which
    // apply to fragments, to inline box pieces, and to block-level boxes interrupting an inline
    // (block-in-inline). It runs *after* the paint tree has been constructed, so it modifies
    // paintable node, fragment & piece offsets directly.
    m_used_values_store.for_each([&](UsedValues& used_values) {
        auto& node = const_cast<NodeWithStyle&>(used_values.node());

        // Nodes outside the committed subtree (materialized containing blocks) keep their
        // already-resolved offsets.
        if (m_subtree_root && !m_subtree_root->is_inclusive_ancestor_of(node))
            return;

        if (auto const* box = as_if<Box>(node); box && box->is_in_flow() && box->display().is_block_outside()) {
            auto accumulated = accumulated_relative_insets_from_inline_ancestor_chain(box->parent(), box->containing_block());
            if (accumulated.found_fragmented_inline_node) {
                if (auto paintable = node.paintable())
                    paintable->set_offset(paintable->offset().translated(accumulated.offset));
            }
        }

        auto* paintable_with_lines = as_if<Painting::PaintableWithLines>(node.paintable().ptr());
        if (!paintable_with_lines)
            return;

        for (auto& fragment : paintable_with_lines->fragments()) {
            auto accumulated = accumulated_relative_insets_from_inline_ancestor_chain(fragment.layout_node().parent(), nullptr);
            if (!accumulated.offset.is_zero())
                fragment.set_offset(fragment.offset().translated(accumulated.offset));
        }

        HashMap<Layout::Node const*, CSSPixelPoint> accumulated_offset_per_node;
        for (auto& piece : paintable_with_lines->inline_box_pieces()) {
            auto const* piece_node = piece.node.ptr();
            if (!piece_node)
                continue;
            // The chain starts at the piece's own node: a relative inline box shifts its own pieces.
            auto offset = accumulated_offset_per_node.ensure(piece_node, [&] {
                return accumulated_relative_insets_from_inline_ancestor_chain(piece_node, nullptr).offset;
            });
            if (!offset.is_zero())
                piece.border_box_rect.translate_by(offset);
        }
    });
}

static void build_paint_tree(Node& node, Painting::Paintable* parent_paintable = nullptr, Painting::Paintable* insert_before_paintable = nullptr)
{
    Painting::Paintable* paintable_for_children = nullptr;
    if (auto paintable = node.paintable()) {
        if (parent_paintable && !paintable->forms_unconnected_subtree()) {
            VERIFY(!paintable->parent());
            parent_paintable->insert_before(*paintable, insert_before_paintable);
        }
        paintable->set_dom_node(node.dom_node());
        if (node.dom_node())
            node.dom_node()->set_paintable(paintable);
        paintable_for_children = paintable.ptr();
    } else if (node.is_fragmented_inline()) {
        // An inline box without a paintable (it was never laid out) must not orphan its
        // descendants' paintables; pass the nearest ancestor paintable through. Other
        // paintable-less nodes (e.g. non-rendered SVG subtrees) keep their descendants
        // disconnected on purpose.
        paintable_for_children = parent_paintable;
    }

    for (auto child = node.first_child(); child; child = child->next_sibling())
        build_paint_tree(*child, paintable_for_children);
}

void LayoutState::resolve_paintable_containing_blocks(Node& root)
{
    root.for_each_in_inclusive_subtree([](Node& node) {
        auto* paintable = node.paintable_ptr();
        if (!paintable)
            return TraversalDecision::Continue;

        auto* containing_block = node.containing_block();
        paintable->set_containing_block(containing_block ? containing_block->paintable_ptr() : nullptr);
        return TraversalDecision::Continue;
    });
}

void LayoutState::commit(Box& root)
{
    if (!root.is_viewport()) {
        if (auto existing_paintable = root.paintable()) {
            commit(root, *existing_paintable);
            return;
        }
    }

    commit_used_values_and_build_paint_tree(root, nullptr, nullptr);
}

void LayoutState::commit(Box& root, Painting::Paintable& paintable_to_replace)
{
    // Splice the rebuilt paint subtree exactly where the replaced paintable stands, because
    // paint and hit-test order between siblings with equal stacking follow paintable tree
    // order.
    NonnullRefPtr<Painting::Paintable> protect_replaced_paintable = paintable_to_replace;
    RefPtr<Painting::Paintable> parent_paintable = paintable_to_replace.parent();
    RefPtr<Painting::Paintable> insert_before_paintable = paintable_to_replace.next_sibling();
    if (parent_paintable)
        parent_paintable->remove_child(paintable_to_replace);

    commit_used_values_and_build_paint_tree(root, move(parent_paintable), move(insert_before_paintable));
}

void LayoutState::commit_used_values_and_build_paint_tree(Box& root, RefPtr<Painting::Paintable> parent_paintable, RefPtr<Painting::Paintable> insert_before_paintable)
{
    // Cache existing paintables before clearing.
    HashMap<Node const*, NonnullRefPtr<Painting::Paintable>> paintable_cache;
    root.for_each_in_inclusive_subtree([&](Node& node) {
        if (auto paintable = node.paintable())
            paintable_cache.set(&node, *paintable);
        return TraversalDecision::Continue;
    });

    // Go through the layout tree and detach all paintables. The layout tree should only point to the new paintable tree
    // which we're about to build.
    root.for_each_in_inclusive_subtree([](Node& node) {
        node.clear_paintable();
        return TraversalDecision::Continue;
    });

    // After this point, we should have a clean slate to build the new paint tree.

    Vector<Node*> fragmented_inline_nodes;
    root.for_each_in_inclusive_subtree([&](Node& node) {
        if (auto* dom_node = node.dom_node())
            dom_node->clear_paintable();
        if (node.is_fragmented_inline() && node.dom_node())
            fragmented_inline_nodes.append(&node);
        return TraversalDecision::Continue;
    });

    auto transfer_box_model_metrics = [](Painting::BoxModelMetrics& box_model, UsedValues const& used_values) {
        box_model.inset = { used_values.inset_top, used_values.inset_right, used_values.inset_bottom, used_values.inset_left };
        box_model.padding = { used_values.padding_top, used_values.padding_right, used_values.padding_bottom, used_values.padding_left };
        box_model.border = { used_values.border_top, used_values.border_right, used_values.border_bottom, used_values.border_left };
        box_model.margin = { used_values.margin_top, used_values.margin_right, used_values.margin_bottom, used_values.margin_left };
    };

    Vector<NonnullRefPtr<Painting::PaintableWithLines>> blocks_with_inline_box_pieces;

    m_used_values_store.for_each([&](UsedValues& used_values) {
        auto& node = used_values.node();

        if (m_subtree_root && !m_subtree_root->is_inclusive_ancestor_of(node))
            return;

        // Clearing on absence keeps saved inputs from a previous pass from surviving a pass
        // that no longer laid the box out as absolutely positioned.
        if (auto* layout_box = as_if<Box>(const_cast<NodeWithStyle&>(node))) {
            if (auto const* abspos_layout_inputs = used_values.abspos_layout_inputs())
                layout_box->set_saved_abspos_layout_inputs(*abspos_layout_inputs);
            else
                layout_box->clear_saved_abspos_layout_inputs();
        }

        RefPtr<Painting::Paintable> paintable;

        // Try to reuse cached paintable for Box nodes
        if (auto cached = paintable_cache.get(&node); cached.has_value()) {
            auto cached_paintable = cached.value();
            cached_paintable->reset_for_relayout();
            paintable = cached_paintable;
        }

        // Fall back to creating new if no reusable paintable
        if (!paintable)
            paintable = node.create_paintable();

        node.set_paintable(paintable);

        // For boxes, transfer all the state needed for painting.
        if (auto* paintable_box = paintable.ptr()) {
            transfer_box_model_metrics(paintable_box->box_model(), used_values);

            paintable_box->set_offset(used_values.content_offset());
            paintable_box->set_content_size(used_values.content_width(), used_values.content_height());
            if (used_values.override_borders_data().has_value())
                paintable_box->set_override_borders_data(used_values.override_borders_data().value());
            if (used_values.table_cell_coordinates().has_value())
                paintable_box->set_table_cell_coordinates(used_values.table_cell_coordinates().value());

            if (auto* paintable_with_lines = as_if<Painting::PaintableWithLines>(*paintable_box)) {
                auto& line_boxes = used_values.line_boxes;
                Vector<Painting::LineRecord> lines;
                lines.ensure_capacity(line_boxes.size());
                for (size_t line_index = 0; line_index < line_boxes.size(); ++line_index) {
                    auto const& line_box = line_boxes[line_index];

                    auto first_fragment_index = paintable_with_lines->fragments().size();
                    for (auto const& fragment : line_box.fragments()) {
                        if (fragment.is_fully_truncated())
                            continue;
                        paintable_with_lines->add_fragment(fragment, static_cast<u32>(line_index));
                    }

                    lines.append({
                        .rect = rect_for_line_box(line_box, used_values.content_width()),
                        .fragment_count = static_cast<u32>(paintable_with_lines->fragments().size() - first_fragment_index),
                    });
                }
                paintable_with_lines->set_lines(move(lines));
                paintable_with_lines->set_inline_box_pieces(move(used_values.inline_box_pieces));
                if (!paintable_with_lines->inline_box_pieces().is_empty())
                    blocks_with_inline_box_pieces.append(*paintable_with_lines);

                // Piece fragment ranges were counted against the same skip-fully-truncated
                // fragment stream during inline layout; a divergence would let piece
                // consumers read out of bounds.
                for (auto const& piece : paintable_with_lines->inline_box_pieces())
                    VERIFY(piece.first_fragment_index + piece.fragment_count <= paintable_with_lines->fragments().size());
            }

            if (auto* svg_graphics_paintable = as_if<Painting::SVGGraphicsPaintable>(paintable.ptr());
                svg_graphics_paintable && used_values.computed_svg_transforms().has_value()) {
                svg_graphics_paintable->set_computed_transforms(*used_values.computed_svg_transforms());
            }
            if (auto* svg_foreign_object_paintable = as_if<Painting::SVGForeignObjectPaintable>(paintable.ptr());
                svg_foreign_object_paintable && used_values.computed_svg_transforms().has_value()) {
                svg_foreign_object_paintable->set_computed_transforms(*used_values.computed_svg_transforms());
            }
            if (auto* svg_svg_paintable = as_if<Painting::SVGSVGPaintable>(paintable.ptr());
                svg_svg_paintable && used_values.computed_svg_transforms().has_value()) {
                svg_svg_paintable->set_computed_transforms(*used_values.computed_svg_transforms());
            }

            if (auto* svg_path_paintable = as_if<Painting::SVGPathPaintable>(paintable.ptr())) {
                if (auto* path = used_values.computed_svg_path())
                    svg_path_paintable->set_computed_path(move(*path));
            }

            if (node.display().is_grid_inside()) {
                paintable_box->set_used_values_for_grid_template_columns(used_values.grid_template_columns());
                paintable_box->set_used_values_for_grid_template_rows(used_values.grid_template_rows());
                paintable_box->set_grid_layout_data(used_values.take_grid_layout_data());
            }

            if (node.display().is_flex_inside()) {
                paintable_box->set_flex_layout_data(used_values.take_flex_layout_data());
            }
        }
    });

    // Inline boxes that never went through inline layout (so they have no used values) still
    // need a paintable so DOM geometry queries have something to answer from.
    for (auto* fragmented_inline_node : fragmented_inline_nodes) {
        if (!fragmented_inline_node->paintable())
            fragmented_inline_node->set_paintable(fragmented_inline_node->create_paintable());
    }

    // Resolve relative positions for regular boxes (not line box fragments):
    m_used_values_store.for_each([&](UsedValues& used_values) {
        auto& node = const_cast<NodeWithStyle&>(used_values.node());

        if (!node.is_box() || node.is_fragmented_inline())
            return;

        auto paintable_ref = node.paintable();
        auto& paintable = *paintable_ref;
        CSSPixelPoint offset;

        if (used_values.containing_line_box_fragment.has_value()) {
            // Atomic inline case:
            // We know that `node` is an atomic inline because `containing_line_box_fragments` refers to the
            // line box fragment in the parent block container that contains it.
            auto const& containing_line_box_fragment = used_values.containing_line_box_fragment.value();
            auto const& containing_block = *node.containing_block();
            auto const& containing_block_used_values = get(containing_block);

            // The fragment has the final offset for the atomic inline, so we just need to copy it from there.
            // However, line box post-processing may remove fragments after we record this coordinate.
            if (containing_line_box_fragment.line_box_index < containing_block_used_values.line_boxes.size()) {
                auto const& line_box = containing_block_used_values.line_boxes[containing_line_box_fragment.line_box_index];
                paintable.set_containing_line_box_index(containing_line_box_fragment.line_box_index);
                if (containing_line_box_fragment.fragment_index < line_box.fragments().size())
                    offset = line_box.fragments()[containing_line_box_fragment.fragment_index].offset();
                else
                    offset = used_values.content_offset();
            } else {
                offset = used_values.content_offset();
            }
        } else {
            // Not an atomic inline, much simpler case.
            offset = used_values.content_offset();
        }

        // Apply relative position inset if appropriate.
        if (node.computed_values().position() == CSS::Positioning::Relative && is<NodeWithStyleAndBoxModelMetrics>(node)) {
            auto const& inset = paintable.box_model().inset;
            offset.translate_by(inset.left, inset.top);
        }
        paintable.set_offset(offset);
    });

    build_paint_tree(root, parent_paintable.ptr(), insert_before_paintable.ptr());
    resolve_paintable_containing_blocks(root);

    resolve_relative_positions();

    // Piece rects are final only now that relative positions are resolved.
    for (auto const& paintable_with_lines : blocks_with_inline_box_pieces)
        paintable_with_lines->assign_inline_box_geometry();
}

LayoutState::UsedValues& LayoutState::UsedValues::operator=(UsedValues const& other)
{
    if (this == &other)
        return *this;

    m_content_offset = other.m_content_offset;
    width_constraint = other.width_constraint;
    height_constraint = other.height_constraint;
    margin_left = other.margin_left;
    margin_right = other.margin_right;
    margin_top = other.margin_top;
    margin_bottom = other.margin_bottom;
    border_left = other.border_left;
    border_right = other.border_right;
    border_top = other.border_top;
    border_bottom = other.border_bottom;
    padding_left = other.padding_left;
    padding_right = other.padding_right;
    padding_top = other.padding_top;
    padding_bottom = other.padding_bottom;
    inset_left = other.inset_left;
    inset_right = other.inset_right;
    inset_top = other.inset_top;
    inset_bottom = other.inset_bottom;
    line_boxes = other.line_boxes;
    inline_box_pieces = other.inline_box_pieces;
    first_baseline = other.first_baseline;
    last_baseline = other.last_baseline;
    containing_line_box_fragment = other.containing_line_box_fragment;

    m_node = other.m_node;
    m_cumulative_offset = other.m_cumulative_offset;
    m_content_width = other.m_content_width;
    m_content_height = other.m_content_height;
    m_has_definite_width = other.m_has_definite_width;
    m_has_definite_height = other.m_has_definite_height;
    if (other.m_rare)
        m_rare = make<RareData>(*other.m_rare);
    else
        m_rare = nullptr;

    return *this;
}

void LayoutState::UsedValues::set_node(NodeWithStyle const& node, Optional<CSSPixels> percentage_basis_width, Optional<CSSPixels> percentage_basis_height)
{
    m_node = &node;

    // NOTE: In the code below, we decide if `node` has definite width and/or height.
    //       This attempts to cover all the *general* cases where CSS considers sizes to be definite.
    //       If `node` has definite values for min/max-width or min/max-height and a definite
    //       preferred size in the same axis, we clamp the preferred size here as well.
    //
    //       There are additional cases where CSS considers values to be definite. We model all of
    //       those by having our engine consider sizes to be definite *once they are assigned to
    //       the UsedValues by calling set_content_width() or set_content_height().

    auto const& computed_values = node.computed_values();

    auto containing_block_size_for_axis = [&](bool width) {
        return width ? percentage_basis_width.value_or(0) : percentage_basis_height.value_or(0);
    };

    auto adjust_for_box_sizing = [&](CSSPixels unadjusted_pixels, CSS::Size const& computed_size, bool width) -> CSSPixels {
        // box-sizing: content-box and/or automatic size don't require any adjustment.
        if (computed_values.box_sizing() == CSS::BoxSizing::ContentBox || computed_size.is_auto())
            return unadjusted_pixels;

        // box-sizing: border-box requires us to subtract the relevant border and padding from the size.
        CSSPixels border_and_padding;

        if (width) {
            border_and_padding = computed_values.border_left().width
                + computed_values.padding().left().to_px_or_zero(percentage_basis_width.value_or(0))
                + computed_values.border_right().width
                + computed_values.padding().right().to_px_or_zero(percentage_basis_width.value_or(0));
        } else {
            border_and_padding = computed_values.border_top().width
                + computed_values.padding().top().to_px_or_zero(percentage_basis_width.value_or(0))
                + computed_values.border_bottom().width
                + computed_values.padding().bottom().to_px_or_zero(percentage_basis_width.value_or(0));
        }

        return unadjusted_pixels - border_and_padding;
    };

    auto is_definite_size = [&](CSS::Size const& size, CSSPixels& resolved_definite_size, bool width) {
        // A size that can be determined without performing layout; that is,
        // a <length>,
        // a measure of text (without consideration of line-wrapping),
        // a size of the initial containing block,
        // or a <percentage> or other formula (such as the “stretch-fit” sizing of non-replaced blocks [CSS2]) that is resolved solely against definite sizes.

        auto containing_block_has_definite_size = width ? percentage_basis_width.has_value() : percentage_basis_height.has_value();

        if (size.is_auto()) {
            // NOTE: The width of a non-flex-item block is considered definite if it's auto and the containing block has definite width.
            //       This models the "stretch-fit" case from the css-sizing-3 definition of definite sizes quoted above.
            //       It explicitly only covers non-replaced blocks; the automatic width of a replaced box is
            //       content-based and thus not definite before layout.
            if (width
                && !node.is_replaced_box()
                && !node.is_floating()
                && !node.is_absolutely_positioned()
                && node.display().is_block_outside()
                && node.parent()
                && !node.parent()->is_floating()
                && (node.parent()->display().is_flow_root_inside()
                    || node.parent()->display().is_flow_inside())) {
                if (containing_block_has_definite_size) {
                    CSSPixels available_width = containing_block_size_for_axis(true);
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

        if (size.is_length_percentage()) {
            if (size.contains_percentage()) {
                if (!containing_block_has_definite_size)
                    return false;
                auto containing_block_size_as_length = containing_block_size_for_axis(width);
                resolved_definite_size = clamp_to_max_dimension_value(adjust_for_box_sizing(size.to_px(containing_block_size_as_length), size, width));
                return true;
            }

            resolved_definite_size = clamp_to_max_dimension_value(adjust_for_box_sizing(size.to_px(CSSPixels { 0 }), size, width));
            return true;
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

void LayoutState::UsedValues::materialize_from_paintable(Painting::Paintable const& paintable)
{
    auto const& box_model = paintable.box_model();

    set_content_width(paintable.content_width());
    set_content_height(paintable.content_height());
    m_has_definite_width = true;
    m_has_definite_height = true;

    m_content_offset = paintable.offset();
    m_cumulative_offset = paintable.absolute_rect().location();

    margin_left = box_model.margin.left;
    margin_right = box_model.margin.right;
    margin_top = box_model.margin.top;
    margin_bottom = box_model.margin.bottom;

    padding_left = box_model.padding.left;
    padding_right = box_model.padding.right;
    padding_top = box_model.padding.top;
    padding_bottom = box_model.padding.bottom;

    border_left = box_model.border.left;
    border_right = box_model.border.right;
    border_top = box_model.border.top;
    border_bottom = box_model.border.bottom;

    inset_left = box_model.inset.left;
    inset_right = box_model.inset.right;
    inset_top = box_model.inset.top;
    inset_bottom = box_model.inset.bottom;

    if (auto const* svg_graphics_paintable = as_if<Painting::SVGGraphicsPaintable>(paintable))
        set_computed_svg_transforms(svg_graphics_paintable->computed_transforms());
    if (auto const* svg_foreign_object_paintable = as_if<Painting::SVGForeignObjectPaintable>(paintable))
        set_computed_svg_transforms(svg_foreign_object_paintable->computed_transforms());
    if (auto const* svg_svg_paintable = as_if<Painting::SVGSVGPaintable>(paintable))
        set_computed_svg_transforms(svg_svg_paintable->computed_transforms());
}

void LayoutState::UsedValues::set_content_width(CSSPixels width)
{
    VERIFY(!is_placed());
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
    VERIFY(!is_placed());
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

void LayoutState::register_contained_abspos_child(Box const& target, Box const& child, StaticPositionRect const& static_position_rect)
{
    auto& children = m_contained_abspos_children.ensure(&target);
    // Entries are inserted in layout index order so consumption follows document order.
    size_t insertion_index = children.size();
    for (size_t i = 0; i < children.size(); ++i) {
        // Every box is laid out at most once per state, so it can only be registered once.
        VERIFY(children[i].box != &child);
        if (insertion_index == children.size() && child.layout_index() < children[i].box->layout_index())
            insertion_index = i;
    }
    children.insert(insertion_index, { &child, static_position_rect });
}

Optional<LayoutState::ContainedAbsposChild> LayoutState::take_next_contained_abspos_child(Box const& target)
{
    auto it = m_contained_abspos_children.find(&target);
    if (it == m_contained_abspos_children.end())
        return {};
    auto child = it->value.take_first();
    if (it->value.is_empty())
        m_contained_abspos_children.remove(it);
    return child;
}

}
