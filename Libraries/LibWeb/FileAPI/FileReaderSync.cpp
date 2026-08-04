/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/FileReaderSync.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::FileAPI {

GC_DEFINE_ALLOCATOR(FileReaderSync);

FileReaderSync::~FileReaderSync() = default;

FileReaderSync::FileReaderSync()
{
}

GC::Ref<FileReaderSync> FileReaderSync::create()
{
    return GC::Heap::the().allocate<FileReaderSync>();
}

// https://w3c.github.io/FileAPI/#dfn-readAsArrayBufferSync
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> FileReaderSync::read_as_array_buffer(JS::Object const& relevant_global_object, Blob& blob)
{
    auto& target = HTML::relevant_realm(relevant_global_object);
    auto bytes = TRY(read_as_array_buffer_impl(blob));
    return JS::ArrayBuffer::create(target, move(bytes));
}

// https://w3c.github.io/FileAPI/#dfn-readAsArrayBufferSync
WebIDL::ExceptionOr<ByteBuffer> FileReaderSync::read_as_array_buffer_impl(Blob& blob)
{
    return MUST(ByteBuffer::copy(blob.raw_bytes()));
}

// https://w3c.github.io/FileAPI/#dfn-readAsBinaryStringSync
WebIDL::ExceptionOr<Utf16String> FileReaderSync::read_as_binary_string(Blob& blob)
{
    return read_as<Utf16String>(blob, FileReader::Type::BinaryString);
}

// https://w3c.github.io/FileAPI/#dfn-readAsTextSync
WebIDL::ExceptionOr<Utf16String> FileReaderSync::read_as_text(Blob& blob, Optional<Utf16String> const& encoding)
{
    return read_as<Utf16String>(blob, FileReader::Type::Text, encoding);
}

// https://w3c.github.io/FileAPI/#dfn-readAsDataURLSync
WebIDL::ExceptionOr<Utf16String> FileReaderSync::read_as_data_url(Blob& blob)
{
    return read_as<Utf16String>(blob, FileReader::Type::DataURL);
}

template<typename Result>
WebIDL::ExceptionOr<Result> FileReaderSync::read_as(Blob& blob, FileReader::Type type, Optional<Utf16String> const& encoding)
{
    auto blob_type = blob.type();
    auto result = TRY(FileReader::blob_package_data(MUST(ByteBuffer::copy(blob.raw_bytes())), type, blob_type, encoding));
    return result.get<Result>();
}

}
