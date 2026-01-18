/*
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

struct StartSource {
    NodeID node_id { 0 };
    double when { 0.0 };
};

struct StopSource {
    NodeID node_id { 0 };
    double when { 0.0 };
};

// FIXME: add more message types

// https://webaudio.github.io/web-audio-api/#control-message
using ControlMessage = Variant<StartSource, StopSource>;

}
