/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/CompositorAnimationEffectState.h>

namespace Web::Painting {

struct CompositorAnimationKeyframes::Data {
    AK_ALLOC_WITH_KMALLOC;

    Data(Animations::KeyframeEffect const& effect, Animations::Animation const& animation, DOM::AbstractElement target)
        : effect(effect)
        , animation(animation)
        , target(target)
    {
    }

    GC::Ref<Animations::KeyframeEffect const> effect;
    GC::Ref<Animations::Animation const> animation;
    DOM::AbstractElement target;
    // The linear easing control points each keyframe's descriptor borrows.
    Vector<Vector<Compositing::RustFFI::FfiLinearEasingPoint>> linear_easing_points;
    Vector<Layout::RustFFI::FfiCompositorAnimationKeyframe> keyframes;
    Vector<Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame const*> entries;
};

static Layout::RustFFI::FfiCompositorKeyframeValueState keyframe_value_state(Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame const& entry, CSS::PropertyID property_id)
{
    auto value = entry.properties.get(CSS::PropertyNameAndID::from_id(property_id));
    if (!value.has_value())
        return Layout::RustFFI::FfiCompositorKeyframeValueState::Absent;
    if (value->has<Animations::KeyframeEffect::KeyFrameSet::UseInitial>())
        return Layout::RustFFI::FfiCompositorKeyframeValueState::UsesUnderlyingStyle;
    return Layout::RustFFI::FfiCompositorKeyframeValueState::Present;
}

CompositorAnimationKeyframes::CompositorAnimationKeyframes(Animations::KeyframeEffect const& effect, Animations::Animation const& animation, DOM::AbstractElement target)
    : m_data(make<Data>(effect, animation, target))
{
    auto const* key_frame_set = effect.key_frame_set();
    if (!key_frame_set)
        return;
    auto keyframe_count = key_frame_set->keyframes_by_key.size();
    m_data->linear_easing_points.ensure_capacity(keyframe_count);
    m_data->keyframes.ensure_capacity(keyframe_count);
    m_data->entries.ensure_capacity(keyframe_count);
    for (auto it = key_frame_set->keyframes_by_key.begin(); it != key_frame_set->keyframes_by_key.end(); ++it) {
        auto const& entry = *it;
        m_data->entries.unchecked_append(&entry);
        Layout::RustFFI::FfiCompositorAnimationKeyframe keyframe {};
        keyframe.offset = static_cast<double>(it.key()) / (100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor);
        auto composite = [&] {
            switch (entry.composite) {
            case Bindings::CompositeOperationOrAuto::Replace:
                return Bindings::CompositeOperation::Replace;
            case Bindings::CompositeOperationOrAuto::Add:
                return Bindings::CompositeOperation::Add;
            case Bindings::CompositeOperationOrAuto::Accumulate:
                return Bindings::CompositeOperation::Accumulate;
            case Bindings::CompositeOperationOrAuto::Auto:
                return effect.composite();
            }
            VERIFY_NOT_REACHED();
        }();
        keyframe.composite_is_replace = composite == Bindings::CompositeOperation::Replace;
        m_data->linear_easing_points.unchecked_append({});
        auto& linear_easing_points = m_data->linear_easing_points.last();
        auto describe_easing = [&](CSS::EasingFunction const& easing) {
            return CSS::to_ffi_easing_descriptor<Compositing::RustFFI::FfiEasingDescriptor>(easing, linear_easing_points);
        };
        keyframe.easing_is_supported = entry.easing.visit(
            [&](Empty) {
                auto easing = animation.is_css_animation()
                    ? static_cast<CSS::CSSAnimation const&>(animation).default_easing()
                    : CSS::EasingFunction::linear();
                keyframe.easing = describe_easing(easing);
                return true;
            },
            [&](CSS::EasingFunction const& easing) {
                keyframe.easing = describe_easing(easing);
                return true;
            },
            [&](CSS::RustStyleValueHandle const&) {
                keyframe.easing = describe_easing(CSS::EasingFunction::linear());
                return false;
            });
        keyframe.opacity = keyframe_value_state(entry, CSS::PropertyID::Opacity);
        keyframe.background_color = keyframe_value_state(entry, CSS::PropertyID::BackgroundColor);
        keyframe.filter = keyframe_value_state(entry, CSS::PropertyID::Filter);
        keyframe.translate = keyframe_value_state(entry, CSS::PropertyID::Translate);
        keyframe.rotate = keyframe_value_state(entry, CSS::PropertyID::Rotate);
        keyframe.scale = keyframe_value_state(entry, CSS::PropertyID::Scale);
        keyframe.transform = keyframe_value_state(entry, CSS::PropertyID::Transform);
        m_data->keyframes.unchecked_append(keyframe);
    }
}

CompositorAnimationKeyframes::~CompositorAnimationKeyframes() = default;

static RefPtr<CSS::StyleValue const> resolved_compositor_animation_style_value(CSS::PropertyID property_id, CSS::RustStyleValueHandle const& value, DOM::AbstractElement target)
{
    ++target.document().style_invalidation_counters().compositor_keyframe_value_resolutions;
    auto style_value = CSS::StyleValue::adopt_rust_style_value_data(CSS::StyleValueFFI::rust_style_value_retain(value.data()));
    if (style_value->is_unresolved())
        style_value = target.document().style_computer().resolve_unresolved_style_value(target, CSS::PropertyNameAndID::from_id(property_id), style_value->as_unresolved());
    if (style_value->is_guaranteed_invalid() || style_value->is_unresolved() || style_value->is_pending_substitution())
        return nullptr;
    CSS::ComputationContext computation_context {
        .length_resolution_context = CSS::Length::ResolutionContext::for_element(target),
        .abstract_element = target,
    };
    return style_value->absolutized(computation_context);
}

static void const* resolved_compositor_keyframe_value(void* context, size_t keyframe_index, u16 property_id, bool uses_underlying_style)
{
    auto& data = *static_cast<CompositorAnimationKeyframes::Data*>(context);
    auto property = static_cast<CSS::PropertyID>(property_id);
    RefPtr<CSS::StyleValue const> style_value;
    if (uses_underlying_style) {
        // NB: Synthesized endpoints use the underlying style, just as main-thread keyframe sampling does.
        auto computed_style = data.target.computed_style();
        if (!computed_style)
            return nullptr;
        style_value = computed_style->computed_style_value(property, CSS::ComputedValues::WithAnimationsApplied::No);
    } else {
        auto value = data.entries[keyframe_index]->properties.get(CSS::PropertyNameAndID::from_id(property));
        if (!value.has_value() || !value->has<CSS::RustStyleValueHandle>())
            return nullptr;
        style_value = resolved_compositor_animation_style_value(property, value->get<CSS::RustStyleValueHandle>(), data.target);
    }
    if (!style_value)
        return nullptr;
    return CSS::StyleValueFFI::rust_style_value_retain(style_value->rust_style_value_data());
}

static bool resolve_compositor_animation_color(void* context, void const* value, Gfx::Color* color)
{
    auto& data = *static_cast<CompositorAnimationKeyframes::Data*>(context);
    auto style_value = CSS::StyleValue::adopt_rust_style_value_data(CSS::StyleValueFFI::rust_style_value_retain(static_cast<CSS::StyleValueFFI::StyleValueData const*>(value)));
    auto resolved = style_value->to_color(CSS::ColorResolutionContext::for_element(data.target));
    if (!resolved.has_value())
        return false;
    *color = *resolved;
    return true;
}

static Layout::RustFFI::FfiCompositorAnimationHost compositor_animation_host(CompositorAnimationKeyframes::Data& data)
{
    return {
        .context = &data,
        .resolved_keyframe_value = resolved_compositor_keyframe_value,
        .resolve_color = resolve_compositor_animation_color,
    };
}

static Layout::RustFFI::FfiCompositorAnimationRequest compositor_animation_request(CompositorAnimationKeyframes::Data const& data, Layout::Node const& layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind target_kind)
{
    auto const& effect = *data.effect;
    Layout::RustFFI::FfiCompositorAnimationRequest request {};
    request.target_kind = target_kind;
    request.layout_node = Layout::Node::slot_id(&layout_node);
    request.keyframes = data.keyframes.data();
    request.keyframe_count = data.keyframes.size();
    request.key_frame_set_identity = reinterpret_cast<uintptr_t>(effect.key_frame_set());
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
    auto request = compositor_animation_request(*m_data, layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind::Transform);
    auto host = compositor_animation_host(*m_data);
    return Layout::RustFFI::compositor_animation_effect_transform_preserves_axes(&request, &host);
}

bool CompositorAnimationKeyframes::only_translates_horizontally(Layout::Node const& layout_node) const
{
    auto request = compositor_animation_request(*m_data, layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind::Transform);
    auto host = compositor_animation_host(*m_data);
    return Layout::RustFFI::compositor_animation_effect_only_translates_horizontally(&request, &host);
}

static void* layout_arena_handle(DOM::Document& document)
{
    return document.layout_node_arena().handle();
}

CompositorAnimationEffectState::CompositorAnimationEffectState()
    : m_handle(Layout::RustFFI::compositor_animation_effect_state_create())
{
}

CompositorAnimationEffectState::~CompositorAnimationEffectState()
{
    Layout::RustFFI::compositor_animation_effect_state_destroy(m_handle);
}

CompositorAnimationEffectState::BuildOutcome CompositorAnimationEffectState::build(CompositorAnimationKeyframes const& keyframes, Layout::Node const& layout_node, Compositing::RustFFI::FfiVisualAnimationTargetKind target_kind, TimingAnchor timing_anchor)
{
    auto& data = *keyframes.m_data;
    auto const& effect = *data.effect;
    auto request = compositor_animation_request(data, layout_node, target_kind);
    Vector<Compositing::RustFFI::FfiLinearEasingPoint> effect_easing_points;
    request.timing.monotonic_time_at_anchor_ns = static_cast<i64>(timing_anchor.monotonic_time_ms * 1'000'000.0);
    request.timing.local_time_at_anchor_ms = timing_anchor.local_time_ms;
    request.timing.playback_rate = data.animation->playback_rate();
    request.timing.start_delay_ms = effect.start_delay().value;
    request.timing.iteration_duration_ms = effect.iteration_duration().value;
    request.timing.iteration_count = effect.iteration_count();
    request.timing.iteration_start = effect.iteration_start();
    request.timing.playback_direction = static_cast<Compositing::RustFFI::FfiVisualAnimationPlaybackDirection>(to_underlying(effect.playback_direction()));
    request.timing.fill_mode = first_is_one_of(effect.fill_mode(), Bindings::FillMode::Backwards, Bindings::FillMode::Both)
        ? Compositing::RustFFI::FfiVisualAnimationFillMode::Backwards
        : Compositing::RustFFI::FfiVisualAnimationFillMode::None;
    request.timing.easing = CSS::to_ffi_easing_descriptor<Compositing::RustFFI::FfiEasingDescriptor>(effect.timing_function(), effect_easing_points);

    auto host = compositor_animation_host(data);
    auto outcome = Layout::RustFFI::compositor_animation_effect_build(m_handle, layout_arena_handle(data.target.document()), &request, &host);
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
    Layout::RustFFI::compositor_animation_effect_publish_pending(m_handle, layout_arena_handle(document), reuse_retained_timing_anchors == ReuseRetainedTimingAnchors::Yes);
}

bool CompositorAnimationEffectState::has_retained() const
{
    return Layout::RustFFI::compositor_animation_effect_has_retained(m_handle);
}

void CompositorAnimationEffectState::clear_retained()
{
    Layout::RustFFI::compositor_animation_effect_clear_retained(m_handle);
}

void CompositorAnimationEffectState::reset()
{
    Layout::RustFFI::compositor_animation_effect_reset(m_handle);
}

}
