/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/RecordHandles.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/CompositorAnimationEffectState.h>

namespace Web::Painting {

struct CompositorAnimationKeyframes::Data {
    AK_ALLOC_WITH_KMALLOC;

    Data(Animations::EffectRef effect, Animations::AnimationRef animation, DOM::AbstractElement target)
        : effect(move(effect))
        , animation(move(animation))
        , target(target)
    {
    }

    Animations::EffectRef effect;
    Animations::AnimationRef animation;
    DOM::AbstractElement target;
    // The keyframes the builder reads, which the build keeps live.
    Animations::KeyframeSetHandle keyframe_set;
    // The easing a keyframe that declares none runs, with the linear easing control points its descriptor borrows.
    Vector<Compositing::RustFFI::FfiLinearEasingPoint> default_easing_points;
    Compositing::RustFFI::FfiEasingDescriptor default_easing {};
};

CompositorAnimationKeyframes::CompositorAnimationKeyframes(Animations::EffectRef const& effect, Animations::AnimationRef const& animation, DOM::AbstractElement target)
    : m_data(make<Data>(effect, animation, target))
{
    m_data->keyframe_set = Animations::KeyframeSetHandle::retained(effect.keyframe_set().ptr());
    if (animation.timing_state().kind != Animations::RustFFI::FfiAnimationKind::CssAnimation) {
        m_data->default_easing = CSS::EasingFunction::linear().descriptor_with_points_in(m_data->default_easing_points);
        return;
    }
    auto default_easing = Animations::RustFFI::animations_css_animation_state(&animation.record()).default_easing;
    m_data->default_easing_points.append(default_easing.linear_points, default_easing.linear_point_count);
    default_easing.linear_points = m_data->default_easing_points.data();
    m_data->default_easing = default_easing;
}

CompositorAnimationKeyframes::~CompositorAnimationKeyframes() = default;

void CompositorAnimationKeyframes::visit_edges(GC::Cell::Visitor& visitor) const
{
    m_data->effect.visit_edges(visitor);
    m_data->animation.visit_edges(visitor);
    m_data->target.visit_edges(visitor);
}

static RefPtr<CSS::StyleValue const> resolved_compositor_animation_style_value(Layout::BegunRead const& read, CSS::PropertyID property_id, CSS::StyleValueFFI::StyleValueData const* value, DOM::AbstractElement target)
{
    HTML::note_rendering_trace_event("compositor keyframe values resolved"sv);
    auto style_value = CSS::StyleValue::adopt_rust_style_value_data(CSS::StyleValueFFI::rust_style_value_retain(value));
    if (style_value->is_unresolved()) {
        // The engine substitutes the value as a sample on the main thread would, against the custom properties the
        // element holds and inherits.
        auto custom_property_data = target.custom_property_data();
        auto inheritance_parent = target.element_to_inherit_style_from();
        auto inheritance_custom_property_data = inheritance_parent.has_value() ? inheritance_parent->custom_property_data() : nullptr;
        auto const* substituted = CSS::StyleValueFFI::rust_substitute_compositor_keyframe_value(
            target.document().style_computer().style_engine().host(),
            &read, target.element().style_node_id().value(),
            CSS::pseudo_element_to_ffi(target.pseudo_element()),
            custom_property_data ? custom_property_data->rust_store() : nullptr,
            inheritance_custom_property_data ? inheritance_custom_property_data->rust_store() : nullptr,
            to_underlying(property_id),
            value);
        if (!substituted)
            return nullptr;
        style_value = CSS::StyleValue::adopt_rust_style_value_data(substituted);
    }
    if (style_value->is_guaranteed_invalid() || style_value->is_unresolved() || style_value->is_pending_substitution())
        return nullptr;
    // Absolutizing against the target's used color-scheme collapses light-dark() as computing the value would, so
    // the color the builder resolves needs no length context.
    auto const* inherited_ui_values = target.style_group<CSS::ComputedValues::InheritedUIValues>();
    CSS::ComputationContext computation_context {
        .length_resolution_context = CSS::Length::ResolutionContext::for_element(target),
        .abstract_element = target,
        .color_scheme = inherited_ui_values ? inherited_ui_values->color_scheme_value() : Optional<CSS::PreferredColorScheme> {},
    };
    return style_value->absolutized(computation_context);
}

// What a build lends the callbacks the animation's host makes: the keyframes it builds from, in the read it builds in.
struct CompositorAnimationBuild {
    CompositorAnimationKeyframes::Data& data;
    Layout::BegunRead const& read;
};

static void const* resolved_compositor_keyframe_value(void* context, u16 property_id, void const* declared_value)
{
    auto& [data, read] = *static_cast<CompositorAnimationBuild*>(context);
    auto property = static_cast<CSS::PropertyID>(property_id);
    RefPtr<CSS::StyleValue const> style_value;
    if (!declared_value) {
        // NB: Synthesized endpoints use the underlying style, just as main-thread keyframe sampling does.
        auto computed_style = data.target.computed_style();
        if (!computed_style)
            return nullptr;
        style_value = computed_style->computed_style_value(property, CSS::ComputedValues::WithAnimationsApplied::No);
    } else {
        style_value = resolved_compositor_animation_style_value(read, property, static_cast<CSS::StyleValueFFI::StyleValueData const*>(declared_value), data.target);
    }
    if (!style_value)
        return nullptr;
    return CSS::StyleValueFFI::rust_style_value_retain(style_value->rust_style_value_data());
}

static Layout::RustFFI::FfiCompositorAnimationHost compositor_animation_host(CompositorAnimationBuild& lent)
{
    return {
        .context = &lent,
        .resolved_keyframe_value = resolved_compositor_keyframe_value,
    };
}

static Layout::RustFFI::FfiCompositorAnimationRequest compositor_animation_request(CompositorAnimationKeyframes::Data const& data, Layout::Node const& layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind target_kind)
{
    auto const& effect = data.effect;
    Layout::RustFFI::FfiCompositorAnimationRequest request {};
    request.target_kind = target_kind;
    request.layout_node = Layout::Node::slot_id(&layout_node);
    auto target_color_resolution_style = CSS::ColorResolutionStyle::for_element(data.target);
    request.target_inherited_ui_values = target_color_resolution_style.inherited_ui_values;
    request.target_inherited_text_values = target_color_resolution_style.inherited_text_values;
    request.keyframe_set = data.keyframe_set.ptr();
    request.effect_composite_is_replace = effect.composite_is_replace();
    request.default_easing = data.default_easing;
    request.target_style_generation = data.target.element().animation_style_generation();
    request.style_environment_version = data.target.document().style_computer().style_environment_version_for_sharing();
    if (target_kind == Compositing::RustFFI::FfiVisualAnimationTargetKind::Transform) {
        auto reference_box_size = transform_reference_box(layout_node).size();
        request.reference_box_width = reference_box_size.width().to_float();
        request.reference_box_height = reference_box_size.height().to_float();
    }
    request.device_pixels_per_css_pixel = static_cast<float>(data.target.document().page().client().device_pixels_per_css_pixel());
    for (auto const& property : effect.target_properties()) {
        switch (property.id()) {
        case CSS::PropertyID::Translate:
            request.targeted_transform_properties |= Layout::RustFFI::TARGETED_TRANSFORM_PROPERTY_TRANSLATE;
            break;
        case CSS::PropertyID::Rotate:
            request.targeted_transform_properties |= Layout::RustFFI::TARGETED_TRANSFORM_PROPERTY_ROTATE;
            break;
        case CSS::PropertyID::Scale:
            request.targeted_transform_properties |= Layout::RustFFI::TARGETED_TRANSFORM_PROPERTY_SCALE;
            break;
        case CSS::PropertyID::Transform:
            request.targeted_transform_properties |= Layout::RustFFI::TARGETED_TRANSFORM_PROPERTY_TRANSFORM;
            break;
        default:
            break;
        }
    }
    request.targets_only_transform = effect.target_properties().size() == 1
        && effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Transform));
    return request;
}

