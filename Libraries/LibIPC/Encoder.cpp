/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NumericLimits.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/DateTime.h>
#include <LibCore/Proxy.h>
#include <LibCore/System.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>

namespace IPC {

ErrorOr<void> Encoder::encode_size(size_t size)
{
    if (static_cast<u64>(size) > static_cast<u64>(NumericLimits<u32>::max()))
        return Error::from_string_literal("Container exceeds the maximum allowed size");
    return encode(static_cast<u32>(size));
}

template<>
ErrorOr<void> encode(Encoder& encoder, float const& value)
{
    return encoder.encode(bit_cast<u32>(value));
}

template<>
ErrorOr<void> encode(Encoder& encoder, double const& value)
{
    return encoder.encode(bit_cast<u64>(value));
}

template<>
ErrorOr<void> encode(Encoder& encoder, String const& value)
{
    auto bytes = value.bytes();
    TRY(encoder.encode_size(bytes.size()));
    TRY(encoder.append(bytes.data(), bytes.size()));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, StringView const& value)
{
    if (value.is_null())
        return encoder.encode(NumericLimits<u32>::max());

    TRY(encoder.encode_size(value.length()));
    TRY(encoder.append(reinterpret_cast<u8 const*>(value.characters_without_null_termination()), value.length()));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, ByteString const& value)
{
    return encoder.encode(value.view());
}

template<>
ErrorOr<void> encode(Encoder& encoder, ByteBuffer const& value)
{
    TRY(encoder.encode_size(value.size()));
    TRY(encoder.append(value.data(), value.size()));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, JsonValue const& value)
{
    return encoder.encode(value.serialized());
}

template<>
ErrorOr<void> encode(Encoder& encoder, AK::Duration const& value)
{
    return encoder.encode(value.to_nanoseconds());
}

template<>
ErrorOr<void> encode(Encoder& encoder, UnixDateTime const& value)
{
    return encoder.encode(value.nanoseconds_since_epoch());
}

template<>
ErrorOr<void> encode(Encoder& encoder, URL::URL const& value)
{
    TRY(encoder.encode(value.serialize()));

    if (!value.blob_url_entry().has_value())
        return encoder.encode(false);

    TRY(encoder.encode(true));

    auto const& blob = value.blob_url_entry().value();

    TRY(encoder.encode(blob.object.type));
    TRY(encoder.encode(blob.object.data));
    TRY(encoder.encode(blob.environment.origin));

    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, URL::Origin const& origin)
{
    if (origin.is_opaque()) {
        TRY(encoder.encode(true));
    } else {
        TRY(encoder.encode(false));
        TRY(encoder.encode(origin.scheme()));
        TRY(encoder.encode(origin.host()));
        TRY(encoder.encode(origin.port()));
    }

    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, URL::Host const& host)
{
    TRY(encoder.encode(host.value()));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, File const& file)
{
    int fd = file.take_fd();

    TRY(encoder.append_file_descriptor(fd));
    return {};
}

template<>
ErrorOr<void> encode(Encoder&, Empty const&)
{
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, Core::AnonymousBuffer const& buffer)
{
    TRY(encoder.encode(buffer.is_valid()));

    if (buffer.is_valid()) {
        TRY(encoder.encode_size(buffer.size()));
        TRY(encoder.encode(TRY(IPC::File::clone_fd(buffer.fd()))));
    }

    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, Core::DateTime const& datetime)
{
    return encoder.encode(static_cast<i64>(datetime.timestamp()));
}

template<>
ErrorOr<void> encode(Encoder& encoder, Core::ProxyData const& proxy)
{
    TRY(encoder.encode(proxy.type));
    TRY(encoder.encode(proxy.host_ipv4));
    TRY(encoder.encode(proxy.port));
    return {};
}

}
