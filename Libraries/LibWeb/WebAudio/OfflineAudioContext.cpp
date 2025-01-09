/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OfflineAudioContext);

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm, OfflineAudioContextOptions const& context_options)
{
    // AD-HOC: This spec text is currently only mentioned in the constructor overload that takes separate arguments,
    //         but these parameters should be validated for both constructors.
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    TRY(verify_audio_options_inside_nominal_range(realm, context_options.number_of_channels, context_options.length, context_options.sample_rate));

    // Let c be a new OfflineAudioContext object. Initialize c as follows:
    auto c = realm.create<OfflineAudioContext>(realm, context_options.length, context_options.sample_rate);

    // 1. Set the [[control thread state]] for c to "suspended".
    c->set_control_state(Bindings::AudioContextState::Suspended);

    // 2. Set the [[rendering thread state]] for c to "suspended".
    c->set_rendering_state(Bindings::AudioContextState::Suspended);

    // FIXME: 3. Determine the [[render quantum size]] for this OfflineAudioContext, based on the value of the renderSizeHint:

    // 4. Construct an AudioDestinationNode with its channelCount set to contextOptions.numberOfChannels.
    c->m_destination = TRY(AudioDestinationNode::construct_impl(realm, c, context_options.number_of_channels));

    // FIXME: 5. Let messageChannel be a new MessageChannel.
    // FIXME: 6. Let controlSidePort be the value of messageChannel’s port1 attribute.
    // FIXME: 7. Let renderingSidePort be the value of messageChannel’s port2 attribute.
    // FIXME: 8. Let serializedRenderingSidePort be the result of StructuredSerializeWithTransfer(renderingSidePort, « renderingSidePort »).
    // FIXME: 9. Set this audioWorklet's port to controlSidePort.
    // FIXME: 10. Queue a control message to set the MessagePort on the AudioContextGlobalScope, with serializedRenderingSidePort.

    return c;
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext-numberofchannels-length-samplerate
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm,
    WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    return construct_impl(realm, { number_of_channels, length, sample_rate });
}

OfflineAudioContext::~OfflineAudioContext() = default;

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::start_rendering()
{
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement OfflineAudioContext::start_rendering"_string);
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::resume()
{
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement OfflineAudioContext::resume"_string);
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::suspend(double suspend_time)
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
GC::Ptr<WebIDL::CallbackType> OfflineAudioContext::oncomplete()
{
    return event_handler_attribute(HTML::EventNames::complete);
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-oncomplete
void OfflineAudioContext::set_oncomplete(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::complete, value);
}

OfflineAudioContext::OfflineAudioContext(JS::Realm& realm, WebIDL::UnsignedLong length, float sample_rate)
    : BaseAudioContext(realm, sample_rate)
    , m_length(length)
{
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
