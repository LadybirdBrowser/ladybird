/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/File.h>
#include <LibWeb/Bindings/FormData.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/XHR/FormDataIterator.h>

namespace Web::XHR {

GC_DEFINE_ALLOCATOR(FormDataIterator);

static constexpr JSHostClass form_data_iterator_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "FormDataIterator"sv, nullptr, nullptr, nullptr, 0);

FormDataIterator::FormDataIterator(Web::XHR::FormData const& form_data, JS::Object::PropertyKind iterator_kind)
    : m_form_data(form_data)
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

GC::Ref<JS::HostObject> FormDataIterator::create(JS::Realm& realm, FormData const& form_data, JS::Object::PropertyKind iterator_kind)
{
    static auto const& prototype_name = "FormDataIterator"_utf16_fly_string;
    auto iterator = realm.create<FormDataIterator>(form_data, iterator_kind);
    auto& prototype = Bindings::ensure_web_prototype<Bindings::FormDataIteratorPrototype>(realm, prototype_name);
    return JS::HostObject::create(realm, form_data_iterator_host_class, prototype, {}, iterator);
}

void FormDataIterator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_form_data);
}

}
