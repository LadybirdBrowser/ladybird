/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::FileAPI {

GC_DEFINE_ALLOCATOR(FileList);

GC::Ref<FileList> FileList::create(JS::Realm& realm)
{
    return realm.create<FileList>(realm);
}

FileList::FileList(JS::Realm& realm)
    : Bindings::Wrappable(realm)
{
}

FileList::~FileList() = default;

Optional<JS::Value> FileList::item_value(JS::Realm& realm, size_t index) const
{
    if (index >= m_files.size())
        return {};

    return Bindings::wrap(realm, m_files[index]);
}

void FileList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_files);
}

WebIDL::ExceptionOr<void> FileList::serialization_steps(HTML::TransferDataEncoder& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Set serialized.[[Files]] to an empty list.
    // 2. For each file in value, append the sub-serialization of file to serialized.[[Files]].
    serialized.encode(m_files.size());

    for (auto file : m_files)
        serialized.append(TRY(HTML::structured_serialize_internal(vm, Bindings::wrap(realm, file), for_storage, memory)));

    return {};
}

WebIDL::ExceptionOr<void> FileList::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory& memory)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. For each file of serialized.[[Files]], add the sub-deserialization of file to value.
    auto size = serialized.decode<size_t>();

    for (size_t i = 0; i < size; ++i) {
        auto deserialized = TRY(HTML::structured_deserialize_internal(vm, serialized, realm, memory));
        auto* file = Bindings::impl_from<File>(&deserialized.as_object());
        VERIFY(file);
        m_files.append(*file);
    }

    return {};
}

}
