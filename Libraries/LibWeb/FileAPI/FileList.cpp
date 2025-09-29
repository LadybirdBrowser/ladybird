/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/FileListPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::FileAPI {

GC_DEFINE_ALLOCATOR(FileList);

GC::Ref<FileList> FileList::create(JS::Realm& realm)
{
    return realm.create<FileList>(realm);
}

FileList::FileList(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = 1 };
}

FileList::~FileList() = default;

void FileList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FileList);
    Base::initialize(realm);
}

Optional<JS::Value> FileList::item_value(size_t index) const
{
    if (index >= m_files.size())
        return {};

    return m_files[index];
}

void FileList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_files);
}

WebIDL::ExceptionOr<void> FileList::serialization_steps(HTML::TransferDataEncoder& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    auto& vm = this->vm();

    // 1. Set serialized.[[Files]] to an empty list.
    // 2. For each file in value, append the sub-serialization of file to serialized.[[Files]].
    serialized.encode(m_files.size());

    for (auto file : m_files)
        serialized.append(TRY(HTML::structured_serialize_internal(vm, file, for_storage, memory)));

    return {};
}

WebIDL::ExceptionOr<void> FileList::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory& memory)
{
    auto& vm = this->vm();
    auto& realm = this->realm();

    // 1. For each file of serialized.[[Files]], add the sub-deserialization of file to value.
    auto size = serialized.decode<size_t>();

    for (size_t i = 0; i < size; ++i) {
        auto deserialized = TRY(HTML::structured_deserialize_internal(vm, serialized, realm, memory));
        m_files.append(as<File>(deserialized.as_object()));
    }

    return {};
}

}
