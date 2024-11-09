/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/FormControlInfrastructure.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::XHR {

JS_DEFINE_ALLOCATOR(FormData);

// https://xhr.spec.whatwg.org/#dom-formdata
WebIDL::ExceptionOr<JS::NonnullGCPtr<FormData>> FormData::construct_impl(JS::Realm& realm, JS::GCPtr<HTML::HTMLFormElement> form)
{
    Vector<FormDataEntry> list;
    // 1. If form is given, then:
    if (form) {
        // 1. Let list be the result of constructing the entry list for form.
        auto entry_list = TRY(construct_entry_list(realm, *form));
        // 2. If list is null, then throw an "InvalidStateError" DOMException.
        if (!entry_list.has_value())
            return WebIDL::InvalidStateError::create(realm, "Form element does not contain any entries."_string);
        // 3. Set this’s entry list to list.
        list = entry_list.release_value();
    }

    return construct_impl(realm, move(list));
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<FormData>> FormData::construct_impl(JS::Realm& realm, Vector<FormDataEntry> entry_list)
{
    return realm.heap().allocate<FormData>(realm, realm, move(entry_list));
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<FormData>> FormData::create(JS::Realm& realm, Vector<DOMURL::QueryParam> entry_list)
{
    Vector<FormDataEntry> list;
    list.ensure_capacity(entry_list.size());
    for (auto& entry : entry_list)
        list.unchecked_append({ .name = move(entry.name), .value = move(entry.value) });

    return construct_impl(realm, move(list));
}

FormData::FormData(JS::Realm& realm, Vector<FormDataEntry> entry_list)
    : PlatformObject(realm)
    , m_entry_list(move(entry_list))
{
}

FormData::~FormData() = default;

void FormData::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FormData);
}

// https://xhr.spec.whatwg.org/#dom-formdata-append
WebIDL::ExceptionOr<void> FormData::append(String const& name, String const& value)
{
    return append_impl(name, value);
}

// https://xhr.spec.whatwg.org/#dom-formdata-append-blob
WebIDL::ExceptionOr<void> FormData::append(String const& name, JS::NonnullGCPtr<FileAPI::Blob> const& blob_value, Optional<String> const& filename)
{
    auto inner_filename = filename.has_value() ? filename.value() : Optional<String> {};
    return append_impl(name, blob_value, inner_filename);
}

// https://xhr.spec.whatwg.org/#dom-formdata-append
// https://xhr.spec.whatwg.org/#dom-formdata-append-blob
WebIDL::ExceptionOr<void> FormData::append_impl(String const& name, Variant<JS::NonnullGCPtr<FileAPI::Blob>, String> const& value, Optional<String> const& filename)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let value be value if given; otherwise blobValue.
    // 2. Let entry be the result of creating an entry with name, value, and filename if given.
    auto entry = TRY(HTML::create_entry(realm, name, value, filename));

    // 3. Append entry to this’s entry list.
    TRY_OR_THROW_OOM(vm, m_entry_list.try_append(move(entry)));
    return {};
}

// https://xhr.spec.whatwg.org/#dom-formdata-delete
void FormData::delete_(String const& name)
{
    // The delete(name) method steps are to remove all entries whose name is name from this’s entry list.
    m_entry_list.remove_all_matching([&name](FormDataEntry const& entry) {
        return entry.name == name;
    });
}

// https://xhr.spec.whatwg.org/#dom-formdata-get
Variant<JS::Handle<FileAPI::File>, String, Empty> FormData::get(String const& name)
{
    // 1. If there is no entry whose name is name in this’s entry list, then return null.
    auto entry_iterator = m_entry_list.find_if([&name](FormDataEntry const& entry) {
        return entry.name == name;
    });
    if (entry_iterator.is_end())
        return Empty {};
    // 2. Return the value of the first entry whose name is name from this’s entry list.
    return entry_iterator->value;
}

// https://xhr.spec.whatwg.org/#dom-formdata-getall
WebIDL::ExceptionOr<Vector<FormDataEntryValue>> FormData::get_all(String const& name)
{
    // 1. If there is no entry whose name is name in this’s entry list, then return the empty list.
    // 2. Return the values of all entries whose name is name, in order, from this’s entry list.
    Vector<FormDataEntryValue> values;
    for (auto const& entry : m_entry_list) {
        if (entry.name == name)
            TRY_OR_THROW_OOM(vm(), values.try_append(entry.value));
    }
    return values;
}

// https://xhr.spec.whatwg.org/#dom-formdata-has
bool FormData::has(String const& name)
{
    // The has(name) method steps are to return true if there is an entry whose name is name in this’s entry list; otherwise false.
    return !m_entry_list.find_if([&name](auto& entry) {
                            return entry.name == name;
                        })
                .is_end();
}

// https://xhr.spec.whatwg.org/#dom-formdata-set
WebIDL::ExceptionOr<void> FormData::set(String const& name, String const& value)
{
    return set_impl(name, value);
}

// https://xhr.spec.whatwg.org/#dom-formdata-set-blob
WebIDL::ExceptionOr<void> FormData::set(String const& name, JS::NonnullGCPtr<FileAPI::Blob> const& blob_value, Optional<String> const& filename)
{
    auto inner_filename = filename.has_value() ? filename.value() : Optional<String> {};
    return set_impl(name, blob_value, inner_filename);
}

// https://xhr.spec.whatwg.org/#dom-formdata-set
// https://xhr.spec.whatwg.org/#dom-formdata-set-blob
WebIDL::ExceptionOr<void> FormData::set_impl(String const& name, Variant<JS::NonnullGCPtr<FileAPI::Blob>, String> const& value, Optional<String> const& filename)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let value be value if given; otherwise blobValue.
    // 2. Let entry be the result of creating an entry with name, value, and filename if given.
    auto entry = TRY(HTML::create_entry(realm, name, value, filename));

    auto existing = m_entry_list.find_if([&name](auto& entry) {
        return entry.name == name;
    });

    // 3. If there are entries in this’s entry list whose name is name, then replace the first such entry with entry and remove the others.
    if (!existing.is_end()) {
        existing->value = entry.value;
        m_entry_list.remove_all_matching([&name, &existing](auto& entry) {
            return &entry != &*existing && entry.name == name;
        });
    }
    // 4. Otherwise, append entry to this’s entry list.
    else {
        TRY_OR_THROW_OOM(vm, m_entry_list.try_append(move(entry)));
    }

    return {};
}

JS::ThrowCompletionOr<void> FormData::for_each(ForEachCallback callback)
{
    for (auto i = 0u; i < m_entry_list.size(); ++i) {
        auto& entry = m_entry_list[i];
        TRY(callback(entry.name, entry.value));
    }

    return {};
}

}
