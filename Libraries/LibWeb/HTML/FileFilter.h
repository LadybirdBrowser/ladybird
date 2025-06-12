/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>

namespace Web::HTML {

struct FileFilter {
    enum class FileType {
        Audio,
        Image,
        Video,
    };

    struct MimeType {
        bool operator==(MimeType const&) const = default;
        String value;
    };

    struct Extension {
        bool operator==(Extension const&) const = default;
        String value;
    };

    using FilterType = Variant<FileType, MimeType, Extension>;

    void add_filter(FilterType);

    Vector<FilterType> filters;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::FileFilter::MimeType const&);

template<>
ErrorOr<Web::HTML::FileFilter::MimeType> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::FileFilter::Extension const&);

template<>
ErrorOr<Web::HTML::FileFilter::Extension> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::FileFilter const&);

template<>
ErrorOr<Web::HTML::FileFilter> decode(Decoder&);

}
