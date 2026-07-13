/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/UnicodeUtils.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextEncoder.h>
#include <LibWeb/Encoding/TextEncoder.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextEncoder);

GC::Ref<TextEncoder> TextEncoder::construct_impl(JS::Realm& realm)
{
    return realm.create<TextEncoder>(realm);
}

TextEncoder::TextEncoder(JS::Realm& realm)
    : PlatformObject(realm)
{
}

TextEncoder::~TextEncoder() = default;

void TextEncoder::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextEncoder);
    Base::initialize(realm);
}

// https://encoding.spec.whatwg.org/#dom-textencoder-encode
GC::Ref<JS::Uint8Array> TextEncoder::encode(Utf16String const& input) const
{
    // 1. Convert input to an I/O queue of scalar values.
    // 2. Let output be the I/O queue of bytes « end-of-queue ».
    // 3. While true:
    //     1. Let item be the result of reading from input.
    //     2. Let result be the result of processing an item with item, an instance of the UTF-8 encoder, input, output, and "fatal".
    //     3. Assert: result is not an error.
    //     4. If result is finished, then convert output into a byte sequence and return a Uint8Array object wrapping an ArrayBuffer containing output.

    auto utf8_input = input.to_utf8(AllowLonelySurrogates::No);
    auto byte_buffer = MUST(ByteBuffer::copy(utf8_input.bytes()));
    auto array_length = byte_buffer.size();
    auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
    return JS::Uint8Array::create(realm(), array_length, *array_buffer);
}

// https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
Bindings::TextEncoderEncodeIntoResult TextEncoder::encode_into(Utf16String const& source, GC::Ref<JS::Uint8Array> destination) const
{
    // AD-HOC: Return early if destination is detached. This is not explicitly handled in the spec,
    //         however no bytes are copied as destinations size is always zero in this case.
    //         See: https://github.com/whatwg/encoding/issues/324
    if (destination->viewed_array_buffer()->is_detached())
        return { 0, 0 };

    auto destination_record = JS::make_typed_array_with_buffer_witness_record(*destination, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(destination_record))
        return { 0, 0 };
    auto destination_byte_length = JS::typed_array_byte_length(destination_record);

    // 1. Let read be 0.
    WebIDL::UnsignedLongLong read = 0;
    // 2. Let written be 0.
    WebIDL::UnsignedLongLong written = 0;

    // 3. Let encoder be an instance of the UTF-8 encoder.
    // 4. Let unused be the I/O queue of scalar values « end-of-queue ».
    // 5. Convert source to an I/O queue of scalar values.
    auto it = source.utf16_view().begin();
    auto end = source.utf16_view().end();

    // 6. While true:
    while (true) {
        // 6.1. Let item be the result of reading from source.
        // 6.2. Let result be the result of running encoder’s handler on unused and item.
        // 6.3. If result is finished, then break.
        if (it == end)
            break;
        auto item = *it;
        auto code_unit_length = it.length_in_code_units();

        Array<u8, 4> result;
        size_t result_size = 0;
        (void)AK::UnicodeUtils::code_point_to_utf8(item, [&](char byte) {
            result[result_size++] = static_cast<u8>(byte);
        });

        // 6.4. Otherwise:
        // 6.4.1. If destination’s byte length − written is greater than or equal to the number of bytes in result, then:
        if (destination_byte_length - written >= result_size) {
            // 6.4.1.1. If item is greater than U+FFFF, then increment read by 2.
            // 6.4.1.2. Otherwise, increment read by 1.
            read += code_unit_length;

            // 6.4.1.3. Write the bytes in result into destination, with startingOffset set to written.
            // 6.4.1.4. Increment written by the number of bytes in result.
            // WARNING: See the warning for SharedArrayBuffer objects at https://encoding.spec.whatwg.org/#sharedarraybuffer-warning.
            destination->viewed_array_buffer()->overwrite(destination->byte_offset() + written, result.data(), result_size);
            written += result_size;
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

}
