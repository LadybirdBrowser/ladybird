/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

struct SerializedFormData {
    String boundary;
    ByteBuffer serialized_data;
};

WebIDL::ExceptionOr<XHR::FormDataEntry> create_entry(JS::Realm& realm, String const& name, Variant<JS::NonnullGCPtr<FileAPI::Blob>, String> const& value, Optional<String> const& filename = {});
WebIDL::ExceptionOr<Optional<Vector<XHR::FormDataEntry>>> construct_entry_list(JS::Realm&, HTMLFormElement&, JS::GCPtr<HTMLElement> submitter = nullptr, Optional<String> encoding = Optional<String> {});
ErrorOr<String> normalize_line_breaks(StringView value);
ErrorOr<SerializedFormData> serialize_to_multipart_form_data(Vector<XHR::FormDataEntry> const& entry_list);

}
