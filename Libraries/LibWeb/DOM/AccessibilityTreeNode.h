/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObjectSerializer.h>
#include <AK/Vector.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class AccessibilityTreeNode final : public JS::Cell {
    JS_CELL(AccessibilityTreeNode, JS::Cell);
    JS_DECLARE_ALLOCATOR(AccessibilityTreeNode);

public:
    static JS::NonnullGCPtr<AccessibilityTreeNode> create(Document*, DOM::Node const*);
    virtual ~AccessibilityTreeNode() override = default;

    JS::GCPtr<DOM::Node const> value() const { return m_value; }
    void set_value(JS::GCPtr<DOM::Node const> value) { m_value = value; }
    Vector<JS::GCPtr<AccessibilityTreeNode>> children() const { return m_children; }
    void append_child(AccessibilityTreeNode* child) { m_children.append(child); }

    void serialize_tree_as_json(JsonObjectSerializer<StringBuilder>& object, Document const&) const;

protected:
    virtual void visit_edges(Visitor&) override;

private:
    explicit AccessibilityTreeNode(JS::GCPtr<DOM::Node const>);

    JS::GCPtr<DOM::Node const> m_value;
    Vector<JS::GCPtr<AccessibilityTreeNode>> m_children;
};

}