bool CompositorAnimationKeyframes::transform_preserves_axes(Layout::Node const& layout_node) const
{
    CompositorAnimationBuild lent { *m_data, layout_node.held_read() };
    auto request = compositor_animation_request(*m_data, layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind::Transform);
    auto host = compositor_animation_host(lent);
    return Layout::RustFFI::compositor_animation_effect_transform_preserves_axes(&request, &host);
}

bool CompositorAnimationKeyframes::only_translates_horizontally(Layout::Node const& layout_node) const
{
    CompositorAnimationBuild lent { *m_data, layout_node.held_read() };
    auto request = compositor_animation_request(*m_data, layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind::Transform);
    auto host = compositor_animation_host(lent);
    return Layout::RustFFI::compositor_animation_effect_only_translates_horizontally(&request, &host);
}

static Layout::RustFFI::DocumentHost* document_host(DOM::Document& document)
{
    return document.layout_node_arena().host();
}

CompositorAnimationEffectState::BuildOutcome CompositorAnimationEffectState::build(Layout::BegunRead const& read, CompositorAnimationKeyframes const& keyframes, Layout::Node const& layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind target_kind, TimingAnchor timing_anchor)
{
    auto& data = *keyframes.m_data;
    auto const& effect = data.effect;
    auto request = compositor_animation_request(data, layout_node, target_kind);
    Vector<Compositing::RustFFI::FfiLinearEasingPoint> effect_easing_points;
    request.timing.monotonic_time_at_anchor_ns = static_cast<i64>(timing_anchor.monotonic_time_ms * 1'000'000.0);
    request.timing.local_time_at_anchor_ms = timing_anchor.local_time_ms;
    request.timing.playback_rate = Animations::RustFFI::animations_animation_playback(&data.animation.record()).playback_rate;
    request.timing.effect = effect.timing();
    request.timing.easing = effect.timing_function_with_points_in(effect_easing_points);

    CompositorAnimationBuild lent { data, read };
    auto host = compositor_animation_host(lent);
    auto outcome = Layout::RustFFI::compositor_animation_effect_build(m_handle, document_host(data.target.document()), &request, &host);
    return {
        .built = outcome.built,
        .missing_visual_context_node = outcome.missing_visual_context_node,
        .only_translates_horizontally = outcome.only_translates_horizontally_is_known ? Optional<bool> { outcome.only_translates_horizontally } : Optional<bool> {},
    };
}

void CompositorAnimationEffectState::discard_pending(Compositing::RustFFI::FfiVisualAnimationTargetKind target_kind)
{
    Layout::RustFFI::compositor_animation_effect_discard_pending(m_handle, target_kind);
}

bool CompositorAnimationEffectState::has_pending() const
{
    return Layout::RustFFI::compositor_animation_effect_has_pending(m_handle);
}

void CompositorAnimationEffectState::clear_pending()
{
    Layout::RustFFI::compositor_animation_effect_clear_pending(m_handle);
}

void CompositorAnimationEffectState::publish_pending(DOM::Document& document, ReuseRetainedTimingAnchors reuse_retained_timing_anchors)
{
    Layout::RustFFI::compositor_animation_effect_publish_pending(m_handle, document.layout_node_arena().host(), reuse_retained_timing_anchors == ReuseRetainedTimingAnchors::Yes);
}

bool CompositorAnimationEffectState::has_retained() const
{
    return Layout::RustFFI::compositor_animation_effect_has_retained(m_handle);
}

void CompositorAnimationEffectState::clear_retained()
{
    Layout::RustFFI::compositor_animation_effect_clear_retained(m_handle);
}

}
