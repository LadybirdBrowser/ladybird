/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/PannerNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(PannerNode);

PannerNode::~PannerNode() = default;

WebIDL::ExceptionOr<GC::Ref<PannerNode>> PannerNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, PannerOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-pannernode
WebIDL::ExceptionOr<GC::Ref<PannerNode>> PannerNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, PannerOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (options.ref_distance < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "refDistance cannot be negative"sv };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (options.rolloff_factor < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "rolloffFactor cannot be negative"sv };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
    // A RangeError exception MUST be thrown if this is set to a non-positive value.
    if (options.max_distance < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "maxDistance cannot be negative"sv };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
    // It is a linear value (not dB) in the range [0, 1]. An InvalidStateError MUST be thrown if the parameter is outside this range.
    if (options.cone_outer_gain < 0.0 || options.cone_outer_gain > 1.0)
        return WebIDL::InvalidStateError::create(realm, "coneOuterGain must be in the range of [0, 1]"_string);

    // Create the node and allocate memory
    auto node = realm.create<PannerNode>(realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#PannerNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::ClampedMax;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to maybe

    TRY(node->initialize_audio_node_options(options, default_options));
    return node;
}

PannerNode::PannerNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, PannerOptions const& options)
    : AudioNode(realm, context)
    , m_panning_model(options.panning_model)
    , m_position_x(AudioParam::create(realm, context, options.position_x, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_position_y(AudioParam::create(realm, context, options.position_y, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_position_z(AudioParam::create(realm, context, options.position_z, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_orientation_x(AudioParam::create(realm, context, options.orientation_x, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_orientation_y(AudioParam::create(realm, context, options.orientation_y, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_orientation_z(AudioParam::create(realm, context, options.orientation_z, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_distance_model(options.distance_model)
    , m_ref_distance(options.ref_distance)
    , m_max_distance(options.max_distance)
    , m_rolloff_factor(options.rolloff_factor)
    , m_cone_inner_angle(options.cone_inner_angle)
    , m_cone_outer_angle(options.cone_outer_angle)
    , m_cone_outer_gain(options.cone_outer_gain)
{
}

void PannerNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PannerNode);
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

// https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
WebIDL::ExceptionOr<void> PannerNode::set_ref_distance(double value)
{
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (value < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "refDistance cannot be negative"sv };

    m_ref_distance = value;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
WebIDL::ExceptionOr<void> PannerNode::set_max_distance(double value)
{
    // A RangeError exception MUST be thrown if this is set to a non-positive value.
    if (value < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "maxDistance cannot be negative"sv };

    m_max_distance = value;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
WebIDL::ExceptionOr<void> PannerNode::set_rolloff_factor(double value)
{
    // A RangeError exception MUST be thrown if this is set to a negative value.
    if (value < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "rolloffFactor cannot be negative"sv };

    m_rolloff_factor = value;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
WebIDL::ExceptionOr<void> PannerNode::set_cone_outer_gain(double value)
{
    // It is a linear value (not dB) in the range [0, 1]. An InvalidStateError MUST be thrown if the parameter is outside this range.
    if (value < 0.0 || value > 1.0)
        return WebIDL::InvalidStateError::create(realm(), "coneOuterGain must be in the range of [0, 1]"_string);

    m_cone_outer_gain = value;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-setposition
WebIDL::ExceptionOr<void> PannerNode::set_position(float x, float y, float z)
{
    // This method is DEPRECATED. It is equivalent to setting positionX.value, positionY.value, and positionZ.value
    // attribute directly with the x, y and z parameters, respectively.
    // FIXME: Consequently, if any of the positionX, positionY, and positionZ AudioParams have an automation curve
    //        set using setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be thrown.
    m_position_x->set_value(x);
    m_position_y->set_value(y);
    m_position_z->set_value(z);
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-pannernode-setorientation
WebIDL::ExceptionOr<void> PannerNode::set_orientation(float x, float y, float z)
{
    // This method is DEPRECATED. It is equivalent to setting orientationX.value, orientationY.value, and
    // orientationZ.value attribute directly, with the x, y and z parameters, respectively.
    // FIXME: Consequently, if any of the orientationX, orientationY, and orientationZ AudioParams have an automation
    //        curve set using setValueCurveAtTime() at the time this method is called, a NotSupportedError MUST be thrown.
    m_orientation_x->set_value(x);
    m_orientation_y->set_value(y);
    m_orientation_z->set_value(z);
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcountmode
WebIDL::ExceptionOr<void> PannerNode::set_channel_count_mode(Bindings::ChannelCountMode mode)
{
    if (mode == Bindings::ChannelCountMode::Max) {
        return WebIDL::NotSupportedError::create(realm(), "PannerNode does not support 'max' as channelCountMode."_string);
    }

    return AudioNode::set_channel_count_mode(mode);
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> PannerNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    if (channel_count > 2) {
        return WebIDL::NotSupportedError::create(realm(), "PannerNode does not support channel count greater than 2"_string);
    }

    return AudioNode::set_channel_count(channel_count);
}

}
