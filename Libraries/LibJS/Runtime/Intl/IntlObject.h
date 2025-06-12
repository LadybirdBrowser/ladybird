/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS::Intl {

// https://tc39.es/ecma402/#resolution-option-descriptor
struct ResolutionOptionDescriptor {
    StringView key;
    PropertyKey property;
    OptionType type { OptionType::String };
    ReadonlySpan<StringView> values {};
};

class IntlObject : public Object {
    JS_OBJECT(IntlObject, Object);

public:
    virtual ReadonlySpan<StringView> relevant_extension_keys() const = 0;
    virtual ReadonlySpan<ResolutionOptionDescriptor> resolution_option_descriptors(VM&) const = 0;

protected:
    using Object::Object;
};

}
