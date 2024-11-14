/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

namespace Web::XHR {

// https://xhr.spec.whatwg.org/#formdataentryvalue
using FormDataEntryValue = Variant<GC::Root<FileAPI::File>, String>;

struct FormDataEntry {
    String name;
    FormDataEntryValue value;
};

}
