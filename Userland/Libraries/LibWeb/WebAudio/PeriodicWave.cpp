/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PeriodicWavePrototype.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(PeriodicWave);

// https://webaudio.github.io/web-audio-api/#dom-periodicwave-periodicwave
WebIDL::ExceptionOr<JS::NonnullGCPtr<PeriodicWave>> PeriodicWave::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext>, PeriodicWaveOptions const&)
{
    return WebIDL::NotSupportedError::create(realm, "FIXME: Implement PeriodicWave::construct_impl"_string);
}

PeriodicWave::~PeriodicWave() = default;

void PeriodicWave::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PeriodicWave);
}

void PeriodicWave::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
