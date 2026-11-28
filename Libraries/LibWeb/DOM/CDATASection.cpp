/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/CDATASection.h>
#include <LibWeb/DOM/Document.h>

namespace Web::DOM {

static_assert(is_shell_of_rust_object<CDATASection, RustFFI::CDATASectionObject>);

CDATASection::CDATASection(VtableProbe probe)
    : Text(probe)
{
}

GC::Ref<CDATASection> CDATASection::create(Document& document, Utf16String data)
{
    return static_cast<CDATASection&>(*RustFFI::rust_dom_create_cdata_section(&document, move(data).into_raw()));
}

}
