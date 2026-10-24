/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Animations/NamedTimelines.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(NamedTimelines);

GC::Ref<NamedTimelines> NamedTimelines::create(DOM::Document& document)
{
    return GC::Heap::the().allocate<NamedTimelines>(document);
}

NamedTimelines::NamedTimelines(DOM::Document& document)
    : m_document(document)
{
}

void NamedTimelines::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    for (auto& declaring_element : m_declaring_elements)
        visitor.visit(declaring_element.element);
    for (auto& [key, timeline] : m_timelines) {
        visitor.visit(key.element);
        visitor.visit(timeline);
    }
}

NamedTimelines::Declaration const* NamedTimelines::DeclaringElement::declaration_named(Utf16FlyString const& name) const
{
    // NB: Of the declarations that share a name, the last one is the one the element defines.
    for (size_t i = declarations.size(); i-- > 0;) {
        if (declarations[i].name == name)
            return &declarations[i];
    }
    return nullptr;
}

// https://drafts.csswg.org/css-scoping-1/#flat-tree
// NB: Visits |node| and its flat tree descendants in flat tree order: a shadow host's shadow tree takes the place of
//     its children, and a slot's assigned nodes take the place of its children when it has any.
template<typename Callback>
static void for_each_flat_tree_inclusive_descendant(DOM::Node& node, Callback const& callback)
{
    callback(node);
    auto visit_children_of = [&](DOM::Node& parent) {
        for (auto* child = parent.first_child(); child; child = child->next_sibling())
            for_each_flat_tree_inclusive_descendant(*child, callback);
    };
    if (auto* element = as_if<DOM::Element>(node); element && element->shadow_root()) {
        visit_children_of(*element->shadow_root());
        return;
    }
    if (auto* slot = as_if<HTML::HTMLSlotElement>(node); slot && !slot->assigned_nodes_internal().is_empty()) {
        for (auto const& slottable : slot->assigned_nodes_internal())
            slottable.visit([&](auto const& assigned) { for_each_flat_tree_inclusive_descendant(static_cast<DOM::Node&>(*assigned), callback); });
        return;
    }
    visit_children_of(node);
}

void NamedTimelines::refresh_declarations()
{
    // NB: An element that comes to declare timelines changes its style, which moves the declarations generation. A change
    //     to the tree can only take declarations away, which matters where any were collected.
    auto dom_tree_version = m_document->dom_tree_version();
    auto declarations_generation = m_document->timeline_declarations_generation();
    bool const tree_changed = m_collected_dom_tree_version != dom_tree_version && !m_declaring_elements.is_empty();
    if (m_collected_declarations_generation.has_value() && m_collected_declarations_generation == declarations_generation && !tree_changed)
        return;
    m_collected_dom_tree_version = dom_tree_version;
    m_collected_declarations_generation = declarations_generation;
    m_tracked_animations_need_update = true;

    m_declaring_elements.clear();
    m_declaring_element_indices.clear();

    // NB: The declarations are collected in flat tree order, which the lookup of the last defined timeline uses.
    for_each_flat_tree_inclusive_descendant(*m_document, [&](DOM::Node& node) {
        auto* element = as_if<DOM::Element>(node);
        if (!element)
            return;
        auto const* values_pointer = element->style_group<CSS::ComputedValues::AnimationValues>();
        if (!values_pointer || !CSS::ComputedValuesFFI::rust_animation_values_declare_timelines(values_pointer))
            return;
        auto const& values = *values_pointer;

        DeclaringElement declaring_element { .element = *element, .declarations = {}, .scopes_all_names = false, .scoped_names = {} };
        CSS::ComputedValuesFFI::rust_animation_values_timeline_declarations(&values, &declaring_element, [](void* context, CSS::ComputedValuesFFI::FfiTimelineDeclaration const* declaration) {
            static_cast<DeclaringElement*>(context)->declarations.append({
                .name = CSS::css_string_from_rust(declaration->name),
                .is_view = declaration->is_view,
                .axis = scroll_axis_from_css_axis(static_cast<CSS::Axis>(declaration->axis)),
                .view_inset = view_timeline_inset_from_ffi(declaration->view_inset),
            });
        });
        auto scope = CSS::ComputedValuesFFI::rust_animation_values_timeline_scope(&values, &declaring_element, [](void* context, CSS::StyleValueFFI::CssString const* name) {
            static_cast<DeclaringElement*>(context)->scoped_names.append(CSS::css_string_from_rust(name));
        });
        declaring_element.scopes_all_names = scope == CSS::ComputedValuesFFI::FfiTimelineScope::All;

        m_declaring_element_indices.set(element, m_declaring_elements.size());
        m_declaring_elements.append(move(declaring_element));
    });

    // NB: A timeline whose element no longer declares it is dropped. The animations that follow it look their
    //     timeline up again.
    m_timelines.remove_all_matching([&](TimelineKey const& key, auto const&) {
        auto const* declaring_element = this->declaring_element(*key.element);
        return !declaring_element || !declaring_element->declaration_named(key.name);
    });
}

