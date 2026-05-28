/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebIDL {

ArrayBufferViewVariant ArrayBufferView::from_object(GC::Ref<JS::Object> object)
{
    if (is<JS::DataView>(*object))
        return GC::Ref { static_cast<JS::DataView&>(*object) };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<JS::ClassName>(*object))                                                 \
        return GC::Ref { static_cast<JS::ClassName&>(*object) };
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

BufferSourceVariant BufferSource::from_object(GC::Ref<JS::Object> object)
{
    if (is<JS::ArrayBuffer>(*object))
        return GC::Ref { static_cast<JS::ArrayBuffer&>(*object) };

    return ArrayBufferView::from_object(object);
}

BufferSource::BufferSource(BufferSourceVariant const& source)
    : m_buffer_source(source)
{
}

BufferSource::BufferSource(ArrayBufferViewVariant const& source)
    : BufferSource(source.visit([](auto const& view) -> BufferSourceVariant { return view; }))
{
}

BufferSource::BufferSource(ArrayBufferView const& source)
    : BufferSource(source.array_buffer_view())
{
}

u32 BufferSource::byte_length() const
{
    return m_buffer_source.visit(
        [](GC::Ref<JS::DataView> data_view) {
            auto view_record = JS::make_data_view_with_buffer_witness_record(data_view, JS::ArrayBuffer::Order::SeqCst);
            return JS::get_view_byte_length(view_record);
        },
        [](GC::Ref<JS::ArrayBuffer> array_buffer) {
            return static_cast<u32>(array_buffer->byte_length());
        },
        [](auto const& typed_array) {
            auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(*typed_array, JS::ArrayBuffer::Order::SeqCst);
            return JS::typed_array_byte_length(typed_array_record);
        });
}

u32 BufferSource::byte_offset() const
{
    return m_buffer_source.visit(
        [](GC::Ref<JS::ArrayBuffer>) -> u32 { return 0; },
        [](auto const& view) -> u32 { return static_cast<u32>(view->byte_offset()); });
}

u32 BufferSource::element_size() const
{
    return m_buffer_source.visit(
        [](GC::Ref<JS::DataView>) -> u32 {
            return 1;
        },
        [](GC::Ref<JS::ArrayBuffer>) -> u32 {
            return 1;
        },
        [](auto const& typed_array) -> u32 {
            return typed_array->element_size();
        });
}

GC::Ptr<JS::ArrayBuffer> BufferSource::viewed_array_buffer() const
{
    return m_buffer_source.visit(
        [](GC::Ref<JS::ArrayBuffer> array_buffer) -> GC::Ptr<JS::ArrayBuffer> { return array_buffer; },
        [](auto const& view) -> GC::Ptr<JS::ArrayBuffer> { return view->viewed_array_buffer(); });
}

GC::Ptr<JS::TypedArrayBase> BufferSource::typed_array_base() const
{
    return m_buffer_source.visit(
        [](GC::Ref<JS::DataView>) -> GC::Ptr<JS::TypedArrayBase> { return nullptr; },
        [](GC::Ref<JS::ArrayBuffer>) -> GC::Ptr<JS::TypedArrayBase> { return nullptr; },
        [](auto const& typed_array) -> GC::Ptr<JS::TypedArrayBase> { return &static_cast<JS::TypedArrayBase&>(*typed_array); });
}

bool BufferSource::is_data_view() const
{
    return m_buffer_source.has<GC::Ref<JS::DataView>>();
}

bool BufferSource::is_array_buffer() const
{
    return m_buffer_source.has<GC::Ref<JS::ArrayBuffer>>();
}

ArrayBufferView::ArrayBufferView(ArrayBufferViewVariant const& view)
    : m_array_buffer_view(view)
{
}

u32 ArrayBufferView::byte_length() const
{
    return BufferSource { m_array_buffer_view }.byte_length();
}

u32 ArrayBufferView::byte_offset() const
{
    return BufferSource { m_array_buffer_view }.byte_offset();
}

u32 ArrayBufferView::element_size() const
{
    return BufferSource { m_array_buffer_view }.element_size();
}

GC::Ptr<JS::ArrayBuffer> ArrayBufferView::viewed_array_buffer() const
{
    return BufferSource { m_array_buffer_view }.viewed_array_buffer();
}

GC::Ptr<JS::TypedArrayBase> ArrayBufferView::typed_array_base() const
{
    return BufferSource { m_array_buffer_view }.typed_array_base();
}

// https://webidl.spec.whatwg.org/#arraybufferview-write
void ArrayBufferView::write(ReadonlyBytes bytes, u32 starting_offset)
{
    // 1. Let jsView be the result of converting view to a JavaScript value.
    // 2. Assert: bytes’s length ≤ jsView.[[ByteLength]] − startingOffset.
    VERIFY(bytes.size() <= byte_length() - starting_offset);

    // 3. Assert: if view is not a DataView, then bytes’s length modulo the element size of view’s type is 0.
    if (!m_array_buffer_view.has<GC::Ref<JS::DataView>>()) {
        auto element_size = typed_array_base()->element_size();
        VERIFY(bytes.size() % element_size == 0);
    }

    // 4. Let arrayBuffer be the result of converting jsView.[[ViewedArrayBuffer]] to an IDL value of type ArrayBuffer.
    auto array_buffer = viewed_array_buffer();

    // 5. Write bytes into arrayBuffer with startingOffset set to jsView.[[ByteOffset]] + startingOffset.
    array_buffer->overwrite(byte_offset() + starting_offset, bytes.data(), bytes.size());
}

// https://webidl.spec.whatwg.org/#buffersource-detached
bool BufferSource::is_detached(JS::Value const& buffer_source)
{
    // A buffer source type instance bufferSource is detached if the following steps return true:

    // 1. Let jsArrayBuffer be the result of converting bufferSource to a JavaScript value.
    // 2. If jsArrayBuffer has a [[ViewedArrayBuffer]] internal slot, then set jsArrayBuffer to jsArrayBuffer.[[ViewedArrayBuffer]].
    if (!buffer_source.is_object())
        return false;

    JS::ArrayBuffer const* js_array_buffer = nullptr;
    auto const& array_buffer_object = buffer_source.as_object();
    if (auto const* array_buffer = as_if<JS::ArrayBuffer>(array_buffer_object)) {
        js_array_buffer = array_buffer;
    } else if (auto const* typed_array_base = as_if<JS::TypedArrayBase>(array_buffer_object)) {
        js_array_buffer = typed_array_base->viewed_array_buffer();
    } else if (auto const* data_view = as_if<JS::DataView>(array_buffer_object)) {
        js_array_buffer = data_view->viewed_array_buffer();
    } else {
        return false;
    }

    // 3. Return IsDetachedBuffer(jsArrayBuffer).
    return js_array_buffer->is_detached();
}

}
