/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/File.h>
#include <LibWeb/Bindings/FormData.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/XHR/FormDataIterator.h>

namespace Web::Bindings {

template<>
void Intrinsics::create_web_prototype_and_constructor<FormDataIteratorPrototype>(JS::Realm& realm)
{
    auto prototype = realm.create<FormDataIteratorPrototype>(realm);
    m_prototypes.set("FormDataIterator"_utf16_fly_string, prototype);
}

static void set_form_data_iterator_prototype(JS::Realm& realm, XHR::FormDataIterator& iterator)
{
    static auto const& name = "FormDataIterator"_utf16_fly_string;
    Detail::set_prototype_for_interface_on<FormDataIteratorPrototype>(realm, iterator, name);
}

}

namespace Web::XHR {

GC_DEFINE_ALLOCATOR(FormDataIterator);

FormDataIterator::FormDataIterator(JS::Realm& realm, Web::XHR::FormData const& form_data, JS::Object::PropertyKind iterator_kind)
    : JS::Object(realm, nullptr)
    , m_form_data(form_data)
    , m_iterator_kind(iterator_kind)
{
}

FormDataIterator::~FormDataIterator() = default;

static JS::Value form_data_entry_value(JS::Realm& realm, FormDataEntryValue const& value)
{
    return value.visit(
        [&](GC::Ref<FileAPI::File> const& file) -> JS::Value {
            return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, file);
        },
        [&](Utf16String const& string) -> JS::Value {
            return JS::PrimitiveString::create(realm.vm(), string);
        });
}

GC::Ref<JS::Object> FormDataIterator::next(JS::Realm& realm)
{
    auto& vm = this->vm();

    if (m_index >= m_form_data->m_entry_list.size())
        return JS::create_iterator_result_object(realm, JS::js_undefined(), true);

    auto entry = m_form_data->m_entry_list[m_index++];
    if (m_iterator_kind == JS::Object::PropertyKind::Key)
        return JS::create_iterator_result_object(realm, JS::PrimitiveString::create(vm, entry.name), false);

    auto entry_value = form_data_entry_value(realm, entry.value);

    if (m_iterator_kind == JS::Object::PropertyKind::Value)
        return JS::create_iterator_result_object(realm, entry_value, false);

    return JS::create_iterator_result_object(realm, JS::Array::create_from(realm, { JS::PrimitiveString::create(vm, entry.name), entry_value }), false);
}

GC::Ref<FormDataIterator> FormDataIterator::create(JS::Realm& realm, FormData const& form_data, JS::Object::PropertyKind iterator_kind)
{
    auto iterator = realm.create<FormDataIterator>(realm, form_data, iterator_kind);
    Bindings::set_form_data_iterator_prototype(realm, iterator);
    return iterator;
}

void FormDataIterator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_form_data);
}

}
