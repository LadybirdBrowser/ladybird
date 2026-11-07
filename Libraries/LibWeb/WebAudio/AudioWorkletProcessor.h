/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioWorkletProcessor.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio {

class WEB_API AudioWorkletProcessor final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AudioWorkletProcessor, audio_worklet_processor)
};

}
