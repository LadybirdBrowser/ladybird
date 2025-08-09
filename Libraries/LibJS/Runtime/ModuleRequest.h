/*
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Module.h>

namespace JS {

struct ModuleWithSpecifier {
    Utf16String specifier;  // [[Specifier]]
    GC::Ref<Module> module; // [[Module]]
};

// https://tc39.es/ecma262/#importattribute-record
struct ImportAttribute {
    Utf16String key;
    Utf16String value;

    bool operator==(ImportAttribute const&) const = default;
};

// https://tc39.es/ecma262/#modulerequest-record
struct ModuleRequest {
    ModuleRequest() = default;

    explicit ModuleRequest(Utf16FlyString specifier)
        : module_specifier(move(specifier))
    {
    }

    ModuleRequest(Utf16FlyString specifier, Vector<ImportAttribute> attributes);

    void add_attribute(Utf16String key, Utf16String value)
    {
        attributes.empend(move(key), move(value));
    }

    Utf16FlyString module_specifier;    // [[Specifier]]
    Vector<ImportAttribute> attributes; // [[Attributes]]

    bool operator==(ModuleRequest const&) const = default;
};

}
