/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LineBox.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>

namespace Web::Layout {

enum class SizeConstraint {
    None,
    MinContent,
    MaxContent,
};

class AvailableSize;
class AvailableSpace;

// https://www.w3.org/TR/css-position-3/#static-position-rectangle
struct StaticPositionRect {
    enum class Alignment {
        Start,
        Center,
        End,
    };

    CSSPixelRect rect;
    Alignment horizontal_alignment { Alignment::Start };
    Alignment vertical_alignment { Alignment::Start };

    CSSPixelPoint aligned_position_for_box_with_size(CSSPixelSize const& size) const
    {
        CSSPixelPoint position = rect.location();
        if (horizontal_alignment == Alignment::Center)
            position.set_x(position.x() + (rect.width() - size.width()) / 2);
        else if (horizontal_alignment == Alignment::End)
            position.set_x(position.x() + rect.width() - size.width());

        if (vertical_alignment == Alignment::Center)
            position.set_y(position.y() + (rect.height() - size.height()) / 2);
        else if (vertical_alignment == Alignment::End)
            position.set_y(position.y() + rect.height() - size.height());

        return position;
    }
};

// Sparse, index-based container using two-level page tables.
// Layout state is throwaway — rebuilt on every layout pass — so a
// flat vector pre-allocated for the entire tree wastes memory, while
// a hash map pays hashing overhead on every access. Page tables give
// O(1) lookup without hashing, allocating pages only on first write.
template<typename T>
class PagedStore {
    static constexpr u32 PageBits = 4;
    static constexpr u32 PageSize = 1u << PageBits;
    static constexpr u32 PageMask = PageSize - 1;

    struct Page {
        Optional<T> entries[PageSize] {};
    };

public:
    void ensure_capacity(u32 count)
    {
        m_pages.resize((count + PageSize - 1) >> PageBits);
    }

    T* get(u32 index) const
    {
        auto page_index = index >> PageBits;
        if (page_index >= m_pages.size())
            return nullptr;
        auto const& page = m_pages[page_index];
        if (!page)
            return nullptr;
        auto& entry = page->entries[index & PageMask];
        if (!entry.has_value())
            return nullptr;
        return &entry.value();
    }

    T& allocate(u32 index)
    {
        auto page_index = index >> PageBits;
        if (page_index >= m_pages.size())
            m_pages.resize(page_index + 1);
        auto& page = m_pages[page_index];
        if (!page)
            page = make<Page>();
        auto& entry = page->entries[index & PageMask];
        entry = T {};
        return entry.value();
    }

    template<typename Callback>
    void for_each(Callback callback)
    {
        for (auto const& page : m_pages) {
            if (!page)
                continue;
            for (auto& entry : page->entries) {
                if (entry.has_value())
                    callback(entry.value());
            }
        }
    }

private:
    Vector<OwnPtr<Page>> m_pages;
};

struct LayoutState {
    struct UsedValues {
        UsedValues() = default;
        UsedValues(UsedValues&&) = default;
        UsedValues& operator=(UsedValues&&) = default;
        UsedValues& operator=(UsedValues const& other);

        NodeWithStyle const& node() const { return *m_node; }
        NodeWithStyle& node() { return const_cast<NodeWithStyle&>(*m_node); }
        void set_node(NodeWithStyle const&, UsedValues const* containing_block_used_values);

        UsedValues const* containing_block_used_values() const { return m_containing_block_used_values; }

        CSSPixels content_width() const { return m_content_width; }
        CSSPixels content_height() const { return m_content_height; }
        void set_content_width(CSSPixels);
        void set_content_height(CSSPixels);

        CSSPixelSize content_size() const { return { content_width(), content_height() }; }

        void set_indefinite_content_width();
        void set_indefinite_content_height();

        void set_has_definite_width(bool has_definite_width) { m_has_definite_width = has_definite_width; }
        void set_has_definite_height(bool has_definite_height) { m_has_definite_height = has_definite_height; }

