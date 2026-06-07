/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>

namespace Web::Bindings {

// https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects
class WEB_API Serializable {
public:
    virtual ~Serializable() = default;

    // https://html.spec.whatwg.org/multipage/structured-data.html#serialization-steps
    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::StructuredSerializeWriter&, bool for_storage, HTML::SerializationMemory&) = 0;

    // Version of this type's storage body shape; starts at 1, with 0 reserved.
    virtual u64 serialization_version() const { return 1; }

    // https://html.spec.whatwg.org/multipage/structured-data.html#deserialization-steps
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::StructuredSerializeReader&, HTML::DeserializationMemory&) = 0;
};

}
