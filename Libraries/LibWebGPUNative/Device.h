/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Adapter;
class CommandEncoder;
class Queue;

class WEBGPUNATIVE_API Device {
public:
    friend class CommandEncoder;
    friend class Queue;

    explicit Device(Adapter const&);
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    ~Device();

    ErrorOr<void> initialize();

    Queue queue() const;

    CommandEncoder command_encoder() const;

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
