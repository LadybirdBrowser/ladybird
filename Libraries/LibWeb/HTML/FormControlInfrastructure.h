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

XHR::FormDataEntry create_entry(Utf16View name, Variant<GC::Ref<FileAPI::Blob>, Utf16String> const& value);
Optional<GC::ConservativeVector<XHR::FormDataEntry>> construct_entry_list(HTMLFormElement&, GC::Ptr<HTMLElement> submitter = nullptr, Optional<Utf16String> encoding = {});
ErrorOr<Utf16String> normalize_line_breaks(Utf16View value);
ErrorOr<ByteBuffer> encode(Utf16View input, StringView encoding);
ErrorOr<SerializedFormData> serialize_to_multipart_form_data(GC::ConservativeVector<XHR::FormDataEntry> const& entry_list, StringView encoding = "UTF-8"sv);

}
