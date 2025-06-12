/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/AbstractRange.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::DOM {

enum class RelativeBoundaryPointPosition {
    Equal,
    Before,
    After,
};

// https://dom.spec.whatwg.org/#concept-range-bp-position
RelativeBoundaryPointPosition position_of_boundary_point_relative_to_other_boundary_point(BoundaryPoint a, BoundaryPoint b);

class Range final : public AbstractRange {
    WEB_PLATFORM_OBJECT(Range, AbstractRange);
    GC_DECLARE_ALLOCATOR(Range);

public:
    [[nodiscard]] static GC::Ref<Range> create(Document&);
    [[nodiscard]] static GC::Ref<Range> create(HTML::Window&);
    [[nodiscard]] static GC::Ref<Range> create(GC::Ref<Node> start_container, WebIDL::UnsignedLong start_offset, GC::Ref<Node> end_container, WebIDL::UnsignedLong end_offset);
    static WebIDL::ExceptionOr<GC::Ref<Range>> construct_impl(JS::Realm&);

    virtual ~Range() override;

    WebIDL::ExceptionOr<void> set_start(GC::Ref<Node> node, WebIDL::UnsignedLong offset);
    WebIDL::ExceptionOr<void> set_end(GC::Ref<Node> node, WebIDL::UnsignedLong offset);
    WebIDL::ExceptionOr<void> set_start_before(GC::Ref<Node> node);
    WebIDL::ExceptionOr<void> set_start_after(GC::Ref<Node> node);
    WebIDL::ExceptionOr<void> set_end_before(GC::Ref<Node> node);
    WebIDL::ExceptionOr<void> set_end_after(GC::Ref<Node> node);
    WebIDL::ExceptionOr<void> select_node(GC::Ref<Node> node);
    void collapse(bool to_start);
    WebIDL::ExceptionOr<void> select_node_contents(GC::Ref<Node>);

    // https://dom.spec.whatwg.org/#dom-range-start_to_start
    enum HowToCompareBoundaryPoints : WebIDL::UnsignedShort {
        START_TO_START = 0,
        START_TO_END = 1,
        END_TO_END = 2,
        END_TO_START = 3,
    };

    WebIDL::ExceptionOr<WebIDL::Short> compare_boundary_points(WebIDL::UnsignedShort how, Range const& source_range) const;

    GC::Ref<Range> inverted() const;
    GC::Ref<Range> normalized() const;
    GC::Ref<Range> clone_range() const;

    GC::Ref<Node> common_ancestor_container() const;

    // https://dom.spec.whatwg.org/#dom-range-detach
    void detach() const
    {
        // The detach() method steps are to do nothing.
        // Note: Its functionality (disabling a Range object) was removed, but the method itself is preserved for compatibility.
    }

    bool intersects_node(GC::Ref<Node>) const;
    WebIDL::ExceptionOr<bool> is_point_in_range(GC::Ref<Node>, WebIDL::UnsignedLong offset) const;
    WebIDL::ExceptionOr<WebIDL::Short> compare_point(GC::Ref<Node>, WebIDL::UnsignedLong offset) const;

    WebIDL::ExceptionOr<void> delete_contents();
    WebIDL::ExceptionOr<GC::Ref<DocumentFragment>> extract_contents();
    WebIDL::ExceptionOr<GC::Ref<DocumentFragment>> clone_contents();

    WebIDL::ExceptionOr<void> insert_node(GC::Ref<Node>);
    WebIDL::ExceptionOr<void> surround_contents(GC::Ref<Node> new_parent);

    String to_string() const;

    static HashTable<Range*>& live_ranges();

    GC::Ref<Geometry::DOMRectList> get_client_rects();
    GC::Ref<Geometry::DOMRect> get_bounding_client_rect();

    bool contains_node(GC::Ref<Node>) const;

    void set_associated_selection(Badge<Selection::Selection>, GC::Ptr<Selection::Selection>);

    WebIDL::ExceptionOr<GC::Ref<DocumentFragment>> create_contextual_fragment(String const& fragment);

    template<typename Callback>
    void for_each_contained(Callback callback) const
    {
        return const_cast<Range*>(this)->for_each_contained(move(callback));
    }

    template<typename Callback>
    void for_each_contained(Callback callback)
    {
        GC::Ptr<Node> end = m_end_container;
        while (end && !end->next_sibling())
            end = end->parent();
        if (end)
            end = end->next_sibling();

        for (GC::Ptr<Node> node = m_start_container; node && node != end; node = node->next_in_pre_order()) {
            if (contains_node(*node)) {
                if (callback(*node) == IterationDecision::Break)
                    return;
            }
        }
    }

private:
    friend class CharacterData;
    friend class Node;
    friend class Text;

    explicit Range(Document&);
    Range(GC::Ref<Node> start_container, WebIDL::UnsignedLong start_offset, GC::Ref<Node> end_container, WebIDL::UnsignedLong end_offset);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Node> root() const;

    void update_associated_selection();

    enum class StartOrEnd {
        Start,
        End,
    };

    void set_start_node(GC::Ref<Node> node) { m_start_container = node; }
    void set_start_offset(WebIDL::UnsignedLong offset) { m_start_offset = offset; }
    void set_end_node(GC::Ref<Node> node) { m_end_container = node; }
    void set_end_offset(WebIDL::UnsignedLong offset) { m_end_offset = offset; }

    void increase_start_offset(WebIDL::UnsignedLong count) { m_start_offset += count; }
    void increase_end_offset(WebIDL::UnsignedLong count) { m_end_offset += count; }
    void decrease_start_offset(WebIDL::UnsignedLong count) { m_start_offset -= count; }
    void decrease_end_offset(WebIDL::UnsignedLong count) { m_end_offset -= count; }

    WebIDL::ExceptionOr<void> set_start_or_end(GC::Ref<Node> node, u32 offset, StartOrEnd start_or_end);
    WebIDL::ExceptionOr<void> select(GC::Ref<Node> node);

    WebIDL::ExceptionOr<GC::Ref<DocumentFragment>> extract();
    WebIDL::ExceptionOr<GC::Ref<DocumentFragment>> clone_the_contents();
    WebIDL::ExceptionOr<void> insert(GC::Ref<Node>);

    bool partially_contains_node(GC::Ref<Node>) const;

    GC::Ptr<Selection::Selection> m_associated_selection;
};

}
