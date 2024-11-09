/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>

namespace WebSocket {

class Message {
public:
    explicit Message(ByteString const& data)
        : m_is_text(true)
        , m_data(ByteBuffer::copy(data.bytes()).release_value_but_fixme_should_propagate_errors()) // FIXME: Handle possible OOM situation.
    {
    }

    explicit Message(ByteBuffer data, bool is_text)
        : m_is_text(is_text)
        , m_data(move(data))
    {
    }

    explicit Message(ByteBuffer const&& data, bool is_text)
        : m_is_text(is_text)
        , m_data(move(data))
    {
    }

    bool is_text() const { return m_is_text; }
    ByteBuffer const& data() const { return m_data; }

private:
    bool m_is_text { false };
    ByteBuffer m_data;
};

}
