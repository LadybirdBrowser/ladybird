/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/OfflineAudioContextPrototype.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#OfflineAudioContextOptions
struct OfflineAudioContextOptions {
    WebIDL::UnsignedLong number_of_channels { 1 };
    WebIDL::UnsignedLong length {};
    float sample_rate {};
};

// https://webaudio.github.io/web-audio-api/#OfflineAudioContext
class OfflineAudioContext final : public BaseAudioContext {
    WEB_PLATFORM_OBJECT(OfflineAudioContext, BaseAudioContext);
    GC_DECLARE_ALLOCATOR(OfflineAudioContext);

public:
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> construct_impl(JS::Realm&, OfflineAudioContextOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> construct_impl(JS::Realm&,
        WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);

    virtual ~OfflineAudioContext() override;

    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> start_rendering();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> resume();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> suspend(double suspend_time);

    WebIDL::UnsignedLong length() const;

    GC::Ptr<WebIDL::CallbackType> oncomplete();
    void set_oncomplete(GC::Ptr<WebIDL::CallbackType>);

private:
    OfflineAudioContext(JS::Realm&, OfflineAudioContextOptions const&);
    OfflineAudioContext(JS::Realm&, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::UnsignedLong m_length {};
};

}
