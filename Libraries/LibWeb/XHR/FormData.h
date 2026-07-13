/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/FormData.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOMURL/URLSearchParams.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::XHR {

// https://xhr.spec.whatwg.org/#interface-formdata
class FormData : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(FormData, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(FormData);

public:
    virtual ~FormData() override;

    static WebIDL::ExceptionOr<GC::Ref<FormData>> construct_impl(JS::Realm&, GC::Ptr<HTML::HTMLFormElement> form = {}, GC::Ptr<HTML::HTMLElement> submitter = nullptr);
    static WebIDL::ExceptionOr<GC::Ref<FormData>> construct_impl(JS::Realm&, GC::ConservativeVector<FormDataEntry> entry_list);

    static WebIDL::ExceptionOr<GC::Ref<FormData>> create(JS::Realm&, Vector<DOMURL::QueryParam> entry_list);
    static WebIDL::ExceptionOr<GC::Ref<FormData>> create(JS::Realm&, GC::ConservativeVector<FormDataEntry> entry_list);

    WebIDL::ExceptionOr<void> append(Utf16String const& name, Utf16String const& value);
    WebIDL::ExceptionOr<void> append(Utf16String const& name, GC::Ref<FileAPI::Blob> const& blob_value, Optional<Utf16String> const& filename = {});
    void delete_(Utf16String const& name);
    Variant<GC::Ref<FileAPI::File>, Utf16String, Empty> get(Utf16String const& name);
    WebIDL::ExceptionOr<Vector<FormDataEntryValue>> get_all(Utf16String const& name);
    bool has(Utf16String const& name);
    WebIDL::ExceptionOr<void> set(Utf16String const& name, Utf16String const& value);
    WebIDL::ExceptionOr<void> set(Utf16String const& name, GC::Ref<FileAPI::Blob> const& blob_value, Optional<Utf16String> const& filename = {});

    GC::ConservativeVector<FormDataEntry> entry_list() const;

    using ForEachCallback = Function<JS::ThrowCompletionOr<void>(Utf16String const&, FormDataEntryValue const&)>;
    JS::ThrowCompletionOr<void> for_each(ForEachCallback);

private:
    friend class FormDataIterator;

    explicit FormData(JS::Realm&, GC::ConservativeVector<FormDataEntry> entry_list);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> append_impl(Utf16String const& name, Variant<GC::Ref<FileAPI::Blob>, Utf16String> const& value, Optional<Utf16String> const& filename = {});
    WebIDL::ExceptionOr<void> set_impl(Utf16String const& name, Variant<GC::Ref<FileAPI::Blob>, Utf16String> const& value, Optional<Utf16String> const& filename = {});

    Vector<FormDataEntry> m_entry_list;
};

}
