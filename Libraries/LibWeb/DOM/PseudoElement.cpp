/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(PseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElement);
GC_DEFINE_ALLOCATOR(SyntheticPseudoElementTreeNode);
GC_DEFINE_ALLOCATOR(ElementReferencePseudoElement);

struct SyntheticPseudoElement::CustomPropertyDataStorage {
    RefPtr<CSS::CustomPropertyData const> data;
};

SyntheticPseudoElement::SyntheticPseudoElement() = default;
SyntheticPseudoElement::SyntheticPseudoElement(GC::Ref<Element> originating_element)
    : m_originating_element(originating_element)
{
}
SyntheticPseudoElement::~SyntheticPseudoElement() = default;

void SyntheticPseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_originating_element);
    if (m_counters_set)
        m_counters_set->visit_edges(visitor);
}

void SyntheticPseudoElement::set_layout_node(Layout::NodeWithStyle* value)
{
    if (m_layout_node && m_layout_node.ptr() != value)
        m_layout_node->pin_style_record_for_detachment();
    m_layout_node = value;
}

void SyntheticPseudoElement::update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement abstract_element, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    if (!m_style_record_identity)
        return;
    effect.update_computed_properties_for_style(context, abstract_element);
}

void SyntheticPseudoElement::replace_style_record(CSS::StyleRecordID style_record_identity)
{
    VERIFY(m_originating_element);
    auto old_style_record_identity = m_style_record_identity;
    if (old_style_record_identity == style_record_identity)
        return;
    m_style_record_identity = style_record_identity;
    if (m_layout_node)
        m_layout_node->set_style_record_identity(style_record_identity);
}

void SyntheticPseudoElement::set_computed_style(CSS::StyleRecordID style_record_identity)
{
    if (!style_record_identity) {
        clear_computed_style();
        return;
    }
    replace_style_record(style_record_identity);
}

void SyntheticPseudoElement::clear_computed_style(RefPtr<CSS::ComputedValues const> style_to_preserve_for_detachment)
{
    if (m_layout_node) {
        if (style_to_preserve_for_detachment)
            m_layout_node->set_computed_values(style_to_preserve_for_detachment.release_nonnull());
        else
            m_layout_node->pin_style_record_for_detachment();
    }
    replace_style_record(0);
}

void SyntheticPseudoElement::refresh_computed_style(CSS::StyleRecordID style_record_identity)
{
    replace_style_record(style_record_identity);
    VERIFY(m_style_record_identity);
}

void SyntheticPseudoElement::set_computed_values_in_display_none_subtree(DOM::AbstractElement abstract_element)
{
    if (auto computed_values = abstract_element.computed_style()) {
        CSS::ComputedValues::Builder builder(*computed_values);
        builder->set_in_display_none_subtree(true);
        if (computed_values->has_animated_values()) {
            CSS::ComputedValues::Builder base_values_builder(computed_values->base_values());
            base_values_builder->set_in_display_none_subtree(true);
            builder->set_base_values(move(base_values_builder).build());
        }
        auto updated_values = move(builder).build();
        auto publication = abstract_element.document().style_computer().publish_computed_style_inputs(abstract_element, *updated_values);
        replace_style_record(publication.new_style_record);
    }
}

RefPtr<CSS::CustomPropertyData const> SyntheticPseudoElement::custom_property_data() const
{
    if (!m_custom_property_data)
        return nullptr;
    return m_custom_property_data->data;
}

void SyntheticPseudoElement::set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value)
{
    if (!value) {
        m_custom_property_data = nullptr;
        return;
    }

    if (!m_custom_property_data)
        m_custom_property_data = make<CustomPropertyDataStorage>();
    m_custom_property_data->data = move(value);
}

Optional<CSS::CountersSet const&> SyntheticPseudoElement::counters_set() const
{
    if (!m_counters_set)
        return {};
    return *m_counters_set;
}

CSS::CountersSet& SyntheticPseudoElement::ensure_counters_set()
{
    if (!m_counters_set)
        m_counters_set = make<CSS::CountersSet>();
    return *m_counters_set;
}

void SyntheticPseudoElement::set_counters_set(OwnPtr<CSS::CountersSet>&& counters_set)
{
    m_counters_set = move(counters_set);
}

SyntheticPseudoElementTreeNode::SyntheticPseudoElementTreeNode() = default;
SyntheticPseudoElementTreeNode::SyntheticPseudoElementTreeNode(GC::Ref<Element> originating_element)
    : SyntheticPseudoElement(originating_element)
{
}
SyntheticPseudoElementTreeNode::~SyntheticPseudoElementTreeNode() = default;

void SyntheticPseudoElementTreeNode::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    TreeNode::visit_edges(visitor);
}

Layout::NodeWithStyle* ElementReferencePseudoElement::layout_node() const
{
    return m_referenced_element->layout_node();
}

Layout::NodeWithStyle* ElementReferencePseudoElement::unsafe_layout_node() const
{
    return m_referenced_element->unsafe_layout_node();
}

CSS::StyleRecordID ElementReferencePseudoElement::style_record_identity() const
{
    return m_referenced_element->style_record_identity({});
}

void ElementReferencePseudoElement::update_animated_properties(Badge<Web::Animations::KeyframeEffect> const& badge, DOM::AbstractElement abstract_element, Web::Animations::KeyframeEffect& effect, Web::Animations::AnimationUpdateContext& context)
{
    m_referenced_element->update_animated_properties_for_abstract_element(badge, abstract_element, effect, context);
}

RefPtr<CSS::CustomPropertyData const> ElementReferencePseudoElement::custom_property_data() const
{
    return m_referenced_element->custom_property_data({});
}

void ElementReferencePseudoElement::set_custom_property_data(RefPtr<CSS::CustomPropertyData const> value)
{
    m_referenced_element->set_custom_property_data({}, move(value));
}

void ElementReferencePseudoElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_referenced_element);
}

}
