/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObjectSerializer.h>
#include <AK/Vector.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class AccessibilityTreeNode final : public JS::Cell {
    GC_CELL(AccessibilityTreeNode, JS::Cell);
    GC_DECLARE_ALLOCATOR(AccessibilityTreeNode);

public:
    static GC::Ref<AccessibilityTreeNode> create(Document*, DOM::Node const*);
    virtual ~AccessibilityTreeNode() override = default;

    GC::Ptr<DOM::Node const> value() const { return m_value; }
    void set_value(GC::Ptr<DOM::Node const> value) { m_value = value; }
    Vector<GC::Ptr<AccessibilityTreeNode>> children() const { return m_children; }
    void append_child(AccessibilityTreeNode* child) { m_children.append(child); }

    void serialize_tree_as_json(JsonObjectSerializer<StringBuilder>& object, Document const&) const;

protected:
    virtual void visit_edges(Visitor&) override;

private:
    explicit AccessibilityTreeNode(GC::Ptr<DOM::Node const>);

    GC::Ptr<DOM::Node const> m_value;
    Vector<GC::Ptr<AccessibilityTreeNode>> m_children;
};

}
