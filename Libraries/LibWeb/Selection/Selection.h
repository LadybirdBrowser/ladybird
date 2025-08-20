/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Selection {

class WEB_API Selection final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Selection, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Selection);

public:
    [[nodiscard]] static GC::Ref<Selection> create(GC::Ref<JS::Realm>, GC::Ref<DOM::Document>);

    virtual ~Selection() override;

    enum class Direction {
        Forwards,
        Backwards,
        Directionless,
    };

    GC::Ptr<DOM::Node> anchor_node();
    unsigned anchor_offset();
    GC::Ptr<DOM::Node> focus_node();
    unsigned focus_offset() const;
    bool is_collapsed() const;
    unsigned range_count() const;
    String type() const;
    String direction() const;
    WebIDL::ExceptionOr<GC::Ptr<DOM::Range>> get_range_at(unsigned index);
    void add_range(GC::Ref<DOM::Range>);
    WebIDL::ExceptionOr<void> remove_range(GC::Ref<DOM::Range>);
    void remove_all_ranges();
    void empty();
    WebIDL::ExceptionOr<void> collapse(GC::Ptr<DOM::Node>, unsigned offset);
    WebIDL::ExceptionOr<void> set_position(GC::Ptr<DOM::Node>, unsigned offset);
    WebIDL::ExceptionOr<void> collapse_to_start();
    WebIDL::ExceptionOr<void> collapse_to_end();
    WebIDL::ExceptionOr<void> extend(GC::Ref<DOM::Node>, unsigned offset);
    WebIDL::ExceptionOr<void> set_base_and_extent(GC::Ref<DOM::Node> anchor_node, unsigned anchor_offset, GC::Ref<DOM::Node> focus_node, unsigned focus_offset);
    WebIDL::ExceptionOr<void> select_all_children(GC::Ref<DOM::Node>);
    WebIDL::ExceptionOr<void> modify(Optional<String> alter, Optional<String> direction, Optional<String> granularity);
    WebIDL::ExceptionOr<void>
    delete_from_document();
    bool contains_node(GC::Ref<DOM::Node>, bool allow_partial_containment) const;

    Utf16String to_string() const;

    // Non-standard convenience accessor for the selection's range.
    GC::Ptr<DOM::Range> range() const;

    // Non-standard accessor for the selection's document.
    GC::Ref<DOM::Document> document() const;

    // Non-standard
    GC::Ptr<DOM::Position> cursor_position() const;

    // Non-standard
    void move_offset_to_next_character(bool collapse_selection);
    void move_offset_to_previous_character(bool collapse_selection);
    void move_offset_to_next_word(bool collapse_selection);
    void move_offset_to_previous_word(bool collapse_selection);
    void move_offset_to_next_line(bool collapse_selection);
    void move_offset_to_previous_line(bool collapse_selection);

private:
    Selection(GC::Ref<JS::Realm>, GC::Ref<DOM::Document>);

    [[nodiscard]] bool is_empty() const;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void set_range(GC::Ptr<DOM::Range>);

    // https://w3c.github.io/selection-api/#dfn-empty
    GC::Ptr<DOM::Range> m_range;

    GC::Ref<DOM::Document> m_document;
    Direction m_direction { Direction::Directionless };
};

}
