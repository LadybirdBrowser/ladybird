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

namespace Web::FileAPI {

JS_DEFINE_ALLOCATOR(FileList);

JS::NonnullGCPtr<FileList> FileList::create(JS::Realm& realm)
{
    return realm.heap().allocate<FileList>(realm, realm);
}

FileList::FileList(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = 1 };
}

FileList::~FileList() = default;

void FileList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FileList);
}

Optional<JS::Value> FileList::item_value(size_t index) const
{
    if (index >= m_files.size())
        return {};

    return m_files[index].ptr();
}

void FileList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_files);
}

WebIDL::ExceptionOr<void> FileList::serialization_steps(HTML::SerializationRecord& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    auto& vm = this->vm();

    // 1. Set serialized.[[Files]] to an empty list.
    // 2. For each file in value, append the sub-serialization of file to serialized.[[Files]].
    HTML::serialize_primitive_type(serialized, m_files.size());
    for (auto& file : m_files)
        serialized.extend(TRY(HTML::structured_serialize_internal(vm, file, for_storage, memory)));

    return {};
}

WebIDL::ExceptionOr<void> FileList::deserialization_steps(ReadonlySpan<u32> const& serialized, size_t& position, HTML::DeserializationMemory& memory)
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 1. For each file of serialized.[[Files]], add the sub-deserialization of file to value.
    auto size = HTML::deserialize_primitive_type<size_t>(serialized, position);
    for (size_t i = 0; i < size; ++i) {
        auto deserialized_record = TRY(HTML::structured_deserialize_internal(vm, serialized, realm, memory, position));
        if (deserialized_record.value.has_value() && is<File>(deserialized_record.value.value().as_object()))
            m_files.append(dynamic_cast<File&>(deserialized_record.value.release_value().as_object()));
        position = deserialized_record.position;
    }

    return {};
}

}
