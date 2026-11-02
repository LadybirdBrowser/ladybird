/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(PseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElementTreeNode);
GC_DEFINE_ALLOCATOR(ElementReferencePseudoElement);

SyntheticPseudoElement::SyntheticPseudoElement(CSS::PseudoElement type)
    : m_type(type)
{
}
SyntheticPseudoElement::SyntheticPseudoElement(CSS::PseudoElement type, GC::Ref<Element> originating_element)
    : m_type(type)
    , m_originating_element(originating_element)
{
}
SyntheticPseudoElement::~SyntheticPseudoElement() = default;

void SyntheticPseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_originating_element);
}

Layout::NodeWithStyle* SyntheticPseudoElement::unsafe_layout_node(Layout::BegunRead const& read) const
{
    if (!m_originating_element)
        return nullptr;
    return m_originating_element->pseudo_element_unsafe_layout_node(read, m_type);
}

void SyntheticPseudoElement::set_scroll_offset(CSSPixelPoint offset)
{
    m_scroll_offset = offset;
    publish_scroll_offset();
}

// The layout node arena holds what the pseudo-element has scrolled to against the originating element's identity and
// this pseudo-element's kind, for the box a build binds to it.
void SyntheticPseudoElement::publish_scroll_offset() const
{
    VERIFY(m_originating_element);
    if (m_originating_element->style_node_id().value() == 0)
        return;
    auto& document = m_originating_element->document();
    // Nothing has scrolled anything before a layout tree exists, so there is no offset to forget.
    if (!document.layout_node_arena_if_created() && m_scroll_offset.is_zero())
        return;
    Layout::RustFFI::render_state_set_pseudo_element_scroll_offset(document.layout_node_arena().host(),
        m_originating_element->style_node_id().value(), Layout::Node::encode_generated_for(m_type), m_scroll_offset);
}

Node& SyntheticPseudoElement::root() const
{
    VERIFY(m_originating_element);
    return m_originating_element->root();
}

void SyntheticPseudoElement::update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement abstract_element, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    if (!m_installed_style.record())
        return;
    effect.update_computed_properties_for_style(context, abstract_element);
}

void SyntheticPseudoElement::replace_installed_style(Layout::BegunRead const& read, CSS::InstalledStyle style)
{
    VERIFY(m_originating_element);
    auto const style_record_identity = style.record();
    if (m_installed_style.record() == style_record_identity)
        return;
    auto& style_computer = m_originating_element->document().style_computer();
    auto held_before = exchange(m_installed_style, move(style));
    // The pseudo-element holds the record it installed before until the engine takes the new one in, so the layout node
    // reads what it moves from before the pseudo-element lets go of it.
    if (auto* layout_node = unsafe_layout_node(read))
        layout_node->set_style_record_identity(m_installed_style.record());
    if (auto style_node_id = m_originating_element->style_node_id(); style_node_id != 0)
        CSS::StyleEngineFFI::style_engine_set_held_pseudo_style_record(style_computer.style_engine().host(), style_node_id, to_underlying(type()), style_record_identity.value(), held_before.record_pointer());
}

void SyntheticPseudoElement::swap_in_style_record(Layout::BegunRead const& read, void const* record)
{
    VERIFY(m_originating_element);
    CSS::InstalledStyle style { record };
    auto const style_record = style.record();
    VERIFY(style_record);
    if (m_installed_style.record() == style_record)
        return;
    m_installed_style = move(style);
    if (auto* layout_node = unsafe_layout_node(read))
        layout_node->set_style_record_identity(style_record);
}

void SyntheticPseudoElement::set_computed_style(Layout::BegunRead const& read, CSS::InstalledStyle style)
{
    if (!style) {
        clear_computed_style();
        return;
    }
    replace_installed_style(read, move(style));
}

void SyntheticPseudoElement::clear_computed_style(RefPtr<CSS::ComputedValues const> style_to_preserve_for_detachment)
{
    if (m_originating_element) {
        Layout::ForcedReadScope read { m_originating_element->document() };
        if (auto* layout_node = unsafe_layout_node(read)) {
            if (style_to_preserve_for_detachment)
                layout_node->set_computed_values(read, style_to_preserve_for_detachment.release_nonnull());
            else
                layout_node->pin_style_record_for_detachment();
        }
        if (auto style_node_id = m_originating_element->style_node_id(); style_node_id != 0 && !!m_installed_style.record())
            CSS::StyleEngineFFI::style_engine_set_held_pseudo_style_record(m_originating_element->document().style_computer().style_engine().host(), style_node_id, to_underlying(type()), 0, m_installed_style.record_pointer());
    }
    m_installed_style = {};
}

SyntheticPseudoElementTreeNode::SyntheticPseudoElementTreeNode(CSS::PseudoElement type)
    : SyntheticPseudoElement(type)
{
}
SyntheticPseudoElementTreeNode::SyntheticPseudoElementTreeNode(CSS::PseudoElement type, GC::Ref<Element> originating_element)
    : SyntheticPseudoElement(type, originating_element)
{
}
SyntheticPseudoElementTreeNode::~SyntheticPseudoElementTreeNode() = default;

void SyntheticPseudoElementTreeNode::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    TreeNode::visit_edges(visitor);
}

Layout::NodeWithStyle* ElementReferencePseudoElement::layout_node(Layout::BegunRead const& read) const
{
    return m_referenced_element->layout_node(read);
}

Layout::NodeWithStyle* ElementReferencePseudoElement::unsafe_layout_node(Layout::BegunRead const& read) const
{
    return m_referenced_element->unsafe_layout_node(read);
}

Node& ElementReferencePseudoElement::root() const
{
    return m_referenced_element->root();
}

CSS::InstalledStyle const& ElementReferencePseudoElement::installed_style() const
{
    return m_referenced_element->installed_style({});
}

void ElementReferencePseudoElement::update_animated_properties(Badge<Web::Animations::KeyframeEffect> const& badge, DOM::AbstractElement abstract_element, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    m_referenced_element->update_animated_properties_for_abstract_element(badge, abstract_element, effect, context);
}

void ElementReferencePseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_referenced_element);
}

}
