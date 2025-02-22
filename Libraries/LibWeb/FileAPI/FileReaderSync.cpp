/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/FileReaderSyncPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/FileReaderSync.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::FileAPI {

GC_DEFINE_ALLOCATOR(FileReaderSync);

FileReaderSync::~FileReaderSync() = default;

FileReaderSync::FileReaderSync(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void FileReaderSync::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FileReaderSync);
}

GC::Ref<FileReaderSync> FileReaderSync::create(JS::Realm& realm)
{
    return realm.create<FileReaderSync>(realm);
}

GC::Ref<FileReaderSync> FileReaderSync::construct_impl(JS::Realm& realm)
{
    return FileReaderSync::create(realm);
}

// https://w3c.github.io/FileAPI/#dfn-readAsArrayBufferSync
WebIDL::ExceptionOr<GC::Root<JS::ArrayBuffer>> FileReaderSync::read_as_array_buffer(Blob& blob)
{
    return read_as<GC::Root<JS::ArrayBuffer>>(blob, FileReader::Type::ArrayBuffer);
}

// https://w3c.github.io/FileAPI/#dfn-readAsBinaryStringSync
WebIDL::ExceptionOr<String> FileReaderSync::read_as_binary_string(Blob& blob)
{
    return read_as<String>(blob, FileReader::Type::BinaryString);
}

// https://w3c.github.io/FileAPI/#dfn-readAsTextSync
WebIDL::ExceptionOr<String> FileReaderSync::read_as_text(Blob& blob, Optional<String> const& encoding)
{
    return read_as<String>(blob, FileReader::Type::Text, encoding);
}

// https://w3c.github.io/FileAPI/#dfn-readAsDataURLSync
WebIDL::ExceptionOr<String> FileReaderSync::read_as_data_url(Blob& blob)
{
    return read_as<String>(blob, FileReader::Type::DataURL);
}

template<typename Result>
WebIDL::ExceptionOr<Result> FileReaderSync::read_as(Blob& blob, FileReader::Type type, Optional<String> const& encoding)
{
    // 1. Let stream be the result of calling get stream on blob.
    auto stream = blob.get_stream();

    // 2. Let reader be the result of getting a reader from stream.
    auto reader = TRY(stream->get_a_reader());

    // 3. Let promise be the result of reading all bytes from stream with reader.
    auto promise_capability = reader->read_all_bytes_deprecated();

    // FIXME: Try harder to not reach into promise's [[Promise]] slot
    auto promise = GC::Ref { as<JS::Promise>(*promise_capability->promise()) };

    // 4. Wait for promise to be fulfilled or rejected.
    // FIXME: Create spec issue to use WebIDL react to promise steps here instead of this custom logic
    HTML::main_thread_event_loop().spin_until(GC::create_function(heap(), [promise]() {
        return promise->state() == JS::Promise::State::Fulfilled || promise->state() == JS::Promise::State::Rejected;
    }));

    // 5. If promise fulfilled with a byte sequence bytes:
    auto result = promise->result();
    auto* array_buffer = result.extract_pointer<JS::ArrayBuffer>();
    if (promise->state() == JS::Promise::State::Fulfilled && array_buffer) {
        // AD-HOC: This diverges from the spec as wrritten, where the type argument is specified explicitly for each caller.
        // 1. Return the result of package data given bytes, type, blob’s type, and encoding.
        auto result = TRY(FileReader::blob_package_data(realm(), array_buffer->buffer(), type, blob.type(), encoding));
        return result.get<Result>();
    }

    // 6. Throw promise’s rejection reason.
    VERIFY(promise->state() == JS::Promise::State::Rejected);
    return JS::throw_completion(result);
}

}
