/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_DIRECTX

#    include <AK/Error.h>
#    include <AK/NonnullOwnPtr.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefCounted.h>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct IDXGIAdapter1;

namespace Gfx {

class Direct3DContext final : public RefCounted<Direct3DContext> {
public:
    ~Direct3DContext();

    IDXGIAdapter1* adapter() const;
    ID3D12Device* device() const;
    ID3D12CommandQueue* queue() const;

private:
    struct Impl;

    explicit Direct3DContext(NonnullOwnPtr<Impl>);

    NonnullOwnPtr<Impl> m_impl;

    friend ErrorOr<NonnullRefPtr<Direct3DContext>> create_direct3d_context();
};

ErrorOr<NonnullRefPtr<Direct3DContext>> create_direct3d_context();

}

#endif
