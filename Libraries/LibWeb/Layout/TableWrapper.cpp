/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TableWrapper.h>

namespace Web::Layout {

TableWrapper::TableWrapper(DOM::Document& document, DOM::Node* node, CSS::ComputedProperties const& style)
    : BlockContainer(document, node, style)
{
}

TableWrapper::TableWrapper(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : BlockContainer(document, node, move(computed_values))
{
}

TableWrapper::~TableWrapper() = default;

}