NamedTimelines::DeclaringElement const* NamedTimelines::declaring_element(DOM::Element const& element) const
{
    auto index = m_declaring_element_indices.get(&element);
    if (!index.has_value())
        return nullptr;
    return &m_declaring_elements[*index];
}

GC::Ref<ScrollTimeline> NamedTimelines::timeline_for(DOM::Element& element, Declaration const& declaration)
{
    TimelineKey key { element, declaration.name };
    if (auto existing = m_timelines.get(key); existing.has_value()) {
        // An element may only define one timeline per name, which keeps its identity while its axis and inset change.
        if (auto* view_timeline = as_if<ViewTimeline>(*existing.value()); view_timeline && declaration.is_view) {
            view_timeline->set_axis_and_inset(declaration.axis, declaration.view_inset);
            return *existing;
        }
        if (!is<ViewTimeline>(*existing.value()) && !declaration.is_view) {
            existing.value()->set_axis(declaration.axis);
            return *existing;
        }
    }

    HTML::TemporaryExecutionContext context { m_document->relevant_settings_object() };
    GC::Ref<ScrollTimeline> timeline = declaration.is_view
        ? GC::Ref<ScrollTimeline> { ViewTimeline::create(m_document, DOM::AbstractElement { element }, declaration.axis, declaration.view_inset) }
        : ScrollTimeline::create(m_document, GC::Ptr<DOM::Element const> { &element }, declaration.axis);
    m_timelines.set(key, timeline);
    return timeline;
}

// https://drafts.csswg.org/scroll-animations-1/#target-timeline
GC::Ptr<ScrollTimeline> NamedTimelines::target_timeline(DOM::AbstractElement const& element, Utf16FlyString const& name)
{
    refresh_declarations();

    // Whether |descendant| is a flat tree descendant of |ancestor| that a timeline name it defines is in scope for:
    // no element between them limits the scope of |name| to its own descendants.
    // INTEROP: The spec looks at all flat tree descendants, but like Chrome, a timeline inside an element that scopes
    //          its name, including the element itself, is only found by lookups within that element.
    auto is_in_scope_of = [&](DOM::Element const& descendant, DOM::Element const& ancestor) {
        for (auto const* element = &descendant; element; element = element->flat_tree_parent_element()) {
            if (element == &ancestor)
                return element != &descendant;
            if (auto const* declaring_element = this->declaring_element(*element); declaring_element && declaring_element->scopes(name))
                return false;
        }
        return false;
    };

    // The target timeline for a given element element referencing a timeline name name is:
    for (Optional<DOM::AbstractElement> current = element; current.has_value();) {
        auto& current_element = current->element();
        auto const* declaring_element = current->pseudo_element().has_value() ? nullptr : this->declaring_element(current_element);

        // If element defines a timeline matching name,
        if (declaring_element) {
            if (auto const* declaration = declaring_element->declaration_named(name)) {
                // That timeline.
                return timeline_for(current_element, *declaration);
            }
        }

        // Otherwise, if name is scoped by element, or if element is the document element,
        if ((declaring_element && declaring_element->scopes(name)) || (!current->pseudo_element().has_value() && &current_element == m_document->document_element())) {
            // The last defined timeline matching name among element's flat tree descendants in flat tree order, or,
            // if no such timeline exists, an inactive timeline.
            for (size_t i = m_declaring_elements.size(); i-- > 0;) {
                auto const& candidate = m_declaring_elements[i];
                auto const* declaration = candidate.declaration_named(name);
                if (declaration && is_in_scope_of(candidate.element, current_element))
                    return timeline_for(candidate.element, *declaration);
            }
            return nullptr;
        }

        // Otherwise, the target timeline for element's flat tree parent referencing name.
        auto* parent = current->flat_tree_parent_element();
        if (!parent)
            break;
        current = DOM::AbstractElement { *parent };
    }
    return nullptr;
}

void NamedTimelines::track_animation(CSS::CSSAnimation& animation)
{
    m_tracked_animations.set(animation);
}

bool NamedTimelines::update_tracked_animations()
{
    refresh_declarations();
    if (!m_tracked_animations_need_update)
        return false;
    m_tracked_animations_need_update = false;
    GC::RootVector<GC::Ref<CSS::CSSAnimation>> animations;
    for (auto& animation : m_tracked_animations)
        animations.append(animation);
    bool any_timeline_changed = false;
    for (auto& animation : animations) {
        auto previous_timeline = animation->timeline();
        animation->update_named_timeline();
        if (animation->timeline() != previous_timeline)
            any_timeline_changed = true;
    }
    return any_timeline_changed;
}

}
