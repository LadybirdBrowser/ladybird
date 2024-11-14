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
    GC_DECLARE_ALLOCATOR(AudioListener);

public:
    static GC::Ref<AudioListener> create(JS::Realm&);
    virtual ~AudioListener() override;

    GC::Ref<AudioParam> forward_x() const { return m_forward_x; }
    GC::Ref<AudioParam> forward_y() const { return m_forward_y; }
    GC::Ref<AudioParam> forward_z() const { return m_forward_z; }
    GC::Ref<AudioParam> position_x() const { return m_position_x; }
    GC::Ref<AudioParam> position_y() const { return m_position_y; }
    GC::Ref<AudioParam> position_z() const { return m_position_z; }
    GC::Ref<AudioParam> up_x() const { return m_up_x; }
    GC::Ref<AudioParam> up_y() const { return m_up_y; }
    GC::Ref<AudioParam> up_z() const { return m_up_z; }

    WebIDL::ExceptionOr<void> set_position(float x, float y, float z);
    WebIDL::ExceptionOr<void> set_orientation(float x, float y, float z, float x_up, float y_up, float z_up);

private:
    explicit AudioListener(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<AudioParam> m_forward_x;
    GC::Ref<AudioParam> m_forward_y;
    GC::Ref<AudioParam> m_forward_z;
    GC::Ref<AudioParam> m_position_x;
    GC::Ref<AudioParam> m_position_y;
    GC::Ref<AudioParam> m_position_z;
    GC::Ref<AudioParam> m_up_x;
    GC::Ref<AudioParam> m_up_y;
    GC::Ref<AudioParam> m_up_z;
};

}
