/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/ConservativeVector.h>
#include <LibWeb/Bindings/FormData.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/DOMURL/URLSearchParams.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::XHR {

// https://xhr.spec.whatwg.org/#interface-formdata
// The C++ side of a FormData, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/xhr): what C++ creates
// and reads.
class WEB_API FormData final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(FormData, form_data)

public:
    static GC::Ref<FormData> create(GC::ConservativeVector<FormDataEntry> const& entry_list);
    static GC::Ref<FormData> create(Vector<DOMURL::QueryParam> const& entry_list);
    static WebIDL::ExceptionOr<GC::Ref<FormData>> create_from_form(GC::Ref<HTML::HTMLFormElement> form, GC::Ptr<HTML::HTMLElement> submitter = nullptr);

    // https://xhr.spec.whatwg.org/#dom-formdata-get
    Variant<GC::Ref<FileAPI::File>, Utf16String, Empty> get(Utf16String const& name) const;
    // https://xhr.spec.whatwg.org/#dom-formdata-has
    bool has(Utf16String const& name) const;

    GC::ConservativeVector<FormDataEntry> entry_list() const;
};

}
