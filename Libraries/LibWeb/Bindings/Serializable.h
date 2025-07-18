/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/IntrinsicDefinitions.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>

namespace Web::Bindings {

// https://html.spec.whatwg.org/multipage/structured-data.html#serializable-objects
class Serializable {
public:
    virtual ~Serializable() = default;

    // https://html.spec.whatwg.org/multipage/structured-data.html#serialization-steps
    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) = 0;

    // https://html.spec.whatwg.org/multipage/structured-data.html#deserialization-steps
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&) = 0;
};

}
