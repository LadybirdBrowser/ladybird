/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_DIRECTX

#    include <AK/Error.h>
#    include <AK/NonnullOwnPtr.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/Optional.h>
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
    u64 adapter_luid() const;

private:
    struct Impl;

    explicit Direct3DContext(NonnullOwnPtr<Impl>);

    NonnullOwnPtr<Impl> m_impl;

    friend ErrorOr<NonnullRefPtr<Direct3DContext>> create_direct3d_context();
};

ErrorOr<NonnullRefPtr<Direct3DContext>> create_direct3d_context();

// LUID of the adapter that Qt's D3D11 backend will pick in the UI process (the first enumerated
// adapter unless overridden by QT_D3D_ADAPTER_INDEX). Used to check that a texture shared by the
// Compositor process can be opened by the UI process.
Optional<u64> default_dxgi_adapter_luid();

}

#endif
