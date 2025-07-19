/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibWebGPUNative/Queue.h>
#include <objc/objc.h>

namespace WebGPUNative {

struct Queue::Impl {
    explicit Impl(Device const& gpu_device);
    ~Impl() = default;

    id command_queue() const { return m_command_queue; }

    ErrorOr<void> submit(Vector<NonnullRawPtr<CommandBuffer>> const& gpu_command_buffers);
    void on_submitted(Function<void()> callback);

private:
    id m_command_queue;
    Function<void()> m_submitted_callback;
};

}
