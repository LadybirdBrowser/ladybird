/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/SpeechSynthesis.h>

namespace Web::Speech {

class SpeechSynthesis : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(SpeechSynthesis, speech_synthesis)

public:
    [[nodiscard]] static GC::Ref<SpeechSynthesis> create();
};

}
