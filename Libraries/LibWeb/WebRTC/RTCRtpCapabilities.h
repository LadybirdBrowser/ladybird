/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16View.h>
#include <LibWeb/WebRTC/RTCRtpParameters.h>

namespace Web::WebRTC {

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpsender-getcapabilities
Optional<RTCRtpCapabilities> capabilities_for_sending(Utf16View kind);
// https://w3c.github.io/webrtc-pc/#dom-rtcrtpreceiver-getcapabilities
Optional<RTCRtpCapabilities> capabilities_for_receiving(Utf16View kind);

}
