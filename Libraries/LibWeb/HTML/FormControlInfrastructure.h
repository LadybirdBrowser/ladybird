/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

struct SerializedFormData {
    Utf16String boundary;
    ByteBuffer serialized_data;
};

WebIDL::ExceptionOr<XHR::FormDataEntry> create_entry(JS::Realm& realm, Utf16View name, Variant<GC::Ref<FileAPI::Blob>, Utf16String> const& value, Optional<Utf16String> const& filename = {});
WebIDL::ExceptionOr<Optional<GC::ConservativeVector<XHR::FormDataEntry>>> construct_entry_list(JS::Realm&, HTMLFormElement&, GC::Ptr<HTMLElement> submitter = nullptr, Optional<Utf16String> encoding = {});
ErrorOr<Utf16String> normalize_line_breaks(Utf16View value);
ErrorOr<SerializedFormData> serialize_to_multipart_form_data(GC::ConservativeVector<XHR::FormDataEntry> const& entry_list);

}
