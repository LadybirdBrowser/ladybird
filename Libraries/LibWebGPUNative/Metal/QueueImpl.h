/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebGPUNative/Queue.h>
#include <objc/objc.h>

namespace WebGPUNative {

struct Queue::Impl {
    explicit Impl(Device const& gpu_device);
    ~Impl() = default;

    id command_queue() const { return m_command_queue; }

private:
    id m_command_queue;
};

}
