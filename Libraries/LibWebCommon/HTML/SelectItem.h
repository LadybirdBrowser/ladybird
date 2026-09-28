/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::HTML {

struct SelectItemOption {
    u32 id { 0 };
    bool selected { false };
    bool disabled { false };
    Utf16String label {};
    Utf16String value {};
};

struct SelectItemOptionGroup {
    Utf16String label = {};
    Vector<SelectItemOption> items = {};
};

struct SelectItemSeparator { };

using SelectItem = Variant<SelectItemOption, SelectItemOptionGroup, SelectItemSeparator>;

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SelectItemOption const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SelectItemOption> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SelectItemOptionGroup const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SelectItemOptionGroup> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SelectItemSeparator const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SelectItemSeparator> decode(Decoder&);

}
