/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

struct PrintSettings {
    float paper_width_mm { 210.0f }; // A4 portrait default
    float paper_height_mm { 297.0f };
    float margin_top_mm { 12.7f }; // ~0.5 inch default
    float margin_right_mm { 12.7f };
    float margin_bottom_mm { 12.7f };
    float margin_left_mm { 12.7f };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::PrintSettings const&);

template<>
WEB_API ErrorOr<Web::HTML::PrintSettings> decode(Decoder&);

}
