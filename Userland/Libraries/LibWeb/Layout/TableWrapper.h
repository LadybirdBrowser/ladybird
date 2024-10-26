/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/BlockContainer.h>

namespace Web::Layout {

class TableWrapper : public BlockContainer {
    JS_CELL(TableWrapper, BlockContainer);
    JS_DECLARE_ALLOCATOR(TableWrapper);

public:
    TableWrapper(DOM::Document&, DOM::Node*, CSS::StyleProperties);
    TableWrapper(DOM::Document&, DOM::Node*, NonnullOwnPtr<CSS::ComputedValues>);
    virtual ~TableWrapper() override;

private:
    virtual bool is_table_wrapper() const final { return true; }
};

template<>
inline bool Node::fast_is<TableWrapper>() const { return is_table_wrapper(); }

}
