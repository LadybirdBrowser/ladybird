/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibGC/Root.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::XHR {

// https://xhr.spec.whatwg.org/#formdataentryvalue
using FormDataEntryValue = Variant<GC::Ref<FileAPI::File>, Utf16String>;

struct FormDataEntry {
    using Value = Variant<GC::Ref<FileAPI::File>, Utf16String>;

    Utf16String name;
    Value value;
};

struct FormDataEntryValueABI {
    void* file;
    FlatPtr string;
};

struct FormDataEntryABI {
    FlatPtr name;
    FormDataEntryValueABI value;
};

WEB_API FormDataEntryABI form_data_entry_to_abi(FormDataEntry const&);
WEB_API FormDataEntry::Value form_data_entry_value_from_abi(FormDataEntryValueABI);
WEB_API FormDataEntry form_data_entry_from_abi(FormDataEntryABI);

}
