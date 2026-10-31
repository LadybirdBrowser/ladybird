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

namespace Web::HTML {

struct ImageBitmapSharedPixels;

}

namespace Web::Bindings {

// The data holder of a transferred object. What the steps throw is created in the realm of the transfer.
struct RustTransferDataWriter {
    GC::Ref<JS::Realm> realm;
    HTML::TransferDataEncoder& data_holder;
    // AD-HOC: The shared memory that the transfer gathers the pixels of its ImageBitmaps in, if it has some.
    HTML::ImageBitmapSharedPixels* image_bitmap_pixels { nullptr };
};

// The data holder of a received object, in the realm that the object is created in.
struct RustTransferDataReader {
    GC::Ref<JS::Realm> realm;
    HTML::TransferDataDecoder& data_holder;
};

// What a function of the C ABI that the steps call returns to them: the value, or the exception, for which a
// DOMException is created in the realm.
WEB_API JSCompletion completion_to_abi(JS::Realm&, WebIDL::ExceptionOr<JS::Value>);

}
