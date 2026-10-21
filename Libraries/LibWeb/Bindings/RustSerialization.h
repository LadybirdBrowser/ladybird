/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/HostObjectABI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

// What the steps of a serializable interface implemented in Rust read and write, which the Rust bindings
// hold as opaque pointers (Libraries/LibWeb/Bindings/Rust/src/bindings/structured_data.rs). They read and write through
// the C ABI of RustBindingsServices.cpp, and through that of the C++ class of their interface for what only C++
// encodes.

// The record of a structured serialization, and its memory, which the sub-serializations of the steps use. What they
// throw is created in the realm of the serialized object.
struct RustSerializationWriter {
    GC::Ref<JS::Realm> realm;
    HTML::StructuredSerializeWriter& serialized;
    bool for_storage { false };
    HTML::SerializationMemory& memory;
};

// The record of a structured deserialization, and its memory, in the realm that the deserialization creates its values
// in.
struct RustSerializationReader {
    GC::Ref<JS::Realm> realm;
    HTML::StructuredSerializeReader& serialized;
    HTML::DeserializationMemory& memory;
};

// What a function of the C ABI that the steps call returns to them: the value, or the exception, for which a
// DOMException is created in the realm.
WEB_API JSCompletion completion_to_abi(JS::Realm&, WebIDL::ExceptionOr<JS::Value>);

}
