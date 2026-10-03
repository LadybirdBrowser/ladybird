/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/OwnPtr.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/InstalledStyle.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TreeNode.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::Animations {

struct AnimationUpdateContext;
class KeyframeEffect;

}

namespace Web::DOM {

class WEB_API PseudoElement : public JS::Cell {
    GC_CELL(PseudoElement, JS::Cell);
    GC_DECLARE_ALLOCATOR(PseudoElement);

public:
    virtual Layout::NodeWithStyle* layout_node(Layout::BegunRead const& read) const = 0;
    virtual Layout::NodeWithStyle* unsafe_layout_node(Layout::BegunRead const& read) const = 0;

    virtual Node& root() const = 0;

    virtual CSS::InstalledStyle const& installed_style() const = 0;
    CSS::StyleRecordID style_record_identity() const { return installed_style().record(); }
    virtual void update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&) = 0;
};

class WEB_API SyntheticPseudoElement : public PseudoElement {
    GC_CELL(SyntheticPseudoElement, PseudoElement);
    GC_DECLARE_ALLOCATOR(SyntheticPseudoElement);

public:
    explicit SyntheticPseudoElement(CSS::PseudoElement type);
    SyntheticPseudoElement(CSS::PseudoElement type, GC::Ref<Element> originating_element);
    virtual ~SyntheticPseudoElement() override;

    CSS::PseudoElement type() const { return m_type; }

    Layout::NodeWithStyle* layout_node(Layout::BegunRead const& read) const override { return unsafe_layout_node(read); }
    Layout::NodeWithStyle* unsafe_layout_node(Layout::BegunRead const& read) const override;

    virtual Node& root() const override;

    virtual CSS::InstalledStyle const& installed_style() const override { return m_installed_style; }
    void update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&) override;
    void set_computed_style(CSS::StyleRecordID);
    void clear_computed_style(RefPtr<CSS::ComputedValues const> style_to_preserve_for_detachment = nullptr);
    void refresh_computed_style(CSS::StyleRecordID);

    CSSPixelPoint scroll_offset() const { return m_scroll_offset; }
    void set_scroll_offset(CSSPixelPoint);
    void publish_scroll_offset() const;

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    void replace_style_record(CSS::StyleRecordID);

    CSS::PseudoElement m_type;
    GC::Ptr<Element> m_originating_element;
    // The authoritative StyleEngine record. C++ compatibility consumers borrow the record-owned
    // computed-values view rather than retaining one complete style per pseudo-element.
    CSS::InstalledStyle m_installed_style;
    CSSPixelPoint m_scroll_offset {};
};

// https://drafts.csswg.org/css-view-transitions/#pseudo-element-tree
class SyntheticPseudoElementTreeNode
    : public SyntheticPseudoElement
    , public TreeNode<SyntheticPseudoElementTreeNode> {
    GC_CELL(SyntheticPseudoElementTreeNode, SyntheticPseudoElement);
    GC_DECLARE_ALLOCATOR(SyntheticPseudoElementTreeNode);

public:
    explicit SyntheticPseudoElementTreeNode(CSS::PseudoElement type);
    SyntheticPseudoElementTreeNode(CSS::PseudoElement type, GC::Ref<Element> originating_element);
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

    Layout::NodeWithStyle* layout_node(Layout::BegunRead const& read) const override;
    Layout::NodeWithStyle* unsafe_layout_node(Layout::BegunRead const& read) const override;

    virtual Node& root() const override;

    virtual CSS::InstalledStyle const& installed_style() const override;
    void update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&) override;

    GC::Ref<Element> const& referenced_element() const { return m_referenced_element; }

protected:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

private:
    GC::Ref<Element> m_referenced_element;
};

}
