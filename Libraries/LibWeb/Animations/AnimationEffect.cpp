/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Animations {

AnimationUpdateContext::ElementData::ElementData() = default;

AnimationUpdateContext::ElementData::ElementData(CSS::InstalledStyle style_before_update, RefPtr<CSS::ComputedStyleWorkingSet> target_style)
    : style_before_update(move(style_before_update))
    , target_style(move(target_style))
{
}

AnimationUpdateContext::ElementData::ElementData(ElementData&&) = default;

AnimationUpdateContext::ElementData& AnimationUpdateContext::ElementData::operator=(ElementData&&) = default;

AnimationUpdateContext::ElementData::~ElementData() = default;

void AnimationUpdateContext::ElementData::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto const& effect : effects)
        effect.visit_edges(visitor);
}

void AnimationUpdateContext::add_effect(DOM::AbstractElement abstract_element, EffectRef const& effect)
{
    auto& style_computer = abstract_element.element().document().style_computer();
    auto& element_data = elements.ensure(abstract_element, [&abstract_element, &style_computer] {
        auto const& style = abstract_element.installed_style();
        if (!style)
            return ElementData {};
        auto computed_properties = style_computer.reconstruct_computed_properties_for_animation(style);
        return ElementData { style, move(computed_properties) };
    });

    if (!element_data.target_style)
        return;
    element_data.effects.append(effect);
}

AnimationUpdateContext::AnimationUpdateContext() = default;

extern "C" {

// Declared in Bindings/Rust/src/animations/animation_effect.rs and keyframe_effect.rs, and in
// Bindings/Rust/src/dom/document.rs.
void web_animation_effect_visit_host_cells(RustFFI::AnimationEffectCell const*, void* visitor);
void web_animation_effect_set_target(RustFFI::AnimationEffectCell const*, DOM::Element*, bool update);
void web_animation_effect_set_keyframe_set(RustFFI::AnimationEffectCell const*, RustFFI::KeyframeSet const*);
DOM::Element* web_animation_element_of_wrapper(JSObject*);
JSObject* web_animation_wrap_element(void* realm, DOM::Element*);
DOM::Document* web_animation_document_of_wrapper(JSObject* document_wrapper);
}

void EffectRef::visit_edges(GC::Cell::Visitor& visitor) const
{
    if (auto* effect = static_cast<GC::Cell*>(RustFFI::animations_effect_object_cell(&record()))) {
        visitor.visit(effect);
        return;
    }
    visitor.visit(target());
}

void web_animation_effect_visit_host_cells(RustFFI::AnimationEffectCell const* record, void* visitor)
{
    auto& cell_visitor = *static_cast<GC::Cell::Visitor*>(visitor);
    EffectRef effect { *record };
    if (auto animation = effect.associated_animation(); animation.has_value())
        animation->visit_edges(cell_visitor);
    cell_visitor.visit(effect.target());
}

// Sets the target element, and where `update`, notes what the keyframes read of it and invalidates the style the effect
// produces.
void web_animation_effect_set_target(RustFFI::AnimationEffectCell const* record, DOM::Element* target, bool update)
{
    EffectRef effect { *record };
    // NB: The record associates the associated animation with the new target rather than the old.
    RustFFI::animations_effect_set_target_element(record, target, web_animation_perform_host_effect);
    if (!update)
        return;

    if (target)
        CSS::record_attr_names_read_by_keyframes(*target, effect.keyframe_set());
    effect.invalidate();
    // FIXME: We don't remove the animated style from the old target element as part of normal animated style update and
    //        it will remain "stuck" until its style is fully invalidated for some other reason.
}

