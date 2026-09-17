/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibCore/ImmutableBytes.h>

namespace Web::Fetch::Engine {

struct NullBody {
    bool operator==(NullBody const&) const = default;
};

// A delivered body is never null.
template<typename Handle>
class Delivered {
public:
    explicit Delivered(Handle handle)
        : m_handle(move(handle))
    {
        VERIFY(m_handle.is_valid());
    }

    Handle const& handle() const { return m_handle; }

private:
    Handle m_handle;
};

using NonNullLocalBodyHandle = Core::ImmutableBytes;

template<typename Handle>
using BodyPresentation = Variant<NullBody, Delivered<Handle>>;

using LocalBodyPresentation = BodyPresentation<NonNullLocalBodyHandle>;

}
