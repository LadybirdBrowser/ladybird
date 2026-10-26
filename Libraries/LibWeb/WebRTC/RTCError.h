/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RTCError.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebRTC {

using RTCErrorInit = Bindings::RTCErrorInit;

class RTCError final : public WebIDL::DOMException {
    WEB_WRAPPABLE(RTCError, WebIDL::DOMException);
    GC_DECLARE_ALLOCATOR(RTCError);

public:
    [[nodiscard]] static GC::Ref<RTCError> create();
    static WebIDL::ExceptionOr<GC::Ref<RTCError>> construct_impl(RTCErrorInit const&, Utf16String const& message);
    virtual ~RTCError() override;

private:
    RTCError();
};

}
