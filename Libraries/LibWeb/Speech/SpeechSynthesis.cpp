/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Speech/SpeechSynthesis.h>

namespace Web::Speech {

extern "C" {
SpeechSynthesis* web_rust_speech_synthesis_create(JSVM*);
}

GC::Ref<SpeechSynthesis> SpeechSynthesis::create()
{
    return *web_rust_speech_synthesis_create(JS::HostABI::vm_to_abi(JS::VM::the()));
}

}
