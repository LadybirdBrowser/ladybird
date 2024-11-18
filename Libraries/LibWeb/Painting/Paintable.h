/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGC/Root.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Layout/LineBox.h>
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
    int index_in_node { 0 };
    Optional<CSSPixels> vertical_distance {};
    Optional<CSSPixels> horizontal_distance {};

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

class Paintable
    : public JS::Cell
    , public TreeNode<Paintable> {
    GC_CELL(Paintable, JS::Cell);

public:
    virtual ~Paintable();

    [[nodiscard]] bool is_visible() const;
    [[nodiscard]] bool is_positioned() const { return m_positioned; }
    [[nodiscard]] bool is_fixed_position() const { return m_fixed_position; }
    [[nodiscard]] bool is_sticky_position() const { return m_sticky_position; }
    [[nodiscard]] bool is_absolutely_positioned() const { return m_absolutely_positioned; }
    [[nodiscard]] bool is_floating() const { return m_floating; }
    [[nodiscard]] bool is_inline() const { return m_inline; }
    [[nodiscard]] CSS::Display display() const;

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback)
    {
        if (is<U>(*this)) {
            if (auto decision = callback(static_cast<U&>(*this)); decision != TraversalDecision::Continue)
                return decision;
        }
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback) const
    {
        if (is<U>(*this)) {
            if (auto decision = callback(static_cast<U const&>(*this)); decision != TraversalDecision::Continue)
                return decision;
        }
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback)
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_subtree_of_type(Callback callback) const
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->template for_each_in_inclusive_subtree_of_type<U>(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback)
    {
        if (auto decision = callback(*this); decision != TraversalDecision::Continue)
            return decision;
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->for_each_in_inclusive_subtree(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback) const
    {
        if (auto decision = callback(*this); decision != TraversalDecision::Continue)
            return decision;
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->for_each_in_inclusive_subtree(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    template<typename Callback>
    TraversalDecision for_each_in_subtree(Callback callback) const
    {
        for (auto* child = first_child(); child; child = child->next_sibling()) {
            if (child->for_each_in_inclusive_subtree(callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    }

    bool has_stacking_context() const;
    StackingContext* enclosing_stacking_context();

    virtual void before_paint(PaintContext&, PaintPhase) const { }
    virtual void after_paint(PaintContext&, PaintPhase) const { }

    virtual void paint(PaintContext&, PaintPhase) const { }

    virtual void before_children_paint(PaintContext&, PaintPhase) const { }
    virtual void after_children_paint(PaintContext&, PaintPhase) const { }

    virtual void apply_scroll_offset(PaintContext&, PaintPhase) const { }
    virtual void reset_scroll_offset(PaintContext&, PaintPhase) const { }

    virtual void apply_clip_overflow_rect(PaintContext&, PaintPhase) const { }
    virtual void clear_clip_overflow_rect(PaintContext&, PaintPhase) const { }

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
    virtual DOM::Node* mouse_event_target() const { return nullptr; }

    virtual bool handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned buttons, unsigned modifiers, int wheel_delta_x, int wheel_delta_y);

    Layout::Node const& layout_node() const { return m_layout_node; }
    Layout::Node& layout_node() { return const_cast<Layout::Node&>(*m_layout_node); }

    [[nodiscard]] GC::Ptr<DOM::Node> dom_node();
    [[nodiscard]] GC::Ptr<DOM::Node const> dom_node() const;
    void set_dom_node(GC::Ptr<DOM::Node>);

    CSS::ImmutableComputedValues const& computed_values() const;

    bool visible_for_hit_testing() const { return computed_values().pointer_events() != CSS::PointerEvents::None; }

    GC::Ptr<HTML::Navigable> navigable() const;

    virtual void set_needs_display(InvalidateDisplayList = InvalidateDisplayList::Yes);

    PaintableBox* containing_block() const;

    template<typename T>
    bool fast_is() const = delete;

    [[nodiscard]] virtual bool is_paintable_box() const { return false; }
    [[nodiscard]] virtual bool is_paintable_with_lines() const { return false; }
    [[nodiscard]] virtual bool is_svg_paintable() const { return false; }
    [[nodiscard]] virtual bool is_text_paintable() const { return false; }

    DOM::Document const& document() const;
    DOM::Document& document();

    CSSPixelPoint box_type_agnostic_position() const;

    enum class SelectionState : u8 {
        None,        // No selection
        Start,       // Selection starts in this Node
        End,         // Selection ends in this Node
        StartAndEnd, // Selection starts and ends in this Node
        Full,        // Selection starts before and ends after this Node
    };

    SelectionState selection_state() const { return m_selection_state; }
    void set_selection_state(SelectionState state) { m_selection_state = state; }

    Gfx::AffineTransform compute_combined_css_transform() const;

    virtual void resolve_paint_properties() { }

    virtual void finalize() override
    {
        if (m_list_node.is_in_list())
            m_list_node.remove();
    }

    friend class Layout::Node;

protected:
    explicit Paintable(Layout::Node const&);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    IntrusiveListNode<Paintable> m_list_node;
    GC::Ptr<DOM::Node> m_dom_node;
    GC::Ref<Layout::Node const> m_layout_node;
    Optional<GC::Ptr<PaintableBox>> mutable m_containing_block;

    SelectionState m_selection_state { SelectionState::None };

    bool m_positioned : 1 { false };
    bool m_fixed_position : 1 { false };
    bool m_sticky_position : 1 { false };
    bool m_absolutely_positioned : 1 { false };
    bool m_floating : 1 { false };
    bool m_inline : 1 { false };
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

Painting::BorderRadiiData normalize_border_radii_data(Layout::Node const& node, CSSPixelRect const& rect, CSS::BorderRadiusData const& top_left_radius, CSS::BorderRadiusData const& top_right_radius, CSS::BorderRadiusData const& bottom_right_radius, CSS::BorderRadiusData const& bottom_left_radius);

}
