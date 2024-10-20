/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(OfflineAudioContext);

WebIDL::ExceptionOr<JS::NonnullGCPtr<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm, OfflineAudioContextOptions const& context_options)
{
    return construct_impl(realm, context_options.number_of_channels, context_options.length, context_options.sample_rate);
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext-numberofchannels-length-samplerate
WebIDL::ExceptionOr<JS::NonnullGCPtr<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm,
    WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // The OfflineAudioContext can be constructed with the same arguments as AudioContext.createBuffer.
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    TRY(verify_audio_options_inside_nominal_range(realm, number_of_channels, length, sample_rate));

    return realm.heap().allocate<OfflineAudioContext>(realm, realm, number_of_channels, length, sample_rate);
}

OfflineAudioContext::~OfflineAudioContext() = default;

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> OfflineAudioContext::start_rendering()
{
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement OfflineAudioContext::start_rendering"_string);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> OfflineAudioContext::resume()
{
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement OfflineAudioContext::resume"_string);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> OfflineAudioContext::suspend(double suspend_time)
{
    (void)suspend_time;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement OfflineAudioContext::suspend"_string);
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-length
WebIDL::UnsignedLong OfflineAudioContext::length() const
{
    // The size of the buffer in sample-frames. This is the same as the value of the length parameter for the constructor.
    return m_length;
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-oncomplete
JS::GCPtr<WebIDL::CallbackType> OfflineAudioContext::oncomplete()
{
    return event_handler_attribute(HTML::EventNames::complete);
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-oncomplete
void OfflineAudioContext::set_oncomplete(JS::GCPtr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::complete, value);
}

OfflineAudioContext::OfflineAudioContext(JS::Realm& realm, OfflineAudioContextOptions const&)
    : BaseAudioContext(realm)
{
}

OfflineAudioContext::OfflineAudioContext(JS::Realm& realm, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
    : BaseAudioContext(realm, sample_rate)
    , m_length(length)
{
    (void)number_of_channels;
}

void OfflineAudioContext::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OfflineAudioContext);
}

void OfflineAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
