/*
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedFlyString.h>
#include <AK/Vector.h>
#include <LibJS/Module.h>

namespace JS {

struct ModuleWithSpecifier {
    ByteString specifier;        // [[Specifier]]
    NonnullGCPtr<Module> module; // [[Module]]
};

// https://tc39.es/proposal-import-attributes/#importattribute-record
struct ImportAttribute {
    ByteString key;
    ByteString value;
};

// https://tc39.es/proposal-import-attributes/#modulerequest-record
struct ModuleRequest {
    ModuleRequest() = default;

    explicit ModuleRequest(DeprecatedFlyString specifier)
        : module_specifier(move(specifier))
    {
    }

    ModuleRequest(DeprecatedFlyString specifier, Vector<ImportAttribute> attributes);

    void add_attribute(ByteString key, ByteString value)
    {
        attributes.empend(move(key), move(value));
    }

    DeprecatedFlyString module_specifier; // [[Specifier]]
    Vector<ImportAttribute> attributes;   // [[Attributes]]
};

}
