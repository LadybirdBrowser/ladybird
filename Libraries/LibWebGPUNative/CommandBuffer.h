/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class CommandEncoder;
class Queue;

class WEBGPUNATIVE_API CommandBuffer {
public:
    friend class Queue;

    explicit CommandBuffer(CommandEncoder const&);
    CommandBuffer(CommandBuffer&&) noexcept;
    CommandBuffer& operator=(CommandBuffer&&) noexcept;
    ~CommandBuffer();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
