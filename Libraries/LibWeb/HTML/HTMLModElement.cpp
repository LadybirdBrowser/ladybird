/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/HTML/HTMLModElement.h>

namespace Web::HTML {

HTMLModElement::HTMLModElement(DOM::VtableProbe probe)
    : HTMLElement(probe)
{
}

Optional<ARIA::Role> HTMLModElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-del
    if (local_name() == TagNames::del)
        return ARIA::Role::deletion;
    // https://www.w3.org/TR/html-aria/#el-ins
    if (local_name() == TagNames::ins)
        return ARIA::Role::insertion;
    VERIFY_NOT_REACHED();
}

}