// Sets the keyframes the effect runs, invalidating the style it produces where they change.
void web_animation_effect_set_keyframe_set(RustFFI::AnimationEffectCell const* record, RustFFI::KeyframeSet const* keyframe_set_pointer)
{
    EffectRef effect { *record };
    auto keyframe_set = KeyframeSetHandle::adopt(keyframe_set_pointer);
    if (effect.keyframe_set() == keyframe_set)
        return;

    RustFFI::animations_effect_set_keyframe_set(record, keyframe_set.ptr());
    if (auto* target = effect.target())
        CSS::record_attr_names_read_by_keyframes(*target, effect.keyframe_set());
    effect.invalidate();
}

DOM::Element* web_animation_element_of_wrapper(JSObject* wrapper)
{
    return Bindings::impl_from<DOM::Element>(JS::HostABI::object_from_abi(wrapper));
}

static JSObject* wrap_in_realm(void* realm, GC::Ref<Bindings::Wrappable> object)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto wrapper = Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, object);
    return JS::HostABI::object_to_abi(wrapper.ptr());
}

JSObject* web_animation_wrap_element(void* realm, DOM::Element* element)
{
    return wrap_in_realm(realm, *element);
}

DOM::Document* web_animation_document_of_wrapper(JSObject* document_wrapper)
{
    return Bindings::impl_from<DOM::Document>(JS::HostABI::object_from_abi(document_wrapper));
}

AnimationUpdateContext::~AnimationUpdateContext()
{
    publish();
}

