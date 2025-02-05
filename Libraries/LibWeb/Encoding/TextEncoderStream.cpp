/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/UnicodeUtils.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextEncoderStreamPrototype.h>
#include <LibWeb/Encoding/TextEncoderStream.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Encoding {

GC_DEFINE_ALLOCATOR(TextEncoderStream);

// https://encoding.spec.whatwg.org/#dom-textencoderstream
WebIDL::ExceptionOr<GC::Ref<TextEncoderStream>> TextEncoderStream::construct_impl(JS::Realm& realm)
{
    // 1. Set this’s encoder to an instance of the UTF-8 encoder.
    // NOTE: No-op, as AK::String is already in UTF-8 format.

    // NOTE: We do these steps first so that we may store it as nonnull in the GenericTransformStream.
    // 4. Let transformStream be a new TransformStream.
    auto transform_stream = realm.create<Streams::TransformStream>(realm);

    // 6. Set this's transform to a new TransformStream.
    auto stream = realm.create<TextEncoderStream>(realm, transform_stream);

    // 2. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the encode and enqueue a chunk
    //    algorithm with this and chunk.
    auto transform_algorithm = GC::create_function(realm.heap(), [stream](JS::Value chunk) -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->encode_and_enqueue_chunk(chunk); result.is_error()) {
            auto throw_completion = Bindings::exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, *throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 3. Let flushAlgorithm be an algorithm which runs the encode and flush algorithm with this.
    auto flush_algorithm = GC::create_function(realm.heap(), [stream]() -> GC::Ref<WebIDL::Promise> {
        auto& realm = stream->realm();
        auto& vm = realm.vm();

        if (auto result = stream->encode_and_flush(); result.is_error()) {
            auto throw_completion = Bindings::exception_to_throw_completion(vm, result.exception());
            return WebIDL::create_rejected_promise(realm, *throw_completion.release_value());
        }

        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 5. Set up transformStream with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to flushAlgorithm.
    transform_stream->set_up(transform_algorithm, flush_algorithm);

    return stream;
}

TextEncoderStream::TextEncoderStream(JS::Realm& realm, GC::Ref<Streams::TransformStream> transform)
    : Bindings::PlatformObject(realm)
    , Streams::GenericTransformStreamMixin(transform)
{
}

TextEncoderStream::~TextEncoderStream() = default;

void TextEncoderStream::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextEncoderStream);
}

void TextEncoderStream::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Streams::GenericTransformStreamMixin::visit_edges(visitor);
}

// https://encoding.spec.whatwg.org/#encode-and-enqueue-a-chunk
WebIDL::ExceptionOr<void> TextEncoderStream::encode_and_enqueue_chunk(JS::Value chunk)
{
    // Spec Note: This is equivalent to the "convert a string into a scalar value string" algorithm from the Infra
    //            Standard, but allows for surrogate pairs that are split between strings. [INFRA]

    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. Let input be the result of converting chunk to a DOMString.
    auto input = TRY(chunk.to_string(vm));

    // 2. Convert input to an I/O queue of code units.
    // Spec Note: DOMString, as well as an I/O queue of code units rather than scalar values, are used here so that a
    //            surrogate pair that is split between chunks can be reassembled into the appropriate scalar value.
    //            The behavior is otherwise identical to USVString. In particular, lone surrogates will be replaced
    //            with U+FFFD.
    auto code_points = input.code_points();
    auto it = code_points.begin();

    // 3. Let output be the I/O queue of bytes « end-of-queue ».
    ByteBuffer output;

    // 4. While true:
    while (true) {
        // 2. If item is end-of-queue, then:
        // NOTE: This is done out-of-order so that we're not dereferencing a code point iterator that points to the end.
        if (it.done()) {
            // 1. Convert output into a byte sequence.
            // Note: No-op.

            // 2. If output is non-empty, then:
            if (!output.is_empty()) {
                // 1. Let chunk be a Uint8Array object wrapping an ArrayBuffer containing output.
                auto array_buffer = JS::ArrayBuffer::create(realm, move(output));
                auto array = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

                // 2. Enqueue chunk into encoder’s transform.
                TRY(Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), array));
            }

            // 3. Return.
            return {};
        }

        // 1. Let item be the result of reading from input.
        auto item = *it;

        // 3. Let result be the result of executing the convert code unit to scalar value algorithm with encoder, item and input.
        auto result = convert_code_unit_to_scalar_value(item, it);

        // 4. If result is not continue, then process an item with result, encoder’s encoder, input, output, and "fatal".
        if (result.has_value()) {
            (void)AK::UnicodeUtils::code_point_to_utf8(result.value(), [&output](char utf8_byte) {
                output.append(static_cast<u8>(utf8_byte));
            });
        }
    }
}

// https://encoding.spec.whatwg.org/#encode-and-flush
WebIDL::ExceptionOr<void> TextEncoderStream::encode_and_flush()
{
    auto& realm = this->realm();

    // 1. If encoder’s leading surrogate is non-null, then:
    if (m_leading_surrogate.has_value()) {
        // 1. Let chunk be a Uint8Array object wrapping an ArrayBuffer containing 0xEF 0xBF 0xBD.
        // Spec Note: This is U+FFFD (�) in UTF-8 bytes.
        constexpr static u8 replacement_character_utf8_bytes[3] = { 0xEF, 0xBF, 0xBD };
        auto bytes = MUST(ByteBuffer::copy(replacement_character_utf8_bytes, sizeof(replacement_character_utf8_bytes)));
        auto array_buffer = JS::ArrayBuffer::create(realm, bytes);
        auto chunk = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

        // 2. Enqueue chunk into encoder’s transform.
        TRY(Streams::transform_stream_default_controller_enqueue(*m_transform->controller(), chunk));
    }

    return {};
}

// https://encoding.spec.whatwg.org/#convert-code-unit-to-scalar-value
Optional<u32> TextEncoderStream::convert_code_unit_to_scalar_value(u32 item, Utf8CodePointIterator& code_point_iterator)
{
    ArmedScopeGuard move_to_next_code_point_guard = [&] {
        ++code_point_iterator;
    };

    // 1. If encoder’s leading surrogate is non-null, then:
    if (m_leading_surrogate.has_value()) {
        // 1. Let leadingSurrogate be encoder’s leading surrogate.
        auto leading_surrogate = m_leading_surrogate.value();

        // 2. Set encoder’s leading surrogate to null.
        m_leading_surrogate.clear();

        // 3. If item is a trailing surrogate, then return a scalar value from surrogates given leadingSurrogate
        //    and item.
        if (Utf16View::is_low_surrogate(item)) {
            // https://encoding.spec.whatwg.org/#scalar-value-from-surrogates
            // To obtain a scalar value from surrogates, given a leading surrogate leading and a trailing surrogate
            // trailing, return 0x10000 + ((leading − 0xD800) << 10) + (trailing − 0xDC00).
            return Utf16View::decode_surrogate_pair(leading_surrogate, item);
        }

        // 4. Restore item to input.
        move_to_next_code_point_guard.disarm();

        // 5. Return U+FFFD.
        return 0xFFFD;
    }

    // 2. If item is a leading surrogate, then set encoder’s leading surrogate to item and return continue.
    if (Utf16View::is_high_surrogate(item)) {
        m_leading_surrogate = item;
        return OptionalNone {};
    }

    // 3. If item is a trailing surrogate, then return U+FFFD.
    if (Utf16View::is_low_surrogate(item))
        return 0xFFFD;

    // 4. Return item.
    return item;
}

}
