/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TreeNode.h>

namespace Web::DOM {

class WEB_API PseudoElement : public JS::Cell {
    GC_CELL(PseudoElement, JS::Cell);
    GC_DECLARE_ALLOCATOR(PseudoElement);

public:
    virtual GC::Ptr<Layout::NodeWithStyle> layout_node() const = 0;
    virtual GC::Ptr<Layout::NodeWithStyle> unsafe_layout_node() const = 0;

    virtual GC::Ptr<CSS::ComputedProperties> computed_properties() const = 0;

    virtual RefPtr<CSS::CustomPropertyData const> custom_property_data() const = 0;
    virtual void set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value) = 0;
};

class WEB_API SyntheticPseudoElement : public PseudoElement {
    GC_CELL(SyntheticPseudoElement, PseudoElement);
    GC_DECLARE_ALLOCATOR(SyntheticPseudoElement);

    GC::Ptr<Layout::NodeWithStyle> layout_node() const override { return m_layout_node; }
    GC::Ptr<Layout::NodeWithStyle> unsafe_layout_node() const override { return m_layout_node; }
    void set_layout_node(GC::Ptr<Layout::NodeWithStyle> value) { m_layout_node = value; }

    GC::Ptr<CSS::ComputedProperties> computed_properties() const override { return m_computed_properties; }
    void set_computed_properties(GC::Ptr<CSS::ComputedProperties> value) { m_computed_properties = value; }

    RefPtr<CSS::CustomPropertyData const> custom_property_data() const override { return m_custom_property_data; }
    void set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value) override { m_custom_property_data = move(value); }

    bool has_non_empty_counters_set() const { return m_counters_set; }
    Optional<CSS::CountersSet const&> counters_set() const;
    CSS::CountersSet& ensure_counters_set();
    void set_counters_set(OwnPtr<CSS::CountersSet>&&);

    CSSPixelPoint scroll_offset() const { return m_scroll_offset; }
    void set_scroll_offset(CSSPixelPoint value) { m_scroll_offset = value; }

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    GC::Ptr<Layout::NodeWithStyle> m_layout_node;
    GC::Ptr<CSS::ComputedProperties> m_computed_properties;
    RefPtr<CSS::CustomPropertyData const> m_custom_property_data;
    OwnPtr<CSS::CountersSet> m_counters_set;
    CSSPixelPoint m_scroll_offset {};
};

// https://drafts.csswg.org/css-view-transitions/#pseudo-element-tree
class SyntheticPseudoElementTreeNode
    : public SyntheticPseudoElement
    , public TreeNode<SyntheticPseudoElementTreeNode> {
    GC_CELL(SyntheticPseudoElementTreeNode, SyntheticPseudoElement);
    GC_DECLARE_ALLOCATOR(SyntheticPseudoElementTreeNode);

protected:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;
};

class WEB_API ElementReferencePseudoElement : public PseudoElement {
    GC_CELL(ElementReferencePseudoElement, PseudoElement);
    GC_DECLARE_ALLOCATOR(ElementReferencePseudoElement);

    ElementReferencePseudoElement(GC::Ref<Element> referenced_element)
        : m_referenced_element(referenced_element)
    {
    }

    GC::Ptr<Layout::NodeWithStyle> layout_node() const override;
    GC::Ptr<Layout::NodeWithStyle> unsafe_layout_node() const override;

    GC::Ptr<CSS::ComputedProperties> computed_properties() const override;

    RefPtr<CSS::CustomPropertyData const> custom_property_data() const override;
    void set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value) override;

    GC::Ref<Element> const& referenced_element() const { return m_referenced_element; }

protected:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

private:
    GC::Ref<Element> m_referenced_element;
};

}
