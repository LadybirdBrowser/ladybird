/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/OwnPtr.h>
#include <AK/WeakPtr.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TreeNode.h>

namespace Web::Animations {

struct AnimationUpdateContext;
class KeyframeEffect;

}

namespace Web::DOM {

class WEB_API PseudoElement : public JS::Cell {
    GC_CELL(PseudoElement, JS::Cell);
    GC_DECLARE_ALLOCATOR(PseudoElement);

public:
    virtual Layout::NodeWithStyle* layout_node() const = 0;
    virtual Layout::NodeWithStyle* unsafe_layout_node() const = 0;

    virtual RefPtr<CSS::ComputedProperties const> computed_properties() const = 0;
    virtual void update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&) = 0;

    virtual RefPtr<CSS::CustomPropertyData const> custom_property_data() const = 0;
    virtual void set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value) = 0;
};

class WEB_API SyntheticPseudoElement : public PseudoElement {
    GC_CELL(SyntheticPseudoElement, PseudoElement);
    GC_DECLARE_ALLOCATOR(SyntheticPseudoElement);

public:
    SyntheticPseudoElement();
    virtual ~SyntheticPseudoElement() override;

    Layout::NodeWithStyle* layout_node() const override { return m_layout_node.ptr(); }
    Layout::NodeWithStyle* unsafe_layout_node() const override { return m_layout_node.ptr(); }
    void set_layout_node(Layout::NodeWithStyle*);

    RefPtr<CSS::ComputedProperties const> computed_properties() const override;
    void update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&) override;
    void set_computed_properties(RefPtr<CSS::ComputedProperties> value);

    RefPtr<CSS::CustomPropertyData const> custom_property_data() const override;
    void set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value) override;

    bool has_non_empty_counters_set() const { return m_counters_set; }
    Optional<CSS::CountersSet const&> counters_set() const;
    CSS::CountersSet& ensure_counters_set();
    void set_counters_set(OwnPtr<CSS::CountersSet>&&);

    CSSPixelPoint scroll_offset() const { return m_scroll_offset; }
    void set_scroll_offset(CSSPixelPoint value) { m_scroll_offset = value; }

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    struct CustomPropertyDataStorage;

    WeakPtr<Layout::NodeWithStyle> m_layout_node;
    RefPtr<CSS::ComputedProperties> m_computed_properties;
    OwnPtr<CustomPropertyDataStorage> m_custom_property_data;
    OwnPtr<CSS::CountersSet> m_counters_set;
    CSSPixelPoint m_scroll_offset {};
};

// https://drafts.csswg.org/css-view-transitions/#pseudo-element-tree
class SyntheticPseudoElementTreeNode
    : public SyntheticPseudoElement
    , public TreeNode<SyntheticPseudoElementTreeNode> {
    GC_CELL(SyntheticPseudoElementTreeNode, SyntheticPseudoElement);
    GC_DECLARE_ALLOCATOR(SyntheticPseudoElementTreeNode);

public:
    SyntheticPseudoElementTreeNode();
    virtual ~SyntheticPseudoElementTreeNode() override;

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

    Layout::NodeWithStyle* layout_node() const override;
    Layout::NodeWithStyle* unsafe_layout_node() const override;

    RefPtr<CSS::ComputedProperties const> computed_properties() const override;
    void update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&) override;

    RefPtr<CSS::CustomPropertyData const> custom_property_data() const override;
    void set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value) override;

    GC::Ref<Element> const& referenced_element() const { return m_referenced_element; }

protected:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

private:
    GC::Ref<Element> m_referenced_element;
};

}
