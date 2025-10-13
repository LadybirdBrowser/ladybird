/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>

#if !defined(AK_OS_WINDOWS)
static_assert(false, "This file must only be used for Windows");
#endif

// NOTE: We can't include the header that defines ComPtr as it transitively includes other headers that define conflicting macros
struct IDXGIAdapter1;
struct ID3D11Device;
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace Gfx {

class Direct3DContext {
public:
    ~Direct3DContext();

    static ErrorOr<NonnullOwnPtr<Direct3DContext>> try_create();

    IDXGIAdapter1& adapter() const;

    ID3D12Device& d12_device() const;
    ID3D12CommandQueue& d12_command_queue() const;

    ID3D11Device& d11_device() const;

private:
    Direct3DContext();

    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
