/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioListenerPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioListener
class AudioListener final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioListener, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(AudioListener);

public:
    static JS::NonnullGCPtr<AudioListener> create(JS::Realm&);
    virtual ~AudioListener() override;

    JS::NonnullGCPtr<AudioParam> forward_x() const { return m_forward_x; }
    JS::NonnullGCPtr<AudioParam> forward_y() const { return m_forward_y; }
    JS::NonnullGCPtr<AudioParam> forward_z() const { return m_forward_z; }
    JS::NonnullGCPtr<AudioParam> position_x() const { return m_position_x; }
    JS::NonnullGCPtr<AudioParam> position_y() const { return m_position_y; }
    JS::NonnullGCPtr<AudioParam> position_z() const { return m_position_z; }
    JS::NonnullGCPtr<AudioParam> up_x() const { return m_up_x; }
    JS::NonnullGCPtr<AudioParam> up_y() const { return m_up_y; }
    JS::NonnullGCPtr<AudioParam> up_z() const { return m_up_z; }

    WebIDL::ExceptionOr<void> set_position(float x, float y, float z);
    WebIDL::ExceptionOr<void> set_orientation(float x, float y, float z, float x_up, float y_up, float z_up);

private:
    explicit AudioListener(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<AudioParam> m_forward_x;
    JS::NonnullGCPtr<AudioParam> m_forward_y;
    JS::NonnullGCPtr<AudioParam> m_forward_z;
    JS::NonnullGCPtr<AudioParam> m_position_x;
    JS::NonnullGCPtr<AudioParam> m_position_y;
    JS::NonnullGCPtr<AudioParam> m_position_z;
    JS::NonnullGCPtr<AudioParam> m_up_x;
    JS::NonnullGCPtr<AudioParam> m_up_y;
    JS::NonnullGCPtr<AudioParam> m_up_z;
};

}
