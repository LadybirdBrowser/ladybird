/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>

namespace IPC {

class ReceivedMessageBytes {
public:
    ReceivedMessageBytes();
    ReceivedMessageBytes(ReceivedMessageBytes const&);
    ReceivedMessageBytes(ReceivedMessageBytes&&);
    ~ReceivedMessageBytes();

    ReceivedMessageBytes& operator=(ReceivedMessageBytes const&);
    ReceivedMessageBytes& operator=(ReceivedMessageBytes&&);

    static ReceivedMessageBytes from_vector(Vector<u8>);
    // Takes over storage that something outside AK owns, such as the Rust transport, which release() gives back.
    static ReceivedMessageBytes adopt_foreign_storage(ReadonlyBytes, void* owner, void (*release)(void* owner));

    ReadonlyBytes bytes() const;
    bool is_empty() const { return bytes().is_empty(); }

private:
    class Impl;

    explicit ReceivedMessageBytes(NonnullRefPtr<Impl>);

    RefPtr<Impl> m_impl;
};

}
