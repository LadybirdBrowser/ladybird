/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <AK/NumericLimits.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/DateTime.h>
#include <LibCore/Proxy.h>
#include <LibCore/Socket.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/File.h>
#include <LibURL/URL.h>

namespace IPC {

ErrorOr<size_t> Decoder::decode_size()
{
    return static_cast<size_t>(TRY(decode<u32>()));
}

template<>
ErrorOr<String> decode(Decoder& decoder)
{
    auto length = TRY(decoder.decode_size());
    return String::from_stream(decoder.stream(), length);
}

template<>
ErrorOr<ByteString> decode(Decoder& decoder)
{
    auto length = TRY(decoder.decode_size());
    if (length == 0)
        return ByteString::empty();

    return ByteString::create_and_overwrite(length, [&](Bytes bytes) -> ErrorOr<void> {
        TRY(decoder.decode_into(bytes));
        return {};
    });
}

template<>
ErrorOr<ByteBuffer> decode(Decoder& decoder)
{
    auto length = TRY(decoder.decode_size());
    if (length == 0)
        return ByteBuffer {};

    auto buffer = TRY(ByteBuffer::create_uninitialized(length));
    auto bytes = buffer.bytes();

    TRY(decoder.decode_into(bytes));
    return buffer;
}

template<>
ErrorOr<JsonValue> decode(Decoder& decoder)
{
    auto json = TRY(decoder.decode<ByteString>());
    return JsonValue::from_string(json);
}

template<>
ErrorOr<AK::Duration> decode(Decoder& decoder)
{
    auto nanoseconds = TRY(decoder.decode<i64>());
    return AK::Duration::from_nanoseconds(nanoseconds);
}

template<>
ErrorOr<UnixDateTime> decode(Decoder& decoder)
{
    auto nanoseconds = TRY(decoder.decode<i64>());
    return AK::UnixDateTime::from_nanoseconds_since_epoch(nanoseconds);
}

template<>
ErrorOr<URL::URL> decode(Decoder& decoder)
{
    auto url_string = TRY(decoder.decode<ByteString>());
    URL::URL url { url_string };

    bool has_blob_url = TRY(decoder.decode<bool>());
    if (!has_blob_url)
        return url;

    url.set_blob_url_entry(URL::BlobURLEntry {
        .object = URL::BlobURLEntry::Object {
            .type = TRY(decoder.decode<String>()),
            .data = TRY(decoder.decode<ByteBuffer>()),
        },
        .environment { .origin = TRY(decoder.decode<URL::Origin>()) },
    });

    return url;
}

template<>
ErrorOr<URL::Origin> decode(Decoder& decoder)
{
    auto is_opaque = TRY(decoder.decode<bool>());
    if (is_opaque)
        return URL::Origin {};

    auto scheme = TRY(decoder.decode<Optional<String>>());
    auto host = TRY(decoder.decode<URL::Host>());
    auto port = TRY(decoder.decode<Optional<u16>>());

    return URL::Origin { move(scheme), move(host), port };
}

template<>
ErrorOr<URL::Host> decode(Decoder& decoder)
{
    auto value = TRY(decoder.decode<URL::Host::VariantType>());
    return URL::Host { move(value) };
}

template<>
ErrorOr<Empty> decode(Decoder&)
{
    return Empty {};
}

template<>
ErrorOr<Core::AnonymousBuffer> decode(Decoder& decoder)
{
    if (auto valid = TRY(decoder.decode<bool>()); !valid)
        return Core::AnonymousBuffer {};

    auto size = TRY(decoder.decode_size());
    auto anon_file = TRY(decoder.decode<IPC::File>());

    return Core::AnonymousBuffer::create_from_anon_fd(anon_file.take_fd(), size);
}

template<>
ErrorOr<Core::DateTime> decode(Decoder& decoder)
{
    auto timestamp = TRY(decoder.decode<i64>());
    return Core::DateTime::from_timestamp(static_cast<time_t>(timestamp));
}

template<>
ErrorOr<Core::ProxyData> decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<Core::ProxyData::Type>());
    auto host_ipv4 = TRY(decoder.decode<u32>());
    auto port = TRY(decoder.decode<int>());

    return Core::ProxyData { type, host_ipv4, port };
}

}
