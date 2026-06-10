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

WebIDL::ExceptionOr<void> FileList::serialization_steps(JS::Realm& realm, HTML::TransferDataEncoder& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    // 1. Set serialized.[[Files]] to an empty list.
    // 2. For each file in value, append the sub-serialization of file to serialized.[[Files]].
    serialized.encode(m_files.size());

    for (auto file : m_files)
        TRY(file->serialization_steps(realm, serialized, for_storage, memory));

    return {};
}

WebIDL::ExceptionOr<void> FileList::deserialization_steps(JS::Realm& realm, HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory& memory)
{
    // 1. For each file of serialized.[[Files]], add the sub-deserialization of file to value.
    auto size = serialized.decode<size_t>();

    for (size_t i = 0; i < size; ++i) {
        auto file = File::create();
        TRY(file->deserialization_steps(realm, serialized, memory));
        m_files.append(file);
    }

    return {};
}

}
