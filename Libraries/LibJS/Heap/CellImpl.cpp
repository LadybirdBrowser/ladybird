/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/CellImpl.h>
#include <LibJS/Heap/NanBoxedValue.h>

namespace JS {

void JS::CellImpl::Visitor::visit(NanBoxedValue const& value)
{
    if (value.is_cell())
        visit_impl(value.as_cell());
}

}
