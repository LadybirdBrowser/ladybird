/*
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Module.h>

namespace JS {

struct ModuleWithSpecifier {
    String specifier;       // [[Specifier]]
    GC::Ref<Module> module; // [[Module]]
};

// https://tc39.es/proposal-import-attributes/#importattribute-record
struct ImportAttribute {
    String key;
    String value;

    bool operator==(ImportAttribute const&) const = default;
};

// https://tc39.es/proposal-import-attributes/#modulerequest-record
struct ModuleRequest {
    ModuleRequest() = default;

    explicit ModuleRequest(FlyString specifier)
        : module_specifier(move(specifier))
    {
    }

    ModuleRequest(FlyString specifier, Vector<ImportAttribute> attributes);

    void add_attribute(String key, String value)
    {
        attributes.empend(move(key), move(value));
    }

    FlyString module_specifier;         // [[Specifier]]
    Vector<ImportAttribute> attributes; // [[Attributes]]

    bool operator==(ModuleRequest const&) const = default;
};

}
