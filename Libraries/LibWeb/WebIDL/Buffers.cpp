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

JS_DEFINE_ALLOCATOR(BufferableObjectBase);
JS_DEFINE_ALLOCATOR(ArrayBufferView);
JS_DEFINE_ALLOCATOR(BufferSource);

u32 BufferableObjectBase::byte_length() const
{
    return m_bufferable_object.visit(
        [](JS::NonnullGCPtr<JS::TypedArrayBase> typed_array) {
            auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(typed_array, JS::ArrayBuffer::Order::SeqCst);
            return JS::typed_array_byte_length(typed_array_record);
        },
        [](JS::NonnullGCPtr<JS::DataView> data_view) {
            auto view_record = JS::make_data_view_with_buffer_witness_record(data_view, JS::ArrayBuffer::Order::SeqCst);
            return JS::get_view_byte_length(view_record);
        },
        [](JS::NonnullGCPtr<JS::ArrayBuffer> array_buffer) { return static_cast<u32>(array_buffer->byte_length()); });
}

JS::NonnullGCPtr<JS::Object> BufferableObjectBase::raw_object()
{
    return m_bufferable_object.visit([](auto const& obj) -> JS::NonnullGCPtr<JS::Object> { return obj; });
}

JS::GCPtr<JS::ArrayBuffer> BufferableObjectBase::viewed_array_buffer()
{
    return m_bufferable_object.visit(
        [](JS::NonnullGCPtr<JS::ArrayBuffer> array_buffer) -> JS::GCPtr<JS::ArrayBuffer> { return array_buffer; },
        [](auto const& view) -> JS::GCPtr<JS::ArrayBuffer> { return view->viewed_array_buffer(); });
}

BufferableObject BufferableObjectBase::bufferable_object_from_raw_object(JS::NonnullGCPtr<JS::Object> object)
{
    if (is<JS::TypedArrayBase>(*object))
        return JS::NonnullGCPtr { static_cast<JS::TypedArrayBase&>(*object) };

    if (is<JS::DataView>(*object))
        return JS::NonnullGCPtr { static_cast<JS::DataView&>(*object) };

    if (is<JS::ArrayBuffer>(*object))
        return JS::NonnullGCPtr { static_cast<JS::ArrayBuffer&>(*object) };

    VERIFY_NOT_REACHED();
}

BufferableObjectBase::BufferableObjectBase(JS::NonnullGCPtr<JS::Object> object)
    : m_bufferable_object(bufferable_object_from_raw_object(object))
{
}

bool BufferableObjectBase::is_typed_array_base() const
{
    return m_bufferable_object.has<JS::NonnullGCPtr<JS::TypedArrayBase>>();
}

bool BufferableObjectBase::is_data_view() const
{
    return m_bufferable_object.has<JS::NonnullGCPtr<JS::DataView>>();
}

bool BufferableObjectBase::is_array_buffer() const
{
    return m_bufferable_object.has<JS::NonnullGCPtr<JS::ArrayBuffer>>();
}

void BufferableObjectBase::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_bufferable_object.visit([&](auto& obj) { visitor.visit(obj); });
}

ArrayBufferView::~ArrayBufferView() = default;

u32 ArrayBufferView::byte_offset() const
{
    return m_bufferable_object.visit(
        [](JS::NonnullGCPtr<JS::ArrayBuffer>) -> u32 { VERIFY_NOT_REACHED(); },
        [](auto& view) -> u32 { return static_cast<u32>(view->byte_offset()); });
}

BufferSource::~BufferSource() = default;

}
