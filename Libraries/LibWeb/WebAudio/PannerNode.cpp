/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/AudioListener.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/PannerNode.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(PannerNode);

PannerNode::~PannerNode() = default;

void PannerNode::queue_panner_parameters_update()
{
    context()->queue_control_message(NodeMessage { SetPannerParameters {
        .node_id = node_id(),
        .panning_model = m_panning_model,
        .distance_model = m_distance_model,
        .ref_distance = m_ref_distance,
        .max_distance = m_max_distance,
        .rolloff_factor = m_rolloff_factor,
        .cone_inner_angle = m_cone_inner_angle,
        .cone_outer_angle = m_cone_outer_angle,
        .cone_outer_gain = m_cone_outer_gain,
    } });
}

WebIDL::ExceptionOr<GC::Ref<PannerNode>> PannerNode::create(GC::Ref<BaseAudioContext> context, PannerOptions const& options)
{
    // Create the node and allocate memory
    auto node = GC::Heap::the().allocate<PannerNode>(context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#PannerNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = ChannelCountMode::ClampedMax;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to maybe

    TRY(node->initialize_audio_node_options(options, default_options));

    auto const& listener = context->listener();
    node->queue_render_node_creation(make<Rendering::PannerRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size(),
        Rendering::PannerRenderNode::Params {
            node->m_position_x->render_param(),
            node->m_position_y->render_param(),
            node->m_position_z->render_param(),
            node->m_orientation_x->render_param(),
            node->m_orientation_y->render_param(),
            node->m_orientation_z->render_param(),
        },
        Rendering::PannerRenderNode::ListenerParams {
            listener->position_x()->render_param(),
            listener->position_y()->render_param(),
            listener->position_z()->render_param(),
            listener->forward_x()->render_param(),
            listener->forward_y()->render_param(),
            listener->forward_z()->render_param(),
            listener->up_x()->render_param(),
            listener->up_y()->render_param(),
            listener->up_z()->render_param(),
        }));
    node->queue_panner_parameters_update();

    return node;
}

WebIDL::ExceptionOr<void> PannerNode::validate_options(PannerOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (options.ref_distance < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "refDistance cannot be negative"_utf16 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (options.rolloff_factor < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "rolloffFactor cannot be negative"_utf16 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
    // A RangeError exception MUST be thrown if this is set to a non-positive value.
    if (options.max_distance <= 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "maxDistance must be positive"_utf16 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
    // It is a linear value (not dB) in the range [0, 1]. An InvalidStateError MUST be thrown if the parameter is outside this range.
    if (options.cone_outer_gain < 0.0 || options.cone_outer_gain > 1.0)
        return WebIDL::InvalidStateError::create("coneOuterGain must be in the range of [0, 1]"_utf16);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-pannernode
WebIDL::ExceptionOr<GC::Ref<PannerNode>> PannerNode::create_for_constructor(GC::Ref<BaseAudioContext> context, PannerOptions const& options)
{
    TRY(validate_options(options));
    return create(context, options);
}

PannerNode::PannerNode(GC::Ref<BaseAudioContext> context, PannerOptions const& options)
    : AudioNode(context)
    , m_panning_model(options.panning_model)
    , m_position_x(AudioParam::create(context, this, options.position_x, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
    , m_position_y(AudioParam::create(context, this, options.position_y, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
    , m_position_z(AudioParam::create(context, this, options.position_z, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
    , m_orientation_x(AudioParam::create(context, this, options.orientation_x, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
    , m_orientation_y(AudioParam::create(context, this, options.orientation_y, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
    , m_orientation_z(AudioParam::create(context, this, options.orientation_z, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
    , m_distance_model(options.distance_model)
    , m_ref_distance(options.ref_distance)
    , m_max_distance(options.max_distance)
    , m_rolloff_factor(options.rolloff_factor)
    , m_cone_inner_angle(options.cone_inner_angle)
    , m_cone_outer_angle(options.cone_outer_angle)
    , m_cone_outer_gain(options.cone_outer_gain)
{
}

void PannerNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_position_x);
    visitor.visit(m_position_y);
    visitor.visit(m_position_z);
    visitor.visit(m_orientation_x);
    visitor.visit(m_orientation_y);
    visitor.visit(m_orientation_z);
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-panningmodel
void PannerNode::set_panning_model(PanningModelType value)
{
    m_panning_model = value;
    queue_panner_parameters_update();
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-distancemodel
void PannerNode::set_distance_model(DistanceModelType value)
{
    m_distance_model = value;
    queue_panner_parameters_update();
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
WebIDL::ExceptionOr<void> PannerNode::set_ref_distance(double value)
{
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (value < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "refDistance cannot be negative"_utf16 };

    m_ref_distance = value;
    queue_panner_parameters_update();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
WebIDL::ExceptionOr<void> PannerNode::set_max_distance(double value)
{
    // A RangeError exception MUST be thrown if this is set to a non-positive value.
    if (value <= 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "maxDistance must be positive"_utf16 };

    m_max_distance = value;
    queue_panner_parameters_update();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
WebIDL::ExceptionOr<void> PannerNode::set_rolloff_factor(double value)
{
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (value < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "rolloffFactor cannot be negative"_utf16 };

    m_rolloff_factor = value;
    queue_panner_parameters_update();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
WebIDL::ExceptionOr<void> PannerNode::set_cone_outer_gain(double value)
{
    // It is a linear value (not dB) in the range [0, 1]. An InvalidStateError MUST be thrown if the parameter is outside this range.
    if (value < 0.0 || value > 1.0)
        return WebIDL::InvalidStateError::create("coneOuterGain must be in the range of [0, 1]"_utf16);

    m_cone_outer_gain = value;
    queue_panner_parameters_update();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-setposition
WebIDL::ExceptionOr<void> PannerNode::set_position(float x, float y, float z)
{
    // This method is DEPRECATED. It is equivalent to setting positionX.value, positionY.value, and positionZ.value
    // attribute directly with the x, y and z parameters, respectively.
    // Consequently, if any of the positionX, positionY, and positionZ AudioParams have an automation curve set using
    // setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be thrown.
    TRY(m_position_x->set_value(x));
    TRY(m_position_y->set_value(y));
    TRY(m_position_z->set_value(z));
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-setorientation
WebIDL::ExceptionOr<void> PannerNode::set_orientation(float x, float y, float z)
{
    // This method is DEPRECATED. It is equivalent to setting orientationX.value, orientationY.value, and
    // orientationZ.value attribute directly, with the x, y and z parameters, respectively.
    // Consequently, if any of the orientationX, orientationY, and orientationZ AudioParams have an automation curve set
    // using setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be thrown.
    TRY(m_orientation_x->set_value(x));
    TRY(m_orientation_y->set_value(y));
    TRY(m_orientation_z->set_value(z));
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
WebIDL::ExceptionOr<void> PannerNode::set_channel_count_mode(ChannelCountMode mode)
{
    if (mode == ChannelCountMode::Max) {
        return WebIDL::NotSupportedError::create("PannerNode does not support 'max' as channelCountMode."_utf16);
    }

    return AudioNode::set_channel_count_mode(mode);
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> PannerNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    if (channel_count > 2) {
        return WebIDL::NotSupportedError::create("PannerNode does not support channel count greater than 2"_utf16);
    }

    return AudioNode::set_channel_count(channel_count);
}

}
