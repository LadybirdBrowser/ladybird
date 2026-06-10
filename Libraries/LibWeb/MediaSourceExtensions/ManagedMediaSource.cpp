/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/ManagedMediaSource.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(ManagedMediaSource);

GC::Ref<ManagedMediaSource> ManagedMediaSource::create(GC::Ref<DOM::EventTarget> relevant_global_object)
{
    return GC::Heap::the().allocate<ManagedMediaSource>(relevant_global_object);
}

GC::Ref<ManagedMediaSource> ManagedMediaSource::create_for_constructor(JS::Realm& realm)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(realm.global_object());
    VERIFY(global_scope);
    return create(global_scope->this_impl());
}

ManagedMediaSource::ManagedMediaSource(GC::Ref<DOM::EventTarget> relevant_global_object)
    : MediaSource(relevant_global_object)
{
}

ManagedMediaSource::~ManagedMediaSource() = default;

// https://w3c.github.io/media-source/#dom-managedmediasource-onstartstreaming
void ManagedMediaSource::set_onstartstreaming(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::startstreaming, event_handler);
}

// https://w3c.github.io/media-source/#dom-managedmediasource-onstartstreaming
GC::Ptr<WebIDL::CallbackType> ManagedMediaSource::onstartstreaming()
{
    return event_handler_attribute(EventNames::startstreaming);
}

// https://w3c.github.io/media-source/#dom-managedmediasource-onendstreaming
void ManagedMediaSource::set_onendstreaming(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::endstreaming, event_handler);
}

// https://w3c.github.io/media-source/#dom-managedmediasource-onendstreaming
GC::Ptr<WebIDL::CallbackType> ManagedMediaSource::onendstreaming()
{
    return event_handler_attribute(EventNames::endstreaming);
}

}
