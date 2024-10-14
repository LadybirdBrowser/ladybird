/*
 * Copyright (c) 2022-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Bindings/FilePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::FileAPI {

JS_DEFINE_ALLOCATOR(File);

File::File(JS::Realm& realm, ByteBuffer byte_buffer, String file_name, String type, i64 last_modified)
    : Blob(realm, move(byte_buffer), move(type))
    , m_name(move(file_name))
    , m_last_modified(last_modified)
{
}

File::File(JS::Realm& realm)
    : Blob(realm, {})
{
}

void File::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(File);
}

File::~File() = default;

JS::NonnullGCPtr<File> File::create(JS::Realm& realm)
{
    return realm.heap().allocate<File>(realm, realm);
}

// https://w3c.github.io/FileAPI/#ref-for-dom-file-file
WebIDL::ExceptionOr<JS::NonnullGCPtr<File>> File::create(JS::Realm& realm, Vector<BlobPart> const& file_bits, String const& file_name, Optional<FilePropertyBag> const& options)
{
    auto& vm = realm.vm();

    // 1. Let bytes be the result of processing blob parts given fileBits and options.
    auto bytes = TRY_OR_THROW_OOM(vm, process_blob_parts(file_bits, options.has_value() ? static_cast<BlobPropertyBag const&>(*options) : Optional<BlobPropertyBag> {}));

    // 2. Let n be the fileName argument to the constructor.
    //    NOTE: Underlying OS filesystems use differing conventions for file name; with constructed files, mandating UTF-16 lessens ambiquity when file names are converted to byte sequences.
    auto name = file_name;

    auto type = String {};
    i64 last_modified = 0;
    // 3. Process FilePropertyBag dictionary argument by running the following substeps:
    if (options.has_value()) {
        // FIXME: 1. If the type member is provided and is not the empty string, let t be set to the type dictionary member.
        //    If t contains any characters outside the range U+0020 to U+007E, then set t to the empty string and return from these substeps.
        // FIXME: 2. Convert every character in t to ASCII lowercase.

        // NOTE: The spec is out of date, and we are supposed to call into the MimeType parser here.
        auto maybe_parsed_type = Web::MimeSniff::MimeType::parse(options->type);

        if (maybe_parsed_type.has_value())
            type = maybe_parsed_type->serialized();

        // 3. If the lastModified member is provided, let d be set to the lastModified dictionary member. If it is not provided, set d to the current date and time represented as the number of milliseconds since the Unix Epoch (which is the equivalent of Date.now() [ECMA-262]).
        //    Note: Since ECMA-262 Date objects convert to long long values representing the number of milliseconds since the Unix Epoch, the lastModified member could be a Date object [ECMA-262].
        last_modified = options->last_modified.has_value() ? options->last_modified.value() : UnixDateTime::now().milliseconds_since_epoch();
    }

    // 4. Return a new File object F such that:
    //    2. F refers to the bytes byte sequence.
    //       NOTE: Spec started at 2 therefore keeping the same number sequence here.
    //    3. F.size is set to the number of total bytes in bytes.
    //    4. F.name is set to n.
    //    5. F.type is set to t.
    //    6. F.lastModified is set to d.
    return realm.heap().allocate<File>(realm, realm, move(bytes), move(name), move(type), last_modified);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<File>> File::construct_impl(JS::Realm& realm, Vector<BlobPart> const& file_bits, String const& file_name, Optional<FilePropertyBag> const& options)
{
    return create(realm, file_bits, file_name, options);
}

WebIDL::ExceptionOr<void> File::serialization_steps(HTML::SerializationRecord& record, bool, HTML::SerializationMemory&)
{
    auto& vm = this->vm();

    // FIXME: 1. Set serialized.[[SnapshotState]] to value’s snapshot state.

    // NON-STANDARD: FileAPI spec doesn't specify that type should be serialized, although
    //               to be conformant with other browsers this needs to be serialized.
    TRY(HTML::serialize_string(vm, record, m_type));

    // 2. Set serialized.[[ByteSequence]] to value’s underlying byte sequence.
    TRY(HTML::serialize_bytes(vm, record, m_byte_buffer.bytes()));

    // 3. Set serialized.[[Name]] to the value of value’s name attribute.
    TRY(HTML::serialize_string(vm, record, m_name));

    // 4. Set serialized.[[LastModified]] to the value of value’s lastModified attribute.
    HTML::serialize_primitive_type(record, m_last_modified);

    return {};
}

WebIDL::ExceptionOr<void> File::deserialization_steps(ReadonlySpan<u32> const& record, size_t& position, HTML::DeserializationMemory&)
{
    auto& vm = this->vm();

    // FIXME: 1. Set value’s snapshot state to serialized.[[SnapshotState]].

    // NON-STANDARD: FileAPI spec doesn't specify that type should be deserialized, although
    //               to be conformant with other browsers this needs to be deserialized.
    m_type = TRY(HTML::deserialize_string(vm, record, position));

    // 2. Set value’s underlying byte sequence to serialized.[[ByteSequence]].
    m_byte_buffer = TRY(HTML::deserialize_bytes(vm, record, position));

    // 3. Initialize the value of value’s name attribute to serialized.[[Name]].
    m_name = TRY(HTML::deserialize_string(vm, record, position));

    // 4. Initialize the value of value’s lastModified attribute to serialized.[[LastModified]].
    m_last_modified = HTML::deserialize_primitive_type<i64>(record, position);

    return {};
}

}
