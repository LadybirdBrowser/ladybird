/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/Forward.h>

namespace Web::Layout {

enum class AnonymousBoxStyleKind : u8 {
    Wrapper,
    TableRow,
    TableCell,
    Table,
    InlineTable,
    MissingTableCell,
    TableWrapper,
    ButtonFlexWrapper,
    ButtonContentBox,
    FieldsetContentWrapper,
};

struct AnonymousBoxStyleOverrides {
    bool inline_block_wrapper { false };
    CSS::Overflow overflow_x { CSS::Overflow::Visible };
    CSS::Overflow overflow_y { CSS::Overflow::Visible };
};

[[nodiscard]] CSS::StyleRecordID derive_pinned_anonymous_box_style_record(CSS::StyleComputer const&, CSS::StyleRecordID parent_style_record, AnonymousBoxStyleKind, AnonymousBoxStyleOverrides const& = {});
[[nodiscard]] CSS::StyleRecordID reinherit_pinned_anonymous_box_style_record(CSS::StyleComputer const&, CSS::StyleRecordID style_record, CSS::StyleRecordID parent_style_record);

}
