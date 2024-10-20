/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/CellAllocator.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioListener.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioListener);

AudioListener::AudioListener(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
    , m_forward_x(AudioParam::create(realm, 0.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_forward_y(AudioParam::create(realm, 0.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_forward_z(AudioParam::create(realm, -1.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_position_x(AudioParam::create(realm, 0.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_position_y(AudioParam::create(realm, 0.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_position_z(AudioParam::create(realm, 0.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_up_x(AudioParam::create(realm, 0.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_up_y(AudioParam::create(realm, 1.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_up_z(AudioParam::create(realm, 0.f, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
{
}

JS::NonnullGCPtr<AudioListener> AudioListener::create(JS::Realm& realm)
{
    return realm.vm().heap().allocate<AudioListener>(realm, realm);
}

AudioListener::~AudioListener() = default;

// https://webaudio.github.io/web-audio-api/#dom-audiolistener-setposition
WebIDL::ExceptionOr<void> AudioListener::set_position(float x, float y, float z)
{
    // This method is DEPRECATED. It is equivalent to setting positionX.value, positionY.value, and
    // positionZ.value directly with the given x, y, and z values, respectively.

    // FIXME: Consequently, any of the positionX, positionY, and positionZ AudioParams for this
    //        AudioListener have an automation curve set using setValueCurveAtTime() at the time this
    //        method is called, a NotSupportedError MUST be thrown.

    m_position_x->set_value(x);
    m_position_y->set_value(y);
    m_position_z->set_value(z);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiolistener-setorientation
WebIDL::ExceptionOr<void> AudioListener::set_orientation(float x, float y, float z, float x_up, float y_up, float z_up)
{
    // This method is DEPRECATED. It is equivalent to setting forwardX.value, forwardY.value,
    // forwardZ.value, upX.value, upY.value, and upZ.value directly with the given x, y, z, xUp,
    // yUp, and zUp values, respectively.

    // FIXME: Consequently, if any of the forwardX, forwardY, forwardZ, upX, upY and upZ
    //        AudioParams have an automation curve set using setValueCurveAtTime() at the time this
    //        method is called, a NotSupportedError MUST be thrown.

    m_forward_x->set_value(x);
    m_forward_y->set_value(y);
    m_forward_z->set_value(z);
    m_up_x->set_value(x_up);
    m_up_y->set_value(y_up);
    m_up_z->set_value(z_up);

    return {};
}

void AudioListener::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioListener);
}

void AudioListener::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_forward_x);
    visitor.visit(m_forward_y);
    visitor.visit(m_forward_z);
    visitor.visit(m_position_x);
    visitor.visit(m_position_y);
    visitor.visit(m_position_z);
    visitor.visit(m_up_x);
    visitor.visit(m_up_y);
    visitor.visit(m_up_z);
}

}
