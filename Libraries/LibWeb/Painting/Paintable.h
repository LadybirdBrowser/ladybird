/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TraversalDecision.h>
#include <LibWeb/TreeNode.h>

namespace Web::Painting {

enum class PaintPhase {
    Background,
    Border,
    TableCollapsedBorder,
    Foreground,
    Outline,
    Overlay,
};

struct HitTestResult {
    GC::Root<Paintable> paintable;
    size_t index_in_node { 0 };
    Optional<CSSPixels> vertical_distance {};
    Optional<CSSPixels> horizontal_distance {};
    Optional<CSS::CursorPredefined> cursor_override {};
    enum InternalPosition {
        None,
        Before,
        Inside,
        After,
    };
    InternalPosition internal_position { None };

    DOM::Node* dom_node();
    DOM::Node const* dom_node() const;
};

enum class HitTestType {
    Exact,      // Exact matches only
    TextCursor, // Clicking past the right/bottom edge of text will still hit the text
};

class WEB_API Paintable
    : public JS::Cell
    , public TreeNode<Paintable> {
    GC_CELL(Paintable, JS::Cell);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~Paintable();

    void detach_from_layout_node();

    [[nodiscard]] bool is_visible() const;
    [[nodiscard]] bool is_positioned() const { return m_positioned; }
    [[nodiscard]] bool is_fixed_position() const { return m_fixed_position; }
    [[nodiscard]] bool is_sticky_position() const { return m_sticky_position; }
    [[nodiscard]] bool is_absolutely_positioned() const { return m_absolutely_positioned; }
    [[nodiscard]] bool is_floating() const { return m_floating; }
    [[nodiscard]] bool is_inline() const { return m_inline; }
    [[nodiscard]] CSS::Display display() const;

    bool has_stacking_context() const;
    StackingContext* enclosing_stacking_context();

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const { }
    void paint_inspector_overlay(DisplayListRecordingContext&) const;

    [[nodiscard]] virtual TraversalDecision hit_test(CSSPixelPoint, HitTestType, Function<TraversalDecision(HitTestResult)> const& callback) const;

    virtual bool wants_mouse_events() const { return false; }

    virtual bool forms_unconnected_subtree() const { return false; }

    enum class DispatchEventOfSameName {
        Yes,
        No,
    };
    // When these methods return true, the DOM event with the same name will be
    // dispatch at the mouse_event_target if it returns a valid DOM::Node, or
    // the layout node's associated DOM node if it doesn't.
    virtual DispatchEventOfSameName handle_mousedown(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers);
    virtual DispatchEventOfSameName handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned button, unsigned modifiers);
    virtual DispatchEventOfSameName handle_mousemove(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers);
    virtual void handle_mouseleave(Badge<EventHandler>) { }

    virtual bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers, int wheel_delta_x, int wheel_delta_y);

    Layout::Node const& layout_node() const { return m_layout_node; }
    Layout::Node& layout_node() { return const_cast<Layout::Node&>(*m_layout_node); }

    [[nodiscard]] GC::Ptr<DOM::Node> dom_node();
    [[nodiscard]] GC::Ptr<DOM::Node const> dom_node() const;
    void set_dom_node(GC::Ptr<DOM::Node>);

    CSS::ImmutableComputedValues const& computed_values() const;

    bool visible_for_hit_testing() const;

    GC::Ptr<HTML::Navigable> navigable() const;

    virtual void set_needs_display(InvalidateDisplayList = InvalidateDisplayList::Yes);
    void set_needs_paint_only_properties_update(bool);
    [[nodiscard]] bool needs_paint_only_properties_update() const { return m_needs_paint_only_properties_update; }

    PaintableBox* containing_block() const;

    template<typename T>
    bool fast_is() const = delete;

    [[nodiscard]] virtual bool is_navigable_container_viewport_paintable() const { return false; }
    [[nodiscard]] virtual bool is_viewport_paintable() const { return false; }
    [[nodiscard]] virtual bool is_paintable_box() const { return false; }
    [[nodiscard]] virtual bool is_paintable_with_lines() const { return false; }
    [[nodiscard]] virtual bool is_svg_paintable() const { return false; }
    [[nodiscard]] virtual bool is_svg_svg_paintable() const { return false; }
    [[nodiscard]] virtual bool is_svg_path_paintable() const { return false; }
    [[nodiscard]] virtual bool is_svg_graphics_paintable() const { return false; }
    [[nodiscard]] virtual bool is_text_paintable() const { return false; }

    DOM::Document const& document() const;
    DOM::Document& document();

    CSSPixelPoint box_type_agnostic_position() const;

    void scroll_ancestor_to_offset_into_view(size_t offset);

    enum class SelectionState : u8 {
        None,        // No selection
        Start,       // Selection starts in this Node
        End,         // Selection ends in this Node
        StartAndEnd, // Selection starts and ends in this Node
        Full,        // Selection starts before and ends after this Node
    };

    SelectionState selection_state() const { return m_selection_state; }
    void set_selection_state(SelectionState state) { m_selection_state = state; }

    // https://drafts.csswg.org/css-pseudo-4/#highlight-styling
    struct TextDecorationStyle {
        Vector<CSS::TextDecorationLine> line;
        CSS::TextDecorationStyle style;
        Color color;
    };
    struct SelectionStyle {
        Color background_color;
        Optional<Color> text_color;
        Optional<Vector<ShadowData>> text_shadow;
        Optional<TextDecorationStyle> text_decoration;

        bool has_styling() const
        {
            return background_color.alpha() > 0 || text_color.has_value() || text_shadow.has_value() || text_decoration.has_value();
        }
    };
    [[nodiscard]] SelectionStyle selection_style() const;

    MUST_UPCALL virtual void resolve_paint_properties();

    [[nodiscard]] String debug_description() const;

    virtual void finalize() override;

    friend class Layout::Node;

protected:
    explicit Paintable(Layout::Node const&);

    virtual void paint_inspector_overlay_internal(DisplayListRecordingContext&) const { }
    virtual void visit_edges(Cell::Visitor&) override;

    Optional<GC::Ptr<PaintableBox>> mutable m_containing_block;

private:
    IntrusiveListNode<Paintable> m_list_node;
    GC::Ptr<DOM::Node> m_dom_node;
    GC::Ref<Layout::Node const> m_layout_node;

    SelectionState m_selection_state { SelectionState::None };

    bool m_positioned : 1 { false };
    bool m_fixed_position : 1 { false };
    bool m_sticky_position : 1 { false };
    bool m_absolutely_positioned : 1 { false };
    bool m_floating : 1 { false };
    bool m_inline : 1 { false };
    bool m_visible_for_hit_testing : 1 { true };

protected:
    bool m_needs_paint_only_properties_update : 1 { true };
};

inline DOM::Node* HitTestResult::dom_node()
{
    return paintable->dom_node();
}

inline DOM::Node const* HitTestResult::dom_node() const
{
    return paintable->dom_node();
}

template<>
inline bool Paintable::fast_is<PaintableBox>() const { return is_paintable_box(); }

template<>
inline bool Paintable::fast_is<PaintableWithLines>() const { return is_paintable_with_lines(); }

template<>
inline bool Paintable::fast_is<TextPaintable>() const { return is_text_paintable(); }

WEB_API Painting::BorderRadiiData normalize_border_radii_data(Layout::Node const& node, CSSPixelRect const& rect, CSS::BorderRadiusData const& top_left_radius, CSS::BorderRadiusData const& top_right_radius, CSS::BorderRadiusData const& bottom_right_radius, CSS::BorderRadiusData const& bottom_left_radius);

}
