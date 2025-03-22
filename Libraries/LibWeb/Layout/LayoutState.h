/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
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

struct LayoutState {
    struct UsedValues {
        NodeWithStyle const& node() const { return *m_node; }
        NodeWithStyle& node() { return const_cast<NodeWithStyle&>(*m_node); }
        void set_node(NodeWithStyle&, UsedValues const* containing_block_used_values);

        UsedValues const* containing_block_used_values() const { return m_containing_block_used_values; }

        CSSPixels content_width() const { return m_content_width; }
        CSSPixels content_height() const { return m_content_height; }
        void set_content_width(CSSPixels);
        void set_content_height(CSSPixels);

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

        void set_content_offset(CSSPixelPoint new_offset) { offset = new_offset; }
        void set_content_x(CSSPixels x) { offset.set_x(x); }
        void set_content_y(CSSPixels y) { offset.set_y(y); }

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

        Optional<LineBoxFragmentCoordinate> containing_line_box_fragment;

        void add_floating_descendant(Box const& box) { m_floating_descendants.set(&box); }
        auto const& floating_descendants() const { return m_floating_descendants; }

        void set_override_borders_data(Painting::PaintableBox::BordersDataWithElementKind const& override_borders_data) { m_override_borders_data = override_borders_data; }
        auto const& override_borders_data() const { return m_override_borders_data; }

        void set_table_cell_coordinates(Painting::PaintableBox::TableCellCoordinates const& table_cell_coordinates) { m_table_cell_coordinates = table_cell_coordinates; }
        auto const& table_cell_coordinates() const { return m_table_cell_coordinates; }

        void set_computed_svg_path(Gfx::Path const& svg_path) { m_computed_svg_path = svg_path; }
        auto& computed_svg_path() { return m_computed_svg_path; }

        void set_computed_svg_transforms(Painting::SVGGraphicsPaintable::ComputedTransforms const& computed_transforms) { m_computed_svg_transforms = computed_transforms; }
        auto const& computed_svg_transforms() const { return m_computed_svg_transforms; }

        void set_grid_template_columns(RefPtr<CSS::GridTrackSizeListStyleValue> used_values_for_grid_template_columns) { m_grid_template_columns = move(used_values_for_grid_template_columns); }
        auto const& grid_template_columns() const { return m_grid_template_columns; }

        void set_grid_template_rows(RefPtr<CSS::GridTrackSizeListStyleValue> used_values_for_grid_template_rows) { m_grid_template_rows = move(used_values_for_grid_template_rows); }
        auto const& grid_template_rows() const { return m_grid_template_rows; }

        void set_static_position_rect(StaticPositionRect const& static_position_rect) { m_static_position_rect = static_position_rect; }
        CSSPixelPoint static_position() const
        {
            CSSPixelSize size;
            size.set_width(content_width() + padding_left + padding_right + border_left + border_right + margin_left + margin_right);
            size.set_height(content_height() + padding_top + padding_bottom + border_top + border_bottom + margin_top + margin_bottom);
            return m_static_position_rect->aligned_position_for_box_with_size(size);
        }

    private:
        AvailableSize available_width_inside() const;
        AvailableSize available_height_inside() const;

        bool use_collapsing_borders_model() const { return m_override_borders_data.has_value(); }
        // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
        CSSPixels border_left_collapsed() const { return use_collapsing_borders_model() ? round(border_left / 2) : border_left; }
        CSSPixels border_right_collapsed() const { return use_collapsing_borders_model() ? round(border_right / 2) : border_right; }
        CSSPixels border_top_collapsed() const { return use_collapsing_borders_model() ? round(border_top / 2) : border_top; }
        CSSPixels border_bottom_collapsed() const { return use_collapsing_borders_model() ? round(border_bottom / 2) : border_bottom; }

        GC::Ptr<Layout::NodeWithStyle const> m_node { nullptr };
        UsedValues const* m_containing_block_used_values { nullptr };

        CSSPixels m_content_width { 0 };
        CSSPixels m_content_height { 0 };

        bool m_has_definite_width { false };
        bool m_has_definite_height { false };

        HashTable<GC::Ptr<Box const>> m_floating_descendants;

        Optional<Painting::PaintableBox::BordersDataWithElementKind> m_override_borders_data;
        Optional<Painting::PaintableBox::TableCellCoordinates> m_table_cell_coordinates;

        Optional<Gfx::Path> m_computed_svg_path;
        Optional<Painting::SVGGraphicsPaintable::ComputedTransforms> m_computed_svg_transforms;

        RefPtr<CSS::GridTrackSizeListStyleValue> m_grid_template_columns;
        RefPtr<CSS::GridTrackSizeListStyleValue> m_grid_template_rows;

        Optional<StaticPositionRect> m_static_position_rect;
    };

    ~LayoutState();

    // Commits the used values produced by layout and builds a paintable tree.
    void commit(Box& root);

    UsedValues& get_mutable(NodeWithStyle const&);
    UsedValues const& get(NodeWithStyle const&) const;

    HashMap<GC::Ref<Layout::Node const>, NonnullOwnPtr<UsedValues>> used_values_per_layout_node;

private:
    void resolve_relative_positions();
};

inline CSSPixels clamp_to_max_dimension_value(CSSPixels value)
{
    if (value.might_be_saturated())
        return CSSPixels(CSSPixels::max_dimension_value);
    return value;
}

}
