/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(BufferableObjectBase);
GC_DEFINE_ALLOCATOR(ArrayBufferView);
GC_DEFINE_ALLOCATOR(BufferSource);

u32 BufferableObjectBase::byte_length() const
{
    return m_bufferable_object.visit(
        [](GC::Ref<JS::TypedArrayBase> typed_array) {
            auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(typed_array, JS::ArrayBuffer::Order::SeqCst);
            return JS::typed_array_byte_length(typed_array_record);
        },
        [](GC::Ref<JS::DataView> data_view) {
            auto view_record = JS::make_data_view_with_buffer_witness_record(data_view, JS::ArrayBuffer::Order::SeqCst);
            return JS::get_view_byte_length(view_record);
        },
        [](GC::Ref<JS::ArrayBuffer> array_buffer) { return static_cast<u32>(array_buffer->byte_length()); });
}

u32 BufferableObjectBase::byte_offset() const
{
    return m_bufferable_object.visit(
        [](GC::Ref<JS::ArrayBuffer>) -> u32 { return 0; },
        [](auto& view) -> u32 { return static_cast<u32>(view->byte_offset()); });
}

u32 BufferableObjectBase::element_size() const
{
    return m_bufferable_object.visit(
        [](GC::Ref<JS::TypedArrayBase> typed_array) -> u32 {
            auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(typed_array, JS::ArrayBuffer::Order::SeqCst);
            return typed_array_record.object->element_size();
        },
        [](GC::Ref<JS::DataView>) -> u32 {
            return 1;
        },
        [](GC::Ref<JS::ArrayBuffer>) -> u32 {
            return 1;
        });
}

GC::Ref<JS::Object> BufferableObjectBase::raw_object()
{
    return m_bufferable_object.visit([](auto const& obj) -> GC::Ref<JS::Object> { return obj; });
}

GC::Ptr<JS::ArrayBuffer> BufferableObjectBase::viewed_array_buffer()
{
    return m_bufferable_object.visit(
        [](GC::Ref<JS::ArrayBuffer> array_buffer) -> GC::Ptr<JS::ArrayBuffer> { return array_buffer; },
        [](auto const& view) -> GC::Ptr<JS::ArrayBuffer> { return view->viewed_array_buffer(); });
}

BufferableObject BufferableObjectBase::bufferable_object_from_raw_object(GC::Ref<JS::Object> object)
{
    if (is<JS::TypedArrayBase>(*object))
        return GC::Ref { static_cast<JS::TypedArrayBase&>(*object) };

    if (is<JS::DataView>(*object))
        return GC::Ref { static_cast<JS::DataView&>(*object) };

    if (is<JS::ArrayBuffer>(*object))
        return GC::Ref { static_cast<JS::ArrayBuffer&>(*object) };

    VERIFY_NOT_REACHED();
}

BufferableObjectBase::BufferableObjectBase(GC::Ref<JS::Object> object)
    : m_bufferable_object(bufferable_object_from_raw_object(object))
{
}

bool BufferableObjectBase::is_typed_array_base() const
{
    return m_bufferable_object.has<GC::Ref<JS::TypedArrayBase>>();
}

bool BufferableObjectBase::is_data_view() const
{
    return m_bufferable_object.has<GC::Ref<JS::DataView>>();
}

bool BufferableObjectBase::is_array_buffer() const
{
    return m_bufferable_object.has<GC::Ref<JS::ArrayBuffer>>();
}

void BufferableObjectBase::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_bufferable_object.visit([&](auto& obj) { visitor.visit(obj); });
}

ArrayBufferView::~ArrayBufferView() = default;

// https://webidl.spec.whatwg.org/#arraybufferview-write
void ArrayBufferView::write(ReadonlyBytes bytes, u32 starting_offset)
{
    // 1. Let jsView be the result of converting view to a JavaScript value.
    // 2. Assert: bytes’s length ≤ jsView.[[ByteLength]] − startingOffset.
    VERIFY(bytes.size() <= byte_length() - starting_offset);

    // 3. Assert: if view is not a DataView, then bytes’s length modulo the element size of view’s type is 0.
    if (!m_bufferable_object.has<GC::Ref<JS::DataView>>()) {
        auto element_size = m_bufferable_object.get<GC::Ref<JS::TypedArrayBase>>()->element_size();
        VERIFY(bytes.size() % element_size == 0);
    }

    // 4. Let arrayBuffer be the result of converting jsView.[[ViewedArrayBuffer]] to an IDL value of type ArrayBuffer.
    auto array_buffer = viewed_array_buffer();

    // 5. Write bytes into arrayBuffer with startingOffset set to jsView.[[ByteOffset]] + startingOffset.
    array_buffer->buffer().overwrite(byte_offset() + starting_offset, bytes.data(), bytes.size());
}

BufferSource::~BufferSource() = default;

// https://webidl.spec.whatwg.org/#buffersource-detached
bool is_buffer_source_detached(JS::Value const& buffer_source)
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
