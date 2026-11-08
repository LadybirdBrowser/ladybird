/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/FormControlInfrastructure.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::XHR {

extern "C" {

// Defined in Bindings/Rust/src/xhr/exports.rs.
void* web_form_data_create(JSVM*, ReadonlySpan<FormDataEntryABI> entries);
size_t web_form_data_entry_count(void* form_data);
FormDataEntryABI web_form_data_entry(void* form_data, size_t index);
bool web_form_data_value_named(void* form_data, FlatPtr name, FormDataEntryValueABI* out_value);

// Called by Bindings/Rust/src/xhr/form_data.rs.
JSCompletion web_form_data_create_from_form(void* realm, JSObject* form, JSObject* submitter, void** out_form_data);
}

void FormDataEntry::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(value);
}

FormDataEntryABI form_data_entry_to_abi(FormDataEntry const& entry)
{
    FormDataEntryABI abi { .name = Utf16String { entry.name }.into_raw(), .value = { .file = nullptr, .string = 0 } };
    entry.value.visit(
        [&](GC::Ref<FileAPI::File> file) { abi.value.file = file.ptr(); },
        [&](Utf16String const& string) { abi.value.string = Utf16String { string }.into_raw(); });
    return abi;
}

FormDataEntry::Value form_data_entry_value_from_abi(FormDataEntryValueABI value)
{
    if (value.file)
        return GC::Ref { *static_cast<FileAPI::File*>(value.file) };
    return Utf16String::adopt_raw(value.string);
}

FormDataEntry form_data_entry_from_abi(FormDataEntryABI entry)
{
    return { .name = Utf16String::adopt_raw(entry.name), .value = form_data_entry_value_from_abi(entry.value) };
}

GC::Ref<FormData> FormData::create(GC::RootVector<FormDataEntry> const& entry_list)
{
    Vector<FormDataEntryABI> entries;
    entries.ensure_capacity(entry_list.size());
    for (auto const& entry : entry_list)
        entries.unchecked_append(form_data_entry_to_abi(entry));
    return GC::Ref { *static_cast<FormData*>(web_form_data_create(JS::HostABI::vm_to_abi(JS::VM::the()), entries)) };
}

GC::Ref<FormData> FormData::create(Vector<DOMURL::QueryParam> const& entry_list)
{
    GC::RootVector<FormDataEntry> list;
    list.ensure_capacity(entry_list.size());
    for (auto const& entry : entry_list)
        list.unchecked_append({ .name = entry.name, .value = entry.value });
    return create(list);
}

// https://xhr.spec.whatwg.org/#dom-formdata
static WebIDL::ExceptionOr<GC::RootVector<FormDataEntry>> entry_list_for_form(GC::Ref<HTML::HTMLFormElement> form, GC::Ptr<HTML::HTMLElement> submitter)
{
    // 1. If form is given, then:
    // 1. If submitter is non-null, then:
    if (submitter) {
        // 1. If submitter is not a submit button, then throw a TypeError.
        auto form_associated_element = as_if<HTML::FormAssociatedElement>(*submitter);
        if (!form_associated_element) {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Submitter is not associated with a form."_utf16 };
        }

        if (!form_associated_element->is_submit_button()) {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Submitter is not a valid submit button."_utf16 };
        }
        // 2. If submitter’s form owner is not form, then throw a "NotFoundError" DOMException.
        GC::Ptr form_owner = form_associated_element->form();
        if (form_owner && form_owner != form) {
            return WebIDL::NotFoundError::create("Submitter does not belong to the provided form."_utf16);
        }
    }

    // 2. Let list be the result of constructing the entry list for form and submitter.
    auto entry_list = HTML::construct_entry_list(*form, submitter);
    // 3. If list is null, then throw an "InvalidStateError" DOMException.
    if (!entry_list.has_value())
        return WebIDL::InvalidStateError::create("Form element does not contain any entries."_utf16);
    // 4. Set this’s entry list to list.
    return entry_list.release_value();
}

WebIDL::ExceptionOr<GC::Ref<FormData>> FormData::create_from_form(GC::Ref<HTML::HTMLFormElement> form, GC::Ptr<HTML::HTMLElement> submitter)
{
    return create(TRY(entry_list_for_form(form, submitter)));
}

JSCompletion web_form_data_create_from_form(void* realm, JSObject* form, JSObject* submitter, void** out_form_data)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto& form_element = as<HTML::HTMLFormElement>(*Bindings::wrappable_impl_from(JS::HostABI::object_from_abi(form)));
    GC::Ptr<HTML::HTMLElement> submitter_element;
    if (submitter)
        submitter_element = as<HTML::HTMLElement>(*Bindings::wrappable_impl_from(JS::HostABI::object_from_abi(submitter)));

    auto form_data_or_error = FormData::create_from_form(form_element, submitter_element);
    if (form_data_or_error.is_exception())
        return Bindings::completion_to_abi(js_realm, form_data_or_error.release_error());
    *out_form_data = form_data_or_error.release_value().ptr();
    return Bindings::completion_to_abi(js_realm, JS::js_undefined());
}

Variant<GC::Ref<FileAPI::File>, Utf16String, Empty> FormData::get(Utf16String const& name) const
{
    FormDataEntryValueABI value {};
    if (!web_form_data_value_named(const_cast<FormData*>(this), Utf16String { name }.into_raw(), &value))
        return Empty {};
    return form_data_entry_value_from_abi(value).visit([](auto value) -> Variant<GC::Ref<FileAPI::File>, Utf16String, Empty> { return value; });
}

bool FormData::has(Utf16String const& name) const
{
    return !get(name).has<Empty>();
}

GC::RootVector<FormDataEntry> FormData::entry_list() const
{
    auto count = web_form_data_entry_count(const_cast<FormData*>(this));
    GC::RootVector<FormDataEntry> entry_list;
    entry_list.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index)
        entry_list.unchecked_append(form_data_entry_from_abi(web_form_data_entry(const_cast<FormData*>(this), index)));
    return entry_list;
}

}
