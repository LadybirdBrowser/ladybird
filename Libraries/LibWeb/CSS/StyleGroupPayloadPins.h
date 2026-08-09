/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>

namespace Web::CSS {

class StyleGroupPayloadPins {
public:
    StyleGroupPayloadPins() = default;
    StyleGroupPayloadPins(StyleGroupPayloadPins const&) = delete;
    StyleGroupPayloadPins& operator=(StyleGroupPayloadPins const&) = delete;
    StyleGroupPayloadPins(StyleGroupPayloadPins&& other)
        : m_payloads(move(other.m_payloads))
    {
    }
    StyleGroupPayloadPins& operator=(StyleGroupPayloadPins&& other)
    {
        if (this == &other)
            return *this;
        clear();
        m_payloads = move(other.m_payloads);
        return *this;
    }

    ~StyleGroupPayloadPins();

    void set(ReadonlySpan<void const*> payloads);

private:
    void clear();

    Vector<void const*, 8> m_payloads;
};

}
