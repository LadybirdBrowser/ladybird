/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioWorkletNode.h>
#include <LibWeb/Bindings/RustInterface.h>
namespace Web::WebAudio {

class WEB_API AudioWorkletNode final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AudioWorkletNode, audio_worklet_node);
};

}
