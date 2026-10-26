/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>

namespace Web::WebRTC {

class RTCCertificate final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCCertificate, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCCertificate);

public:
    [[nodiscard]] static GC::Ref<RTCCertificate> create();
    virtual ~RTCCertificate() override;

private:
    RTCCertificate();
};

}
