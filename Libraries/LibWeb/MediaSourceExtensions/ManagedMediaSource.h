/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MediaSourceExtensions/MediaSource.h>

namespace Web::MediaSourceExtensions {

// https://w3c.github.io/media-source/#managedmediasource-interface
class ManagedMediaSource : public MediaSource {
    WEB_WRAPPABLE(ManagedMediaSource, MediaSource);
    GC_DECLARE_ALLOCATOR(ManagedMediaSource);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ManagedMediaSource>> construct_impl(GC::Ref<DOM::EventTarget> relevant_global_object);

    void set_onstartstreaming(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onstartstreaming();

    void set_onendstreaming(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onendstreaming();

private:
    ManagedMediaSource(GC::Ref<DOM::EventTarget> relevant_global_object);

    virtual ~ManagedMediaSource() override;
};

}
