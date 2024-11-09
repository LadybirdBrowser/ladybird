/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextEncoderPrototype.h>
#include <LibWeb/Encoding/TextEncoder.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Encoding {

JS_DEFINE_ALLOCATOR(TextEncoder);

WebIDL::ExceptionOr<JS::NonnullGCPtr<TextEncoder>> TextEncoder::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<TextEncoder>(realm, realm);
}

TextEncoder::TextEncoder(JS::Realm& realm)
    : PlatformObject(realm)
{
}

TextEncoder::~TextEncoder() = default;

void TextEncoder::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextEncoder);
}

// https://encoding.spec.whatwg.org/#dom-textencoder-encode
JS::NonnullGCPtr<JS::Uint8Array> TextEncoder::encode(String const& input) const
{
    // NOTE: The AK::String is always UTF-8, so most of these steps are no-ops.
    // 1. Convert input to an I/O queue of scalar values.
    // 2. Let output be the I/O queue of bytes « end-of-queue ».
    // 3. While true:
    //     1. Let item be the result of reading from input.
    //     2. Let result be the result of processing an item with item, an instance of the UTF-8 encoder, input, output, and "fatal".
    //     3. Assert: result is not an error.
    //     4. If result is finished, then convert output into a byte sequence and return a Uint8Array object wrapping an ArrayBuffer containing output.

    auto byte_buffer = MUST(ByteBuffer::copy(input.bytes()));
    auto array_length = byte_buffer.size();
    auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
    return JS::Uint8Array::create(realm(), array_length, *array_buffer);
}

// https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
TextEncoderEncodeIntoResult TextEncoder::encode_into(String const& source, JS::Handle<WebIDL::BufferSource> const& destination) const
{
    auto& data = destination->viewed_array_buffer()->buffer();

    // 1. Let read be 0.
    WebIDL::UnsignedLongLong read = 0;
    // 2. Let written be 0.
    WebIDL::UnsignedLongLong written = 0;

    // NOTE: The AK::String is always UTF-8, so most of these steps are no-ops.
    // 3. Let encoder be an instance of the UTF-8 encoder.
    // 4. Let unused be the I/O queue of scalar values « end-of-queue ».
    // 5. Convert source to an I/O queue of scalar values.
    auto code_points = source.code_points();
    auto it = code_points.begin();

    // 6. While true:
    while (true) {
        // 6.1. Let item be the result of reading from source.
        // 6.2. Let result be the result of running encoder’s handler on unused and item.
        // 6.3. If result is finished, then break.
        if (it.done())
            break;
        auto item = *it;
        auto result = it.underlying_code_point_bytes();

        // 6.4. Otherwise:
        // 6.4.1. If destination’s byte length − written is greater than or equal to the number of bytes in result, then:
        if (data.size() - written >= result.size()) {
            // 6.4.1.1. If item is greater than U+FFFF, then increment read by 2.
            if (item > 0xffff) {
                read += 2;
            }
            // 6.4.1.2. Otherwise, increment read by 1.
            else {
                read++;
            }

            // 6.4.1.3. Write the bytes in result into destination, with startingOffset set to written.
            // 6.4.1.4. Increment written by the number of bytes in result.
            for (auto byte : result)
                data[written++] = byte;
        }
        // 6.4.2. Otherwise, break.
        else {
            break;
        }

        ++it;
    }

    // 7. Return «[ "read" → read, "written" → written ]».
    return { read, written };
}

// https://encoding.spec.whatwg.org/#dom-textencoder-encoding
FlyString const& TextEncoder::encoding()
{
    static FlyString const encoding = "utf-8"_fly_string;
    return encoding;
}

}
