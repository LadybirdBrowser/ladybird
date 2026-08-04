/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::FileAPI {

GC_DEFINE_ALLOCATOR(FileList);

GC::Ref<FileList> FileList::create()
{
    return GC::Heap::the().allocate<FileList>();
}

FileList::FileList()
{
}

FileList::~FileList() = default;

void FileList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_files);
}

WebIDL::ExceptionOr<void> FileList::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    // 1. Set serialized.[[Files]] to an empty list.
    // 2. For each file in value, append the sub-serialization of file to serialized.[[Files]].
    serialized.encode(static_cast<u64>(m_files.size()));

    for (auto file : m_files) {
        serialized.encode(to_underlying(HTML::ValueTag::SerializableObject));
        serialized.encode(Bindings::InterfaceName::File);
        if (serialized.type() == HTML::SerializationType::Storage)
            serialized.encode(u64 { 1 });
        TRY(file->serialization_steps(serialized, for_storage, memory));
    }

    return {};
}

WebIDL::ExceptionOr<void> FileList::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory& memory)
{
    // 1. For each file of serialized.[[Files]], add the sub-deserialization of file to value.
    auto size = TRY(HTML::decode_or_throw_data_clone_error<u64>(realm, serialized));

    for (size_t i = 0; i < size; ++i) {
        auto tag = TRY(HTML::decode_or_throw_data_clone_error<u8>(realm, serialized));
        auto interface_name = TRY(HTML::decode_or_throw_data_clone_error<Bindings::InterfaceName>(realm, serialized));
        if (tag != to_underlying(HTML::ValueTag::SerializableObject) || interface_name != Bindings::InterfaceName::File)
            return WebIDL::DataCloneError::create("Invalid FileList entry"_utf16);
        if (serialized.type() == HTML::SerializationType::Storage) {
            auto version = TRY(HTML::decode_or_throw_data_clone_error<u64>(realm, serialized));
            if (version != 1)
                return WebIDL::DataCloneError::create("Unsupported FileList entry version"_utf16);
        }
        auto file = File::create();
        TRY(file->deserialization_steps(realm, serialized, memory));
        m_files.append(file);
    }

    return {};
}

}
