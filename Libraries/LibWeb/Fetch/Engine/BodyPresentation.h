/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Variant.h>
#include <LibCore/ImmutableBytes.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>

namespace Web::Fetch::Engine {

struct NullBody {
    bool operator==(NullBody const&) const = default;
};

inline bool is_present_body_handle(Core::ImmutableBytes const& bytes)
{
    return bytes.is_valid();
}

template<typename T>
bool is_present_body_handle(NonnullRefPtr<T> const&)
{
    return true;
}

template<typename... Ts>
bool is_present_body_handle(Variant<Ts...> const& handle)
{
    return handle.visit([](auto const& arm) { return is_present_body_handle(arm); });
}

// A delivered body is never null.
template<typename Handle>
class Delivered {
public:
    explicit Delivered(Handle handle)
        : m_handle(move(handle))
    {
        VERIFY(is_present_body_handle(m_handle));
    }

    Handle const& handle() const { return m_handle; }

private:
    Handle m_handle;
};

using NonNullLocalBodyHandle = Variant<Core::ImmutableBytes, NonnullRefPtr<FetchByteChannel>>;

template<typename Handle>
using BodyPresentation = Variant<NullBody, Delivered<Handle>>;

using LocalBodyPresentation = BodyPresentation<NonNullLocalBodyHandle>;

}