void AnimationUpdateContext::publish()
{
    // Each element's effects, sampled over the style it holds.
    struct Sample {
        DOM::AbstractElement element;
        ElementData const& data;
        Vector<EffectRef> effects;

        void visit_edges(GC::Cell::Visitor& visitor)
        {
            element.visit_edges(visitor);
            for (auto const& effect : effects)
                effect.visit_edges(visitor);
        }
    };
    GC::RootVector<Sample> samples;
    for (auto& it : elements) {
        if (!it.value.target_style)
            continue;
        auto& element = it.key;
        GC::Ref<DOM::Element> target = element.element();
        // Disconnected elements no longer have a style-engine row to publish refreshed
        // animation style into.
        if (target->style_node_id() == 0)
            continue;
        // NB: The engine assigns no record to a pseudo-element that is not rendered, so the style a read of it holds
        //     has no record for its animations to compose over, and it reads as it computes to without them.
        if (auto pseudo_element = element.pseudo_element(); pseudo_element.has_value() && !CSS::is_element_reference_pseudo_element(*pseudo_element)) {
            auto& document = target->document();
            Layout::ForcedReadScope read_scope { document };
            Layout::BegunRead const& read = read_scope;
            if (!CSS::StyleEngineFFI::style_engine_assigns_pseudo_element_record(document.style_computer().style_engine().host(), &read, target->style_node_id().value(), CSS::pseudo_element_to_ffi(pseudo_element)))
                continue;
        }
        // Provisionally started transitions are not associated with the element yet, so they are
        // never among the collected effects, but their values are already part of the published
        // style. Collect them first, in composite order below every associated effect, or this
        // update would rebuild the style without them.
        Vector<EffectRef> effects_to_collect;
        RustFFI::FfiOwningElement owner {
            .element = target.ptr(),
            .pseudo_element = element.pseudo_element().has_value() ? to_underlying(*element.pseudo_element()) : u8 { 0 },
            .has_pseudo_element = element.pseudo_element().has_value(),
        };
        for (size_t index = 0;; ++index) {
            auto const* effect = RustFFI::animations_document_animations_proposed_transition_effect(&target->document().document_animations(), owner, index);
            if (!effect)
                break;
            effects_to_collect.append(EffectRef { *effect });
        }
        for (auto const& animation : target->associated_animations_in_composite_order()) {
            auto effect = animation.effect();
            if (!effect.has_value() || animation.play_state() == RustFFI::AnimationPlayState::Idle)
                continue;
            if (effect->target() != target.ptr() || effect->pseudo_element_type() != element.pseudo_element())
                continue;
            effects_to_collect.append(*effect);
        }
        // A dirty effect may have just become irrelevant. Include it once so rebuilding the
        // animated overlay removes its terminal contribution.
        for (auto& dirty_effect : it.value.effects) {
            if (!effects_to_collect.contains_slow(dirty_effect))
                effects_to_collect.append(dirty_effect);
        }
        samples.append({ element, it.value, move(effects_to_collect) });
    }

    // An element whose overlay the update publishes, and whether the record it installed may become its transition
    // baseline.
    struct Publication {
        Sample& sample;
        bool may_record_baseline { false };
    };
    Vector<Publication> publications;
    GC::RootVector<CSS::StyleComputer::SampledAnimationOverlay> overlays;
    Vector<CSS::StyleEngineFFI::FfiAnimationOverlayPublication> published;

    // The render owner samples every element of a document in one call, and publishes what it sampled in another,
    // before any of their styles is installed.
    for (size_t first = 0; first < samples.size();) {
        auto& document = samples[first].element.document();
        auto& style_computer = document.style_computer();
        Layout::ForcedReadScope read { document };
        GC::RootVector<CSS::StyleComputer::AnimationRefreshRequest> requests;
        size_t end = first;
        for (; end < samples.size() && &samples[end].element.document() == &document; ++end) {
            auto& sample = samples[end];
            if (sample.element.installed_style() != sample.data.style_before_update)
                continue;
            // NB: An element left with no effects, as where its style just removed its last animation, has nothing to
            //     sample, and what it animated of its custom properties ends here.
            if (sample.effects.is_empty())
                style_computer.stop_publishing_animated_custom_properties(sample.element);
            else
                requests.append({ sample.element, sample.effects.span(), *sample.data.target_style });
        }
        // A timeline script observed ahead of its current time in this task is sampled at that time, as an update of the
        // effects needing it is, or the style installed here and the one that update sampled would keep disagreeing about
        // an effect that ended between the two times.
        auto refresh = [&] { style_computer.refresh_animations_into_each(read, requests); };
        using Refresh = decltype(refresh);
        RustFFI::animations_document_animations_at_times_observed_ahead(&document.document_animations(), &refresh, [](void* context) { (*static_cast<Refresh*>(context))(); });

        publications.clear_with_capacity();
        overlays.clear_with_capacity();
        for (auto& sample : samples.span().slice(first, end - first)) {
            auto const& style = *sample.data.target_style;
            if (sample.element.installed_style() != sample.data.style_before_update
                || !sample.element.installed_style().animation_overlay_changed(style.animated_overlay()))
                continue;
            bool const may_record_baseline = style.animated_overlay() && !CSS::ComputedValuesFFI::rust_animated_overlay_is_empty(style.animated_overlay())
                && document.is_in_style_stabilization_epoch();
            publications.append({ sample, may_record_baseline });
            overlays.append({ sample.element, style });
        }
        published.resize(overlays.size());
        style_computer.publish_sampled_animation_overlays(read, overlays, published);
        // NB: Each publication hands over a reference to the style it published, which a later one can retire.
        Vector<CSS::InstalledStyle> published_styles;
        published_styles.ensure_capacity(published.size());
        for (auto const& publication : published)
            published_styles.unchecked_append(CSS::InstalledStyle::adopt(publication.record));
        first = end;

        for (size_t i = 0; i < publications.size(); ++i) {
            auto const& [sample, may_record_baseline] = publications[i];
            auto const& animated_property_invalidation = published[i].invalidation;
            auto& element = sample.element;
            auto const& data = sample.data;
            GC::Ref<DOM::Element> target = element.element();
            // An earlier entry already republished this style with the current animation values.
            bool const republished = element.installed_style() != data.style_before_update;
            if (!republished && may_record_baseline && (document.style_stabilization_has_style_reactions() || animated_property_invalidation.requires_base_style_recomputation))
                style_computer.record_transition_stabilization_baseline(element, data.style_before_update);
            if (republished)
                continue;
            auto invalidation = CSS::decode_style_invalidation(animated_property_invalidation.invalidation);
            target->refresh_computed_style(read, element.pseudo_element(), move(published_styles[i]));
            if (auto* svg_element = as_if<SVG::SVGElement>(*target); svg_element && !element.pseudo_element().has_value())
                svg_element->note_svg_paint_resource_description_may_have_changed();

            // Box-type, overflow, and text-alignment adjustments consume the unadjusted base values,
            // which an animation-only overlay update deliberately does not reconstruct. Publish an
            // exact feedback action so the ordinary reaction path re-cascades that base before the
            // frame becomes observable.
            if (animated_property_invalidation.requires_base_style_recomputation && !data.base_is_current)
                target->document().style_computer().style_engine().record_derived_element_style_input_change(target->style_node_id(), CSS::StyleEngine::PublishedStyle | CSS::StyleEngine::RecomputeStyle);

            // NB: Publishing the composition had the style engine restyle the element's pseudo-elements and the
            //     children that compute from what the composition moved, in its next pass.

            // NB: refresh_computed_style() already publishes the new record to the layout node. Only
            // inherited values and image resources require the additional C++ style side effects.
            auto apply_layout_node_style_side_effects = [&](Layout::NodeWithStyle& layout_node, CSS::StyleRecordID style_record) {
                if (animated_property_invalidation.requires_layout_node_style_application)
                    layout_node.apply_style(style_record);
                else if (animated_property_invalidation.requires_style_resource_update)
                    layout_node.attach_style_resources();
            };
            if (!element.pseudo_element().has_value()) {
                if (auto* layout_node = target->unsafe_layout_node(read))
                    apply_layout_node_style_side_effects(*layout_node, target->style_record_identity());
            } else if (auto pseudo_element_node = target->pseudo_element_unsafe_layout_node(read, element.pseudo_element().value())) {
                apply_layout_node_style_side_effects(*pseudo_element_node, target->style_record_identity(element.pseudo_element()));
            }

            if (invalidation.needs_relayout())
                target->set_needs_layout_update(DOM::SetNeedsLayoutReason::KeyframeEffect);
            if (invalidation.needs_layout_tree_rebuild()) {
                auto rebuild_root = element.pseudo_element().has_value()
                    ? CSS::LayoutTreeRebuildRoot::Parent
                    : invalidation.layout_tree_rebuild_root();
                target->set_needs_layout_tree_rebuild(read, DOM::SetNeedsLayoutTreeUpdateReason::KeyframeEffect, rebuild_root);
            }
            if (invalidation.accumulated_visual_contexts() != CSS::AccumulatedVisualContextInvalidation::None) {
                auto scope = invalidation.accumulated_visual_contexts() == CSS::AccumulatedVisualContextInvalidation::Rebuild
                    ? DOM::Document::AccumulatedVisualContextUpdateScope::Structure
                    : DOM::Document::AccumulatedVisualContextUpdateScope::Values;
                // NB: Element-reference pseudo elements (e.g. ::placeholder) are not synthetic, so schedule their
                //     layout node directly instead of going through the owning element.
                if (element.pseudo_element().has_value()) {
                    if (auto pseudo_element_node = target->pseudo_element_unsafe_layout_node(read, element.pseudo_element().value()))
                        element.document().schedule_accumulated_visual_context_update(*pseudo_element_node, scope);
                } else {
                    element.document().schedule_accumulated_visual_context_update(read, target, scope);
                }
            }

            auto* repaint_layout_node = element.pseudo_element().has_value()
                ? target->pseudo_element_unsafe_layout_node(read, *element.pseudo_element())
                : target->unsafe_layout_node(read);
            if (repaint_layout_node && Painting::has_committed_box(*repaint_layout_node))
                Painting::repaint_after_style_change(*repaint_layout_node, invalidation);
        }
    }
    elements.clear();
}

}
