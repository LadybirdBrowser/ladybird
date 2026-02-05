/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/FileFilter.h>

namespace Web::HTML {

void FileFilter::add_filter(FilterType filter)
{
    if (!filters.contains_slow(filter))
        filters.append(move(filter));
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::FileFilter::MimeType const& mime_type)
{
    TRY(encoder.encode(mime_type.value));
    return {};
}

template<>
ErrorOr<Web::HTML::FileFilter::MimeType> IPC::decode(Decoder& decoder)
{
    auto value = TRY(decoder.decode<String>());
    return Web::HTML::FileFilter::MimeType { move(value) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::FileFilter::Extension const& extension)
{
    TRY(encoder.encode(extension.value));
    return {};
}

template<>
ErrorOr<Web::HTML::FileFilter::Extension> IPC::decode(Decoder& decoder)
{
    auto value = TRY(decoder.decode<String>());
    return Web::HTML::FileFilter::Extension { move(value) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::FileFilter const& filter)
{
    TRY(encoder.encode(filter.filters));
    return {};
}

template<>
ErrorOr<Web::HTML::FileFilter> IPC::decode(Decoder& decoder)
{
    auto filters = TRY(decoder.decode<Vector<Web::HTML::FileFilter::FilterType>>());
    return Web::HTML::FileFilter { move(filters) };
}
