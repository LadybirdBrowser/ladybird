/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Animations/NamedTimelines.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>

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
    visitor.visit(m_declaring_element_indices);
    for (auto& [key, timeline] : m_timelines)
        visitor.visit(key.element);
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
            for_each_flat_tree_inclusive_descendant(*slottable, callback);
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
            // NB: The style engine hands over a reference to a calculated inset, which may be null.
            auto retain = [](void const* value) {
                return value ? CSS::RustStyleValueHandle::retained(static_cast<CSS::StyleValueFFI::StyleValueData const*>(value)) : CSS::RustStyleValueHandle {};
            };
            auto const& inset = declaration->view_inset;
            static_cast<DeclaringElement*>(context)->declarations.append({
                .name = CSS::css_string_from_rust(declaration->name),
                .is_view = declaration->is_view,
                .axis = declaration->axis,
                .view_inset_start = { inset.start_is_auto, inset.start_px, inset.start_percentage, retain(inset.start_calculated) },
                .view_inset_end = { inset.end_is_auto, inset.end_px, inset.end_percentage, retain(inset.end_calculated) },
            });
        });
        auto scope = CSS::ComputedValuesFFI::rust_animation_values_timeline_scope(&values, &declaring_element, [](void* context, CSS::StyleValueFFI::CssString const* name) {
            static_cast<DeclaringElement*>(context)->scoped_names.append(CSS::css_string_from_rust(name));
        });
        declaring_element.scopes_all_names = scope == CSS::ComputedValuesFFI::FfiTimelineScope::All;

        m_declaring_element_indices.set(*element, m_declaring_elements.size());
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
    auto index = m_declaring_element_indices.get(element);
    if (!index.has_value())
        return nullptr;
    return &m_declaring_elements[*index];
}

TimelineView NamedTimelines::timeline_for(DOM::Element& element, Declaration const& declaration)
{
    auto edge = [](ViewTimelineInsetEdge const& edge) -> RustFFI::FfiViewTimelineInsetEdge {
        return { .is_auto = edge.is_auto, .px = edge.px, .percentage = edge.percentage, .calculated = edge.calculated.data() };
    };
    RustFFI::FfiViewTimelineInset inset { .start = edge(declaration.view_inset_start), .end = edge(declaration.view_inset_end) };

    TimelineKey key { element, declaration.name };
    if (auto existing = m_timelines.get(key); existing.has_value() && existing->is_view_timeline() == declaration.is_view) {
        // An element may only define one timeline per name, which keeps its identity while its axis and inset change.
        // NB: The animations the timeline updates as they change stay alive through it, which can run script.
        GC::RootVector<AnimationRef> updated_animations;
        RustFFI::FfiTimelineAnimationsHost host { .context = &updated_animations, .keep_alive = keep_animation_alive_in, .perform = web_animation_perform_host_effect };
        RustFFI::animations_scroll_timeline_set_axis_and_inset(&existing->record(), declaration.axis, declaration.is_view ? &inset : nullptr, &host);
        return *existing;
    }

    RustFFI::FfiTimelineDescription description {
        .kind = declaration.is_view ? RustFFI::FfiTimelineKind::View : RustFFI::FfiTimelineKind::Scroll,
        .origin_time = 0,
        .source = declaration.is_view
            ? RustFFI::FfiScrollTimelineSource { .element = nullptr, .is_anonymous = true, .scroller = to_underlying(CSS::Scroller::Nearest), .target = &element, .pseudo_element = 0, .has_pseudo_element = false }
            : RustFFI::FfiScrollTimelineSource { .element = &element, .is_anonymous = false, .scroller = 0, .target = nullptr, .pseudo_element = 0, .has_pseudo_element = false },
        .axis = declaration.axis,
        .inset = inset,
    };
    TimelineView timeline { *RustFFI::animations_document_create_timeline(m_document.ptr(), &description) };
    m_timelines.set(key, timeline);
    return timeline;
}

// https://drafts.csswg.org/scroll-animations-1/#target-timeline
Optional<TimelineView> NamedTimelines::target_timeline(DOM::AbstractElement const& element, Utf16FlyString const& name)
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
            return {};
        }

        // Otherwise, the target timeline for element's flat tree parent referencing name.
        auto* parent = current->flat_tree_parent_element();
        if (!parent)
            break;
        current = DOM::AbstractElement { *parent };
    }
    return {};
}

namespace {

// What looking up the named timelines of the CSS animations of the document again asks of the host.
struct NamedTimelinesHostContext {
    GC::Ref<NamedTimelines> named_timelines;
    // NB: The animations stay alive while their timelines are looked up, which can run script.
    GC::RootVector<AnimationRef> animations;
};

RustFFI::TimelineCell const* target_timeline_of_owner(void* context, RustFFI::FfiOwningElement owner, size_t name)
{
    DOM::AbstractElement target { *static_cast<DOM::Element*>(owner.element), owner.has_pseudo_element ? static_cast<CSS::PseudoElement>(owner.pseudo_element) : Optional<CSS::PseudoElement> {} };
    auto timeline = static_cast<NamedTimelinesHostContext*>(context)->named_timelines->target_timeline(target, Utf16FlyString::from_raw(name));
    return timeline.has_value() ? &timeline->record() : nullptr;
}

void keep_alive(void* context, RustFFI::AnimationCell const* animation)
{
    keep_animation_alive_in(&static_cast<NamedTimelinesHostContext*>(context)->animations, animation);
}

}

bool NamedTimelines::update_tracked_animations()
{
    refresh_declarations();
    if (!m_tracked_animations_need_update)
        return false;
    m_tracked_animations_need_update = false;
    NamedTimelinesHostContext context { *this, {} };
    RustFFI::FfiNamedTimelinesHost host {
        .context = &context,
        .target_timeline = target_timeline_of_owner,
        .keep_alive = keep_alive,
        .in_execution_context = in_execution_context_of_animation,
        .perform = web_animation_perform_host_effect,
    };
    return m_document->update_named_timelines_of_animations(host);
}

}
