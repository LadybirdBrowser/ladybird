/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibWebView/Application.h>
#include <UI/Qt/WebContentView.h>

#include <QColor>
#include <QImage>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <AK/Windows.h>
#include <d3d11_1.h>

#include <wrl/client.h>

namespace Ladybird {

using Microsoft::WRL::ComPtr;

// Front, back, and backup backing stores can be alive at the same time; anything above that means
// the cached buffer pointers are stale (e.g. after a reallocation), so the cache is reset.
static constexpr size_t max_cached_imported_d3d_textures = 4;

void WebContentView::release_imported_d3d_textures()
{
    for (auto& texture : m_imported_d3d_textures) {
        // The QRhiTexture wraps the D3D texture without owning it, so it has to go first.
        delete texture.qrhi_texture;
        if (texture.d3d11_texture)
            texture.d3d11_texture->Release();
    }
    m_imported_d3d_textures.clear();
}

QRhiTexture* WebContentView::imported_d3d_texture_for(Gfx::SharedImageBuffer const& shared_image_buffer, Gfx::WindowsD3DHandle const& d3d_handle)
{
    for (auto& texture : m_imported_d3d_textures) {
        if (texture.shared_image_buffer == &shared_image_buffer && texture.handle_value == d3d_handle.file.fd())
            return texture.qrhi_texture;
    }

    if (!rhi() || rhi()->backend() != QRhi::D3D11)
        return nullptr;

    auto const* rhi_native_handles = static_cast<QRhiD3D11NativeHandles const*>(rhi()->nativeHandles());
    if (!rhi_native_handles || !rhi_native_handles->dev)
        return nullptr;

    ComPtr<ID3D11Device1> device;
    if (FAILED(static_cast<ID3D11Device*>(rhi_native_handles->dev)->QueryInterface(IID_PPV_ARGS(&device))))
        return nullptr;

    if (m_imported_d3d_textures.size() >= max_cached_imported_d3d_textures)
        release_imported_d3d_textures();

    ID3D11Texture2D* texture = nullptr;
    QRhiTexture* qrhi_texture = nullptr;
    auto hr = device->OpenSharedResource1(to_handle(d3d_handle.file.fd()), IID_PPV_ARGS(&texture));
    if (SUCCEEDED(hr)) {
        qrhi_texture = rhi()->newTexture(QRhiTexture::RGBA8, QSize(d3d_handle.size.width(), d3d_handle.size.height()), 1, QRhiTexture::UsedAsTransferSource);
        if (qrhi_texture && !qrhi_texture->createFrom({ reinterpret_cast<quint64>(texture), 0 })) {
            delete qrhi_texture;
            qrhi_texture = nullptr;
        }
        if (!qrhi_texture) {
            texture->Release();
            texture = nullptr;
        }
    } else {
        dbgln("Failed to open shared Direct3D texture in the UI process: {}", Error::from_windows_error(hr));
    }

    // Failed imports are cached as well (with a null texture) so they are not retried every frame
    // while the Compositor downgrades to CPU-shared backing stores.
    m_imported_d3d_textures.append({ &shared_image_buffer, d3d_handle.file.fd(), texture, qrhi_texture });
    return qrhi_texture;
}

void WebContentView::initialize(QRhiCommandBuffer*)
{
    release_imported_d3d_textures();
}

void WebContentView::releaseResources()
{
    release_imported_d3d_textures();
}

void WebContentView::render(QRhiCommandBuffer* command_buffer)
{
    auto background_color = page_background_color();
    auto clear_color = QColor(background_color.red(), background_color.green(), background_color.blue());

    // The clear covers the part of the render target that the (possibly padded or stale-sized)
    // backing store does not.
    command_buffer->beginPass(renderTarget(), clear_color, { 1.0f, 0 });
    command_buffer->endPass();

    auto paintable = current_paintable();
    if (!paintable.has_value() || paintable->bitmap_size.is_empty() || !rhi() || !colorTexture())
        return;

    auto target_size = colorTexture()->pixelSize();
    auto content_width = min(paintable->bitmap_size.width(), target_size.width());
    auto content_height = min(paintable->bitmap_size.height(), target_size.height());
    if (content_width <= 0 || content_height <= 0)
        return;

    if (auto const* d3d_handle = paintable->shared_image_buffer->windows_d3d_handle()) {
        auto* imported_texture = imported_d3d_texture_for(*paintable->shared_image_buffer, *d3d_handle);
        if (!imported_texture) {
            WebView::Application::the().notify_compositor_gpu_presentation_unavailable();
            return;
        }

        QRhiTextureCopyDescription copy_description;
        copy_description.setPixelSize(QSize(
            min(content_width, d3d_handle->size.width()),
            min(content_height, d3d_handle->size.height())));

        auto* batch = rhi()->nextResourceUpdateBatch();
        batch->copyTexture(colorTexture(), imported_texture, copy_description);
        command_buffer->resourceUpdate(batch);
        m_force_full_repaint = false;
        m_has_pending_frame_damage = false;
        m_pending_frame_damage = {};
        return;
    }

    // CPU-shared backing store (e.g. --force-cpu-painting, or the Compositor could not share GPU
    // textures): upload the visible part of the bitmap into the render target.
    auto bitmap = paintable->shared_image_buffer->bitmap_if_present();
    if (!bitmap)
        return;

    auto image = QImage(bitmap->scanline_u8(0), min(content_width, bitmap->width()), min(content_height, bitmap->height()), bitmap->pitch(), QImage::Format_RGB32)
                     .convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        return;

    QRhiTextureSubresourceUploadDescription subresource_description(image);
    subresource_description.setSourceSize(image.size());

    auto* batch = rhi()->nextResourceUpdateBatch();
    batch->uploadTexture(colorTexture(), QRhiTextureUploadDescription({ 0, 0, subresource_description }));
    command_buffer->resourceUpdate(batch);
    m_force_full_repaint = false;
    m_has_pending_frame_damage = false;
    m_pending_frame_damage = {};
}

}
