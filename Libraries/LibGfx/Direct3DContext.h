/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>
#include <AK/RefCounted.h>

#if !defined(AK_OS_WINDOWS)
static_assert(false, "This file must only be used for Windows");
#endif

#include <dxgiformat.h>
// NOTE: We can't include the header that defines ComPtr as it transitively includes other headers that define conflicting macros

struct IDXGIAdapter1;
struct ID3D12Device;
struct ID3D12CommandQueue;

struct ID3D11Device;
struct IDXGIResource;

namespace Gfx {

class Direct3DContext;

class Direct3D11Texture : public RefCounted<Direct3D11Texture> {
public:
    ~Direct3D11Texture();

    static ErrorOr<NonnullRefPtr<Direct3D11Texture>> try_create_shared(Direct3DContext const& context, u32 width, u32 height, DXGI_FORMAT format);

    IDXGIResource& get_resource() const;

private:
    Direct3D11Texture(Direct3DContext const& context);

    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

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
