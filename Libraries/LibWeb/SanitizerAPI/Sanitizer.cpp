/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SanitizerAPI/Sanitizer.h>

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SanitizerAPI {

GC_DEFINE_ALLOCATOR(Sanitizer);

// https://wicg.github.io/sanitizer-api/#dom-sanitizer-constructor
WebIDL::ExceptionOr<GC::Ref<Sanitizer>> Sanitizer::construct_impl(JS::Realm& realm, Bindings::SanitizerPresets configuration)
{
    // 1. TODO If configuration is a SanitizerPresets string, then:
    if (true) {
        // 1. Assert: configuration is default.
        // 2. TODO Set configuration to the built-in safe default configuration.
    }
    auto result = realm.create<Sanitizer>(realm);

    // 2. Let valid be the return value of set a configuration with configuration and true on this.
    auto const valid = result->set_a_configuration(configuration, AllowCommentsAndDataAttributes::Yes);

    // 3. If valid is false, then throw a TypeError.
    if (!valid)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Sanitizer configuration is not valid"_string };

    return result;
}

Sanitizer::Sanitizer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void Sanitizer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Sanitizer);
    Base::initialize(realm);
}

// https://wicg.github.io/sanitizer-api/#sanitizer-set-a-configuration
bool Sanitizer::set_a_configuration(Bindings::SanitizerPresets, AllowCommentsAndDataAttributes)
{
    // 1. TODO Canonicalize configuration with allowCommentsAndDataAttributes.
    // 2. TODO If configuration is not valid, then return false.
    // 3. TODO Set sanitizer’s configuration to configuration.
    // 4. Return true.
    return true;
}

}
