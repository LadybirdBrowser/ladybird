/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibCore/Promise.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Adapter;

class WEBGPUNATIVE_API Instance {
public:
    friend class Adapter;

    explicit Instance();
    Instance(Instance&&) noexcept;
    Instance& operator=(Instance&&) noexcept;
    ~Instance();

    ErrorOr<void> initialize();

    Adapter adapter() const;

    NonnullRefPtr<Core::Promise<Adapter>> request_adapter();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