        bool has_definite_width() const { return m_has_definite_width && width_constraint == SizeConstraint::None; }
        bool has_definite_height() const { return m_has_definite_height && height_constraint == SizeConstraint::None; }

        // Returns the available space for content inside this layout box.
        // If the space in an axis is indefinite, and the outer space is an intrinsic sizing constraint,
        // the constraint is used in that axis instead.
        AvailableSpace available_inner_space_or_constraints_from(AvailableSpace const& outer_space) const;

        void materialize_from_paintable(Painting::PaintableBox const&);

        void set_content_offset(CSSPixelPoint new_offset) { offset = new_offset; }
        void set_content_x(CSSPixels x) { offset.set_x(x); }
        void set_content_y(CSSPixels y) { offset.set_y(y); }

        // Offset from ICB (viewport) content edge to this box's content edge.
        // Computed lazily by walking the containing block chain.
        // For pre-populated nodes (partial relayout), returns the cached value from paintable absolute position.
        CSSPixelPoint cumulative_offset() const
        {
            if (m_cumulative_offset.has_value())
                return *m_cumulative_offset;
            if (m_containing_block_used_values)
                return m_containing_block_used_values->cumulative_offset() + offset;
            return offset;
        }

        // offset from top-left corner of content area of box's containing block to top-left corner of box's content area
        CSSPixelPoint offset;

        SizeConstraint width_constraint { SizeConstraint::None };
        SizeConstraint height_constraint { SizeConstraint::None };

        CSSPixels margin_left { 0 };
        CSSPixels margin_right { 0 };
        CSSPixels margin_top { 0 };
        CSSPixels margin_bottom { 0 };

        CSSPixels border_left { 0 };
        CSSPixels border_right { 0 };
        CSSPixels border_top { 0 };
        CSSPixels border_bottom { 0 };

        CSSPixels padding_left { 0 };
        CSSPixels padding_right { 0 };
        CSSPixels padding_top { 0 };
        CSSPixels padding_bottom { 0 };

        CSSPixels inset_left { 0 };
        CSSPixels inset_right { 0 };
        CSSPixels inset_top { 0 };
        CSSPixels inset_bottom { 0 };

        Vector<LineBox> line_boxes;

        CSSPixels margin_box_left() const { return margin_left + border_left_collapsed() + padding_left; }
        CSSPixels margin_box_right() const { return margin_right + border_right_collapsed() + padding_right; }
        CSSPixels margin_box_top() const { return margin_top + border_top_collapsed() + padding_top; }
        CSSPixels margin_box_bottom() const { return margin_bottom + border_bottom_collapsed() + padding_bottom; }

        CSSPixels margin_box_width() const { return margin_box_left() + content_width() + margin_box_right(); }
        CSSPixels margin_box_height() const { return margin_box_top() + content_height() + margin_box_bottom(); }

        CSSPixels border_box_left() const { return border_left_collapsed() + padding_left; }
        CSSPixels border_box_right() const { return border_right_collapsed() + padding_right; }
        CSSPixels border_box_top() const { return border_top_collapsed() + padding_top; }
        CSSPixels border_box_bottom() const { return border_bottom_collapsed() + padding_bottom; }

        CSSPixels border_box_width() const { return border_box_left() + content_width() + border_box_right(); }
        CSSPixels border_box_height() const { return border_box_top() + content_height() + border_box_bottom(); }

        CSSPixels padding_box_width() const { return padding_left + content_width() + padding_right; }
        CSSPixels padding_box_height() const { return padding_top + content_height() + padding_bottom; }

        Optional<LineBoxFragmentCoordinate> containing_line_box_fragment;

        void set_inline_end_static_position_rect(StaticPositionRect const& static_position_rect) { ensure_rare_data().inline_end_static_position_rect = static_position_rect; }
        Optional<StaticPositionRect> const& inline_end_static_position_rect() const
        {
            static Optional<StaticPositionRect> const empty;
            return m_rare ? m_rare->inline_end_static_position_rect : empty;
        }

