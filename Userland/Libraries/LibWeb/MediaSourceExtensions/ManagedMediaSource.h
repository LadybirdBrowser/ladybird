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
    WEB_PLATFORM_OBJECT(ManagedMediaSource, MediaSource);
    JS_DECLARE_ALLOCATOR(ManagedMediaSource);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<JS::NonnullGCPtr<ManagedMediaSource>> construct_impl(JS::Realm&);

private:
    ManagedMediaSource(JS::Realm&);

    virtual ~ManagedMediaSource() override;

    virtual void initialize(JS::Realm&) override;
};

}
