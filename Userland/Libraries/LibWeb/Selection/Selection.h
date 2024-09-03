/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Selection {

class Selection final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Selection, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Selection);

public:
    [[nodiscard]] static JS::NonnullGCPtr<Selection> create(JS::NonnullGCPtr<JS::Realm>, JS::NonnullGCPtr<DOM::Document>);

    virtual ~Selection() override;

    enum class Direction {
        Forwards,
        Backwards,
        Directionless,
    };

    JS::GCPtr<DOM::Node> anchor_node();
    unsigned anchor_offset();
    JS::GCPtr<DOM::Node> focus_node();
    unsigned focus_offset() const;
    bool is_collapsed() const;
    unsigned range_count() const;
    String type() const;
    String direction() const;
    WebIDL::ExceptionOr<JS::GCPtr<DOM::Range>> get_range_at(unsigned index);
    void add_range(JS::NonnullGCPtr<DOM::Range>);
    WebIDL::ExceptionOr<void> remove_range(JS::NonnullGCPtr<DOM::Range>);
    void remove_all_ranges();
    void empty();
    WebIDL::ExceptionOr<void> collapse(JS::GCPtr<DOM::Node>, unsigned offset);
    WebIDL::ExceptionOr<void> set_position(JS::GCPtr<DOM::Node>, unsigned offset);
    WebIDL::ExceptionOr<void> collapse_to_start();
    WebIDL::ExceptionOr<void> collapse_to_end();
    WebIDL::ExceptionOr<void> extend(JS::NonnullGCPtr<DOM::Node>, unsigned offset);
    WebIDL::ExceptionOr<void> set_base_and_extent(JS::NonnullGCPtr<DOM::Node> anchor_node, unsigned anchor_offset, JS::NonnullGCPtr<DOM::Node> focus_node, unsigned focus_offset);
    WebIDL::ExceptionOr<void> select_all_children(JS::NonnullGCPtr<DOM::Node>);
    WebIDL::ExceptionOr<void>
    delete_from_document();
    bool contains_node(JS::NonnullGCPtr<DOM::Node>, bool allow_partial_containment) const;

    String to_string() const;

    // Non-standard convenience accessor for the selection's range.
    JS::GCPtr<DOM::Range> range() const;

    // Non-standard accessor for the selection's document.
    JS::NonnullGCPtr<DOM::Document> document() const;

private:
    Selection(JS::NonnullGCPtr<JS::Realm>, JS::NonnullGCPtr<DOM::Document>);

    [[nodiscard]] bool is_empty() const;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void set_range(JS::GCPtr<DOM::Range>);

    // https://w3c.github.io/selection-api/#dfn-empty
    JS::GCPtr<DOM::Range> m_range;

    JS::NonnullGCPtr<DOM::Document> m_document;
    Direction m_direction { Direction::Directionless };
};

}
