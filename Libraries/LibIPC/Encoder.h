/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/HashMap.h>
#include <AK/StdLibExtras.h>
#include <AK/Variant.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibIPC/Concepts.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Message.h>
#include <LibURL/Forward.h>

namespace IPC {

template<typename T>
ErrorOr<void> encode(Encoder&, T const&)
{
    static_assert(DependentFalse<T>, "Base IPC::encode() was instantiated");
    VERIFY_NOT_REACHED();
}

class Encoder {
public:
    explicit Encoder(MessageBuffer& buffer)
        : m_buffer(buffer)
    {
    }

    template<typename T>
    ErrorOr<void> encode(T const& value);

    ErrorOr<void> extend_capacity(size_t capacity)
    {
        TRY(m_buffer.extend_data_capacity(capacity));
        return {};
    }

    ErrorOr<void> append(u8 const* values, size_t count)
    {
        TRY(m_buffer.append_data(values, count));
        return {};
    }

    ErrorOr<void> append_file_descriptor(int fd)
    {
        TRY(m_buffer.append_file_descriptor(fd));
        return {};
    }

    ErrorOr<void> encode_size(size_t size);

private:
    MessageBuffer& m_buffer;
};

template<Arithmetic T>
ErrorOr<void> encode(Encoder& encoder, T const& value)
{
    TRY(encoder.append(reinterpret_cast<u8 const*>(&value), sizeof(value)));
    return {};
}

template<Enum T>
ErrorOr<void> encode(Encoder& encoder, T const& value)
{
    return encoder.encode(to_underlying(value));
}

template<>
ErrorOr<void> encode(Encoder&, float const&);

template<>
ErrorOr<void> encode(Encoder&, double const&);

template<>
ErrorOr<void> encode(Encoder&, String const&);

template<>
ErrorOr<void> encode(Encoder&, StringView const&);

template<>
ErrorOr<void> encode(Encoder&, ByteString const&);

template<>
ErrorOr<void> encode(Encoder&, ByteBuffer const&);

template<>
ErrorOr<void> encode(Encoder&, JsonValue const&);

template<>
ErrorOr<void> encode(Encoder&, AK::Duration const&);

template<>
ErrorOr<void> encode(Encoder&, UnixDateTime const&);

template<>
ErrorOr<void> encode(Encoder&, URL::URL const&);

template<>
ErrorOr<void> encode(Encoder&, URL::Origin const&);

template<>
ErrorOr<void> encode(Encoder&, URL::Host const&);

template<>
ErrorOr<void> encode(Encoder&, File const&);

template<>
ErrorOr<void> encode(Encoder&, Empty const&);

template<Concepts::Span T>
ErrorOr<void> encode(Encoder& encoder, T const& span)
{
    TRY(encoder.encode_size(span.size()));

    for (auto const& value : span)
        TRY(encoder.encode(value));

    return {};
}

template<typename T, size_t N>
ErrorOr<void> encode(Encoder& encoder, Array<T, N> const& array)
{
    return encoder.encode(array.span());
}

template<Concepts::Vector T>
ErrorOr<void> encode(Encoder& encoder, T const& vector)
{
    return encoder.encode(vector.span());
}

template<Concepts::HashMap T>
ErrorOr<void> encode(Encoder& encoder, T const& hashmap)
{
    TRY(encoder.encode_size(hashmap.size()));

    for (auto it : hashmap) {
        TRY(encoder.encode(it.key));
        TRY(encoder.encode(it.value));
    }

    return {};
}

template<Concepts::SharedSingleProducerCircularQueue T>
ErrorOr<void> encode(Encoder& encoder, T const& queue)
{
    TRY(encoder.encode(TRY(IPC::File::clone_fd(queue.fd()))));
    return {};
}

template<Concepts::Optional T>
ErrorOr<void> encode(Encoder& encoder, T const& optional)
{
    TRY(encoder.encode(optional.has_value()));

    if (optional.has_value())
        TRY(encoder.encode(optional.value()));

    return {};
}

template<Concepts::Variant T>
ErrorOr<void> encode(Encoder& encoder, T const& variant)
{
    TRY(encoder.encode(variant.index()));

    return variant.visit([&](auto const& value) {
        return encoder.encode(value);
    });
}

// This must be last so that it knows about the above specializations.
template<typename T>
ErrorOr<void> Encoder::encode(T const& value)
{
    return IPC::encode(*this, value);
}

}
