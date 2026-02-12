/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/MediaCapture/MediaStreamConstraints.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

// Spec: https://w3c.github.io/mediacapture-main/#mediadevices
class MediaDevices final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaDevices, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaDevices);

public:
    [[nodiscard]] static GC::Ref<MediaDevices> create(JS::Realm&);
    virtual ~MediaDevices() override;

    GC::Ref<WebIDL::Promise> enumerate_devices();
    MediaTrackSupportedConstraints get_supported_constraints();
    GC::Ref<WebIDL::Promise> get_user_media(Optional<MediaStreamConstraints> const& constraints = {});

    void set_ondevicechange(WebIDL::CallbackType* event_handler);
    WebIDL::CallbackType* ondevicechange();

private:
    explicit MediaDevices(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
