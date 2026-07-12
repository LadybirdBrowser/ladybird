/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_DIRECTX

#    include <AK/AtomicRefCounted.h>
#    include <AK/Error.h>
#    include <AK/NonnullRefPtr.h>
#    include <LibGfx/Size.h>
#    include <LibIPC/File.h>

struct ID3D12Resource;

namespace Gfx {

class Direct3DContext;

// A DXGI_FORMAT_R8G8B8A8_UNORM render-target texture allocated on a shared heap so its NT handle
// can be duplicated into another process and opened there (e.g. by the UI process via
// ID3D11Device1::OpenSharedResource1).
class D3DSharedTexture final : public AtomicRefCounted<D3DSharedTexture> {
public:
    static ErrorOr<NonnullRefPtr<D3DSharedTexture>> create(Direct3DContext&, IntSize);
    ~D3DSharedTexture();

    ID3D12Resource* resource() const { return m_resource; }
    IntSize size() const { return m_size; }

    ErrorOr<IPC::File> clone_handle() const { return IPC::File::clone_fd(m_shared_handle.fd()); }

private:
    D3DSharedTexture(ID3D12Resource*, IPC::File shared_handle, IntSize);

    ID3D12Resource* m_resource { nullptr };
    IPC::File m_shared_handle;
    IntSize m_size;
};

}

#endif