        void add_floating_descendant(Box const& box) { ensure_rare_data().floating_descendants.set(&box); }
        HashTable<GC::Ptr<Box const>> const& floating_descendants() const
        {
            static HashTable<GC::Ptr<Box const>> const empty;
            return m_rare ? m_rare->floating_descendants : empty;
        }

        void set_override_borders_data(Painting::PaintableBox::BordersDataWithElementKind const& override_borders_data) { ensure_rare_data().override_borders_data = override_borders_data; }
        Optional<Painting::PaintableBox::BordersDataWithElementKind> const& override_borders_data() const
        {
            static Optional<Painting::PaintableBox::BordersDataWithElementKind> const empty;
            return m_rare ? m_rare->override_borders_data : empty;
        }

        void set_table_cell_coordinates(Painting::PaintableBox::TableCellCoordinates const& table_cell_coordinates) { ensure_rare_data().table_cell_coordinates = table_cell_coordinates; }
        Optional<Painting::PaintableBox::TableCellCoordinates> const& table_cell_coordinates() const
        {
            static Optional<Painting::PaintableBox::TableCellCoordinates> const empty;
            return m_rare ? m_rare->table_cell_coordinates : empty;
        }

        void set_computed_svg_path(Gfx::Path const& svg_path) { ensure_rare_data().computed_svg_path = svg_path; }
        Gfx::Path* computed_svg_path()
        {
            if (!m_rare || !m_rare->computed_svg_path.has_value())
                return nullptr;
            return &*m_rare->computed_svg_path;
        }

        void set_computed_svg_transforms(Painting::SVGGraphicsPaintable::ComputedTransforms const& computed_transforms) { ensure_rare_data().computed_svg_transforms = computed_transforms; }
        Optional<Painting::SVGGraphicsPaintable::ComputedTransforms> const& computed_svg_transforms() const
        {
            static Optional<Painting::SVGGraphicsPaintable::ComputedTransforms> const empty;
            return m_rare ? m_rare->computed_svg_transforms : empty;
        }

        void set_grid_layout_data(OwnPtr<GridLayoutData> grid_layout_data) { ensure_rare_data().grid_layout_data = move(grid_layout_data); }
        GridLayoutData const* grid_layout_data() const
        {
            return m_rare ? m_rare->grid_layout_data.ptr() : nullptr;
        }
        OwnPtr<GridLayoutData> take_grid_layout_data()
        {
            if (!m_rare)
                return {};
            return move(m_rare->grid_layout_data);
        }

        void set_flex_layout_data(OwnPtr<FlexLayoutData> flex_layout_data) { ensure_rare_data().flex_layout_data = move(flex_layout_data); }
        FlexLayoutData const* flex_layout_data() const
        {
            return m_rare ? m_rare->flex_layout_data.ptr() : nullptr;
        }
        OwnPtr<FlexLayoutData> take_flex_layout_data()
        {
            if (!m_rare)
                return {};
            return move(m_rare->flex_layout_data);
        }

        void set_grid_area_size(CSSPixelSize grid_area_size) { ensure_rare_data().grid_area_size = grid_area_size; }
        Optional<CSSPixelSize> const& grid_area_size() const
        {
            static Optional<CSSPixelSize> const empty;
            return m_rare ? m_rare->grid_area_size : empty;
        }

        void set_static_position_rect(StaticPositionRect const& static_position_rect) { ensure_rare_data().static_position_rect = static_position_rect; }
        CSSPixelPoint static_position() const
        {
            if (!m_rare || !m_rare->static_position_rect.has_value())
                return {};
            return m_rare->static_position_rect->aligned_position_for_box_with_size({ margin_box_width(), margin_box_height() });
        }

    private:
        friend struct LayoutState;

