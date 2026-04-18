/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibGfx/Font/Typeface.h>

namespace Web::CSS {

struct PreparedVectorFontData {
    ByteBuffer data;
    Optional<ByteString> mime_type_essence;
};

bool requires_off_thread_vector_font_preparation(ByteBuffer const&, Optional<ByteString> const& mime_type_essence = {});
ErrorOr<NonnullRefPtr<Gfx::Typeface const>> try_load_vector_font(ByteBuffer const&, Optional<ByteString> const& mime_type_essence = {});
void prepare_vector_font_data_off_thread(ByteBuffer, Function<void(ErrorOr<PreparedVectorFontData>)>&&, Optional<ByteString> mime_type_essence = {});

}
