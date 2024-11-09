/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibIPC/Forward.h>
#include <LibWeb/HTML/HTMLOptionElement.h>

namespace Web::HTML {

struct SelectItemOption {
    u32 id { 0 };
    String label {};
    String value {};
    bool selected { false };
    bool disabled { false };
    JS::GCPtr<HTMLOptionElement> option_element {};
};

struct SelectItemOptionGroup {
    String label = {};
    Vector<SelectItemOption> items = {};
};

struct SelectItemSeparator { };

using SelectItem = Variant<SelectItemOption, SelectItemOptionGroup, SelectItemSeparator>;

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::SelectItemOption const&);

template<>
ErrorOr<Web::HTML::SelectItemOption> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::SelectItemOptionGroup const&);

template<>
ErrorOr<Web::HTML::SelectItemOptionGroup> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::SelectItemSeparator const&);

template<>
ErrorOr<Web::HTML::SelectItemSeparator> decode(Decoder&);

}