        AvailableSize available_width_inside() const;
        AvailableSize available_height_inside() const;

        bool use_collapsing_borders_model() const { return m_rare && m_rare->override_borders_data.has_value(); }
        // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
        CSSPixels border_left_collapsed() const { return use_collapsing_borders_model() ? round(border_left / 2) : border_left; }
        CSSPixels border_right_collapsed() const { return use_collapsing_borders_model() ? round(border_right / 2) : border_right; }
        CSSPixels border_top_collapsed() const { return use_collapsing_borders_model() ? round(border_top / 2) : border_top; }
        CSSPixels border_bottom_collapsed() const { return use_collapsing_borders_model() ? round(border_bottom / 2) : border_bottom; }

        struct RareData {
            RareData() = default;
            RareData(RareData const& other)
                : floating_descendants(other.floating_descendants)
                , table_cell_coordinates(other.table_cell_coordinates)
                , computed_svg_path(other.computed_svg_path)
                , grid_area_size(other.grid_area_size)
                , override_borders_data(other.override_borders_data)
                , computed_svg_transforms(other.computed_svg_transforms)
                , static_position_rect(other.static_position_rect)
            {
                if (other.grid_layout_data)
                    grid_layout_data = make<GridLayoutData>(*other.grid_layout_data);
                if (other.flex_layout_data)
                    flex_layout_data = make<FlexLayoutData>(*other.flex_layout_data);
            }

            HashTable<GC::Ptr<Box const>> floating_descendants;
            Optional<Painting::PaintableBox::TableCellCoordinates> table_cell_coordinates;
            Optional<Gfx::Path> computed_svg_path;
            OwnPtr<GridLayoutData> grid_layout_data;
            OwnPtr<FlexLayoutData> flex_layout_data;
            Optional<CSSPixelSize> grid_area_size;
            Optional<Painting::PaintableBox::BordersDataWithElementKind> override_borders_data;
            Optional<Painting::SVGGraphicsPaintable::ComputedTransforms> computed_svg_transforms;
            Optional<StaticPositionRect> static_position_rect;
            Optional<StaticPositionRect> inline_end_static_position_rect;
        };

        RareData& ensure_rare_data()
        {
            if (!m_rare)
                m_rare = make<RareData>();
            return *m_rare;
        }

        GC::Ptr<Layout::NodeWithStyle const> m_node { nullptr };
        UsedValues const* m_containing_block_used_values { nullptr };
        Optional<CSSPixelPoint> m_cumulative_offset;

        CSSPixels m_content_width { 0 };
        CSSPixels m_content_height { 0 };

        bool m_has_definite_width { false };
        bool m_has_definite_height { false };

        OwnPtr<RareData> m_rare;
    };

    LayoutState() = default;
    explicit LayoutState(NodeWithStyle const& subtree_root);
    ~LayoutState();

    // Commits the used values produced by layout and builds a paintable tree.
    void commit(Box& root);

    void ensure_capacity(u32 node_count);

    UsedValues& get_mutable(NodeWithStyle const&);
    UsedValues const& get(NodeWithStyle const&) const;

    UsedValues& populate_from_paintable(NodeWithStyle const&, Painting::PaintableBox const&);
    UsedValues& populate_node_from(LayoutState const& source, NodeWithStyle const& node);

    UsedValues const* try_get(NodeWithStyle const&) const;
    UsedValues* try_get_mutable(NodeWithStyle const&);
    UsedValues const* try_get(Node const&) const;

private:
    UsedValues& ensure_used_values_for(NodeWithStyle const&);
    void resolve_relative_positions();

    PagedStore<UsedValues> m_used_values_store;
    GC::Ptr<Layout::NodeWithStyle const> m_subtree_root;
};

inline CSSPixels clamp_to_max_dimension_value(CSSPixels value)
{
    if (value.might_be_saturated())
        return CSSPixels(CSSPixels::max_dimension_value);
    return value;
}

}
