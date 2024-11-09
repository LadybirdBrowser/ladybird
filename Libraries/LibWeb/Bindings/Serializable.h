/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::Bindings {

// https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects
class Serializable {
public:
    virtual ~Serializable() = default;

    virtual StringView interface_name() const = 0;

    // https://html.spec.whatwg.org/multipage/structured-data.html#serialization-steps
    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::SerializationRecord&, bool for_storage, HTML::SerializationMemory&) = 0;
    // https://html.spec.whatwg.org/multipage/structured-data.html#deserialization-steps
    virtual WebIDL::ExceptionOr<void> deserialization_steps(ReadonlySpan<u32> const&, size_t& position, HTML::DeserializationMemory&) = 0;
};

}
