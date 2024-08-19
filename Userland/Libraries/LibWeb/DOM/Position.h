/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <LibJS/Heap/Heap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class Position final : public JS::Cell {
    JS_CELL(Position, JS::Cell);
    GC_DECLARE_ALLOCATOR(Position);

public:
    [[nodiscard]] static GC::Ref<Position> create(JS::Realm& realm, GC::Ref<Node> node, unsigned offset)
    {
        return realm.heap().allocate<Position>(realm, node, offset);
    }

    GC::Ptr<Node> node() { return m_node; }
    GC::Ptr<Node const> node() const { return m_node; }
    void set_node(GC::Ref<Node> node) { m_node = node; }

    unsigned offset() const { return m_offset; }
    bool offset_is_at_end_of_node() const;
    void set_offset(unsigned value) { m_offset = value; }
    bool increment_offset();
    bool decrement_offset();

    bool equals(GC::Ref<Position> other) const
    {
        return m_node.ptr() == other->m_node.ptr() && m_offset == other->m_offset;
    }

    ErrorOr<String> to_string() const;

private:
    Position(GC::Ptr<Node>, unsigned offset);

    virtual void visit_edges(Visitor&) override;

    GC::Ptr<Node> m_node;
    unsigned m_offset { 0 };
};

}

template<>
struct AK::Formatter<Web::DOM::Position> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::DOM::Position const& value)
    {
        return Formatter<StringView>::format(builder, TRY(value.to_string()));
    }
};
