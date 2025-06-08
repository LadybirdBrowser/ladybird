/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRawPtr.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Device;
class CommandBuffer;

class WEBGPUNATIVE_API Queue {
public:
    explicit Queue(Device const&);
    Queue(Queue&&) noexcept;
    Queue& operator=(Queue&&) noexcept;
    ~Queue();

    ErrorOr<void> submit(Vector<NonnullRawPtr<CommandBuffer>> const&);
    void on_submitted(Function<void()> callback);

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
