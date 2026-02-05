/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SpeechGrammarPrototype.h>
#include <LibWeb/Speech/SpeechGrammar.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechGrammar);

WebIDL::ExceptionOr<GC::Ref<SpeechGrammar>> SpeechGrammar::construct_impl(JS::Realm& realm)
{
    return realm.create<SpeechGrammar>(realm);
}

SpeechGrammar::SpeechGrammar(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

SpeechGrammar::~SpeechGrammar() = default;

void SpeechGrammar::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SpeechGrammar);
    Base::initialize(realm);
}

}
