/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
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

struct WEBCOMMON_API FileFilter {
    enum class FileType {
        Audio,
        Image,
        Video,
    };

    struct MimeType {
        bool operator==(MimeType const&) const = default;
        Utf16String value;
    };

    struct Extension {
        bool operator==(Extension const&) const = default;
        Utf16String value;
    };

    using FilterType = Variant<FileType, MimeType, Extension>;

    void add_filter(FilterType);

    Vector<FilterType> filters;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::FileFilter::MimeType const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::FileFilter::MimeType> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::FileFilter::Extension const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::FileFilter::Extension> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::FileFilter const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::FileFilter> decode(Decoder&);

}
