/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleRecordID.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>

namespace Web::Layout {

[[nodiscard]] CSS::StyleRecordID derive_pinned_anonymous_box_style_record(CSS::StyleComputer const&, CSS::StyleRecordID parent_style_record, RustFFI::FfiAnonymousStyleKind, RustFFI::FfiAnonymousStyleOverrides const&);
[[nodiscard]] CSS::StyleRecordID reinherit_pinned_anonymous_box_style_record(CSS::StyleComputer const&, CSS::StyleRecordID style_record, CSS::StyleRecordID parent_style_record);

}
