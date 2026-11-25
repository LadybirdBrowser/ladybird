/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <Compositor/DirectCompositionWindowServer.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibCompositing/FrameFitting.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/D3DSharedTexture.h>
#include <LibGfx/Direct3DContext.h>
#include <LibGfx/Rect.h>
#include <LibThreading/Thread.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkPaint.h>
#include <core/SkRect.h>
#include <core/SkSurface.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>

#include <AK/Windows.h>
#include <gpu/ganesh/d3d/GrD3DBackendSurface.h>
#include <gpu/ganesh/d3d/GrD3DTypes.h>

#include <d3d11_3.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_4.h>

#include <wrl/client.h>

namespace Compositor {

using Microsoft::WRL::ComPtr;

static constexpr UINT create_child_window_message = WM_APP + 1;
static constexpr UINT destroy_child_window_message = WM_APP + 2;
static constexpr wchar_t parking_window_class_name[] = L"LadybirdCompositorParkingWindow";
static constexpr wchar_t child_window_class_name[] = L"LadybirdCompositorWindow";

class ChildWindowThread {
public:
    static ChildWindowThread* the();

    HWND create_child_window() const { return reinterpret_cast<HWND>(SendMessageW(m_parking_window, create_child_window_message, 0, 0)); }
    void destroy_child_window(HWND window) const { SendMessageW(m_parking_window, destroy_child_window_message, reinterpret_cast<WPARAM>(window), 0); }

private:
    ChildWindowThread() = default;

    bool start();
    void run();

    static LRESULT CALLBACK parking_window_procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK child_window_procedure(HWND, UINT, WPARAM, LPARAM);

    RefPtr<Threading::Thread> m_thread;
    HANDLE m_started_event { nullptr };
    HWND m_parking_window { nullptr };
};

ChildWindowThread* ChildWindowThread::the()
{
    static ChildWindowThread* s_the = []() -> ChildWindowThread* {
        auto* thread = new ChildWindowThread;
        if (!thread->start()) {
            dbgln("Compositor: Unable to start the thread of the windows of remote layers");
            return nullptr;
        }
        return thread;
    }();
    return s_the;
}

bool ChildWindowThread::start()
{
    m_started_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_started_event)
        return false;
    m_thread = Threading::Thread::construct("RemoteLayerWindows"sv, [this] {
        run();
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
    m_thread->detach();
    WaitForSingleObject(m_started_event, INFINITE);
    CloseHandle(m_started_event);
    m_started_event = nullptr;
    return m_parking_window != nullptr;
}

void ChildWindowThread::run()
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW parking_window_class {};
    parking_window_class.cbSize = sizeof(WNDCLASSEXW);
    parking_window_class.lpfnWndProc = parking_window_procedure;
    parking_window_class.hInstance = instance;
    parking_window_class.lpszClassName = parking_window_class_name;
    WNDCLASSEXW child_window_class {};
    child_window_class.cbSize = sizeof(WNDCLASSEXW);
    child_window_class.style = CS_OWNDC;
    child_window_class.lpfnWndProc = child_window_procedure;
    child_window_class.hInstance = instance;
    child_window_class.lpszClassName = child_window_class_name;
    if (RegisterClassExW(&parking_window_class) && RegisterClassExW(&child_window_class)) {
        m_parking_window = CreateWindowExW(WS_EX_TOOLWINDOW, parking_window_class_name, L"", WS_POPUP | WS_DISABLED, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    }
    SetEvent(m_started_event);
    if (!m_parking_window)
        return;

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

LRESULT CALLBACK ChildWindowThread::parking_window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message) {
    case create_child_window_message: {
        auto child_window = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_NOREDIRECTIONBITMAP, child_window_class_name, L"",
            WS_CHILDWINDOW | WS_DISABLED | WS_VISIBLE, 0, 0, 1, 1, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!child_window)
            dbgln("Compositor: Unable to create the window of a remote layer: error {}", GetLastError());
        return reinterpret_cast<LRESULT>(child_window);
    }
    case destroy_child_window_message:
        DestroyWindow(reinterpret_cast<HWND>(w_param));
        return 0;
    case WM_CLOSE:
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

LRESULT CALLBACK ChildWindowThread::child_window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message) {
    case WM_CLOSE:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

class CompositionSwapChain {
    AK_MAKE_NONCOPYABLE(CompositionSwapChain);
    AK_MAKE_NONMOVABLE(CompositionSwapChain);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<CompositionSwapChain>> create(Gfx::SkiaBackendContext&, IDXGIFactory2&, Gfx::IntSize, DXGI_ALPHA_MODE);

    IUnknown* content() const { return m_swap_chain.Get(); }

    ErrorOr<void> draw(Gfx::PaintingSurface const&, Gfx::IntSize);
    void show_drawing();

private:
    static constexpr UINT buffer_count = 3;

    CompositionSwapChain(Gfx::SkiaBackendContext& skia_backend_context, ComPtr<IDXGISwapChain3> swap_chain, Gfx::IntSize buffer_size)
        : m_skia_backend_context(skia_backend_context)
        , m_swap_chain(move(swap_chain))
        , m_buffer_size(buffer_size)
    {
    }

    ErrorOr<void> make_buffers_fit(Gfx::IntSize);
    ErrorOr<sk_sp<SkSurface>> surface_of_buffer(UINT index);

    NonnullRefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    ComPtr<IDXGISwapChain3> m_swap_chain;
    Gfx::IntSize m_buffer_size;
    Array<sk_sp<SkSurface>, buffer_count> m_buffer_surfaces;
    bool m_has_drawing_to_show { false };
};

ErrorOr<NonnullOwnPtr<CompositionSwapChain>> CompositionSwapChain::create(Gfx::SkiaBackendContext& skia_backend_context, IDXGIFactory2& factory, Gfx::IntSize size, DXGI_ALPHA_MODE alpha_mode)
{
    DXGI_SWAP_CHAIN_DESC1 description {};
    description.Width = size.width();
    description.Height = size.height();
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = buffer_count;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = alpha_mode;

    ComPtr<IDXGISwapChain1> swap_chain;
    if (auto result = factory.CreateSwapChainForComposition(skia_backend_context.direct3d_context().queue(), &description, nullptr, &swap_chain); FAILED(result))
        return Error::from_windows_error(result);
    ComPtr<IDXGISwapChain3> swap_chain3;
    if (auto result = swap_chain.As(&swap_chain3); FAILED(result))
        return Error::from_windows_error(result);
    return adopt_own(*new CompositionSwapChain(skia_backend_context, move(swap_chain3), size));
}

static constexpr int buffer_growth_step = 256;

static int grown_buffer_extent(int needed, int current)
{
    if (needed <= current)
        return current;
    return (needed + buffer_growth_step - 1) / buffer_growth_step * buffer_growth_step;
}

ErrorOr<void> CompositionSwapChain::make_buffers_fit(Gfx::IntSize size)
{
    auto fits = size.width() <= m_buffer_size.width() && size.height() <= m_buffer_size.height();
    auto is_far_too_large = static_cast<i64>(size.width()) * size.height() * 4 < static_cast<i64>(m_buffer_size.width()) * m_buffer_size.height();
    if (!fits || is_far_too_large) {
        Gfx::IntSize buffer_size = is_far_too_large
            ? size
            : Gfx::IntSize { grown_buffer_extent(size.width(), m_buffer_size.width()), grown_buffer_extent(size.height(), m_buffer_size.height()) };

        for (auto& surface : m_buffer_surfaces)
            surface.reset();
        m_skia_backend_context->sk_context()->flushAndSubmit(GrSyncCpu::kYes);
        if (auto result = m_swap_chain->ResizeBuffers(buffer_count, buffer_size.width(), buffer_size.height(), DXGI_FORMAT_R8G8B8A8_UNORM, 0); FAILED(result))
            return Error::from_windows_error(result);
        m_buffer_size = buffer_size;
        m_has_drawing_to_show = false;
    }
    return {};
}

ErrorOr<sk_sp<SkSurface>> CompositionSwapChain::surface_of_buffer(UINT index)
{
    VERIFY(index < buffer_count);
    if (m_buffer_surfaces[index])
        return m_buffer_surfaces[index];

    ComPtr<ID3D12Resource> buffer;
    if (auto result = m_swap_chain->GetBuffer(index, IID_PPV_ARGS(&buffer)); FAILED(result))
        return Error::from_windows_error(result);

    GrD3DTextureResourceInfo info(nullptr, nullptr, D3D12_RESOURCE_STATE_PRESENT, DXGI_FORMAT_R8G8B8A8_UNORM,
        /* sampleCount */ 1, /* levelCount */ 1, /* sampleQualityLevel */ 0);
    info.fResource.retain(buffer.Get());
    auto render_target = GrBackendRenderTargets::MakeD3D(m_buffer_size.width(), m_buffer_size.height(), info);
    auto surface = SkSurfaces::WrapBackendRenderTarget(m_skia_backend_context->sk_context(), render_target, kTopLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
    if (!surface)
        return Error::from_string_literal("Failed to wrap a buffer of a swap chain in a Skia surface");
    m_buffer_surfaces[index] = surface;
    return surface;
}

ErrorOr<void> CompositionSwapChain::draw(Gfx::PaintingSurface const& source, Gfx::IntSize size)
{
    TRY(make_buffers_fit(size));
    auto target = TRY(surface_of_buffer(m_swap_chain->GetCurrentBackBufferIndex()));

    auto image = source.sk_image_snapshot<sk_sp<SkImage>>();
    if (!image)
        return Error::from_string_literal("Failed to take a snapshot of a surface to show");

    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    auto rect = SkRect::MakeIWH(size.width(), size.height());
    target->getCanvas()->drawImageRect(image.get(), rect, rect, SkSamplingOptions {}, &paint, SkCanvas::kStrict_SrcRectConstraint);

    auto& context = *m_skia_backend_context->sk_context();
    context.flush(target.get(), SkSurfaces::BackendSurfaceAccess::kPresent, GrFlushInfo {});
    context.submit(GrSyncCpu::kNo);
    m_has_drawing_to_show = true;
    return {};
}

void CompositionSwapChain::show_drawing()
{
    if (!exchange(m_has_drawing_to_show, false))
        return;
    if (auto result = m_swap_chain->Present(0, 0); FAILED(result))
        dbgln("Compositor: Failed to present a swap chain of a remote layer: {}", Error::from_windows_error(result));
}

static ComPtr<ID3D11Device1> create_d3d11_device_for_composition_textures()
{
    if (WindowServer::showing_tiles_of_painted_layers_is_switched_off())
        return nullptr;
    auto skia_backend_context = Gfx::SkiaBackendContext::the_main_thread_context();
    if (!skia_backend_context || !skia_backend_context->sk_context() || skia_backend_context->sk_context()->backend() != GrBackendApi::kDirect3D)
        return nullptr;

    // The device opens the textures that the main thread's context paints, so it is created on the same adapter.
    Array feature_levels { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ComPtr<ID3D11Device> device;
    if (auto result = D3D11CreateDevice(skia_backend_context->direct3d_context().adapter(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels.data(), static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &device, nullptr, nullptr);
        FAILED(result)) {
        dbgln("Compositor: Unable to create a Direct3D 11 device for composition textures: {}", Error::from_windows_error(result));
        return nullptr;
    }
    ComPtr<ID3D11Device1> device1;
    if (auto result = device.As(&device1); FAILED(result)) {
        dbgln("Compositor: Unable to reach the Direct3D 11.1 interface of a device: {}", Error::from_windows_error(result));
        return nullptr;
    }

    ComPtr<IDCompositionDesktopDevice> composition_device;
    if (auto result = DCompositionCreateDevice3(nullptr, IID_PPV_ARGS(&composition_device)); FAILED(result)) {
        dbgln("Compositor: Unable to create a DirectComposition device to check for composition textures: {}", Error::from_windows_error(result));
        return nullptr;
    }
    ComPtr<IDCompositionDevice4> composition_device4;
    if (auto result = composition_device.As(&composition_device4); FAILED(result)) {
        dbgln("Compositor: This DirectComposition has no composition textures: {}", Error::from_windows_error(result));
        return nullptr;
    }
    BOOL supports_composition_textures = FALSE;
    if (auto result = composition_device4->CheckCompositionTextureSupport(device.Get(), &supports_composition_textures); FAILED(result)) {
        dbgln("Compositor: Unable to check for composition textures: {}", Error::from_windows_error(result));
        return nullptr;
    }
    if (!supports_composition_textures) {
        dbgln("Compositor: DirectComposition shows no composition textures of this GPU");
        return nullptr;
    }
    return device1;
}

// The Direct3D 11 device that opens the surfaces the window server reads as they are, shared by every window server, or
// null where the window server cannot read surfaces so.
static ID3D11Device1* d3d11_device_for_composition_textures()
{
    static ID3D11Device1* device = create_d3d11_device_for_composition_textures().Detach();
    return device;
}

// A texture of one pixel, which a visual scales up into a fill.
static ErrorOr<ComPtr<IDCompositionTexture>> create_composition_texture_filled_with(IDCompositionDevice4& device, Gfx::Color color)
{
    auto* d3d11_device = d3d11_device_for_composition_textures();
    VERIFY(d3d11_device);

    D3D11_TEXTURE2D_DESC description {};
    description.Width = 1;
    description.Height = 1;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // A texture that cannot be shared is not taken as a composition texture.
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    auto premultiplied = [&](u8 channel) { return static_cast<u8>((channel * color.alpha() + 127) / 255); };
    Array<u8, 4> pixel { premultiplied(color.red()), premultiplied(color.green()), premultiplied(color.blue()), color.alpha() };
    D3D11_SUBRESOURCE_DATA initial_data {};
    initial_data.pSysMem = pixel.data();
    initial_data.SysMemPitch = static_cast<UINT>(pixel.size());
    ComPtr<ID3D11Texture2D> texture;
    if (auto result = d3d11_device->CreateTexture2D(&description, &initial_data, &texture); FAILED(result))
        return Error::from_windows_error(result);
    // Another device sees what this one wrote into a shared texture only once its work is flushed.
    ComPtr<ID3D11DeviceContext> d3d11_context;
    d3d11_device->GetImmediateContext(&d3d11_context);
    d3d11_context->Flush();

    ComPtr<IDCompositionTexture> composition_texture;
    if (auto result = device.CreateCompositionTexture(texture.Get(), &composition_texture); FAILED(result))
        return Error::from_windows_error(result);
    if (!composition_texture)
        return Error::from_string_literal("DirectComposition made no composition texture of a color");
    if (auto result = composition_texture->SetAlphaMode(DXGI_ALPHA_MODE_PREMULTIPLIED); FAILED(result))
        return Error::from_windows_error(result);
    return composition_texture;
}

static constexpr size_t maximum_count_of_composition_textures_filled_with_colors = 256;

struct DirectCompositionWindowServer::Impl {
    AK_ALLOC_WITH_KMALLOC;

    IDCompositionTexture* composition_texture_filled_with(Gfx::Color);

    NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    ComPtr<IDCompositionDesktopDevice> device;
    // Null where the window server cannot show surfaces as they are, which it then shows by copying them into swap chains.
    ComPtr<IDCompositionDevice4> device_showing_composition_textures;
    ComPtr<IDXGIFactory2> dxgi_factory;
    // Tells apart the window servers that the composition textures of a surface belong to, as each belongs to the device
    // that made it.
    u64 id { 0 };
    HashMap<Gfx::Color, ComPtr<IDCompositionTexture>> composition_textures_filled_with_colors;
};

IDCompositionTexture* DirectCompositionWindowServer::Impl::composition_texture_filled_with(Gfx::Color color)
{
    if (!device_showing_composition_textures)
        return nullptr;
    if (auto texture = composition_textures_filled_with_colors.find(color); texture != composition_textures_filled_with_colors.end())
        return texture->value.Get();
    auto texture = create_composition_texture_filled_with(*device_showing_composition_textures.Get(), color);
    if (texture.is_error()) {
        dbgln("Compositor: Unable to create a composition texture of a color: {}", texture.error());
        return nullptr;
    }
    if (composition_textures_filled_with_colors.size() >= maximum_count_of_composition_textures_filled_with_colors)
        composition_textures_filled_with_colors.clear();
    auto* composition_texture = texture.value().Get();
    composition_textures_filled_with_colors.set(color, texture.release_value());
    return composition_texture;
}

struct DirectCompositionSurface::Impl {
    AK_ALLOC_WITH_KMALLOC;

    IDCompositionTexture* composition_texture_shown_by(u64 window_server_id, IDCompositionDevice4&, DXGI_ALPHA_MODE, Gfx::IntRect source_rect);

    struct CompositionTexture {
        u64 window_server_id { 0 };
        ComPtr<IDCompositionTexture> texture;
        DXGI_ALPHA_MODE alpha_mode { DXGI_ALPHA_MODE_UNSPECIFIED };
        Optional<Gfx::IntRect> source_rect {};
    };

    NonnullRefPtr<Gfx::D3DSharedTexture> shared_texture;
    ComPtr<ID3D11Texture2D> texture_opened_on_d3d11_device;
    // One per window server that showed the surface.
    Vector<CompositionTexture, 1> composition_textures;
};

IDCompositionTexture* DirectCompositionSurface::Impl::composition_texture_shown_by(u64 window_server_id, IDCompositionDevice4& device, DXGI_ALPHA_MODE alpha_mode, Gfx::IntRect source_rect)
{
    CompositionTexture* composition_texture = nullptr;
    if (auto shown = composition_textures.find_if([&](auto const& texture) { return texture.window_server_id == window_server_id; }); !shown.is_end())
        composition_texture = &*shown;
    if (!composition_texture) {
        if (!texture_opened_on_d3d11_device) {
            auto* d3d11_device = d3d11_device_for_composition_textures();
            VERIFY(d3d11_device);
            if (auto result = d3d11_device->OpenSharedResource1(shared_texture->shared_handle(), IID_PPV_ARGS(&texture_opened_on_d3d11_device)); FAILED(result)) {
                dbgln("Compositor: Unable to open a surface on the Direct3D 11 device: {}", Error::from_windows_error(result));
                return nullptr;
            }
        }
        // A composition texture made of the Direct3D 12 resource itself shows nothing, so it is made of the texture
        // that the Direct3D 11 device opened.
        ComPtr<IDCompositionTexture> texture;
        if (auto result = device.CreateCompositionTexture(texture_opened_on_d3d11_device.Get(), &texture); FAILED(result)) {
            dbgln("Compositor: Unable to create a composition texture of a surface: {}", Error::from_windows_error(result));
            return nullptr;
        }
        if (!texture) {
            dbgln("Compositor: DirectComposition made no composition texture of a surface");
            return nullptr;
        }
        composition_textures.append({ window_server_id, move(texture), DXGI_ALPHA_MODE_UNSPECIFIED });
        composition_texture = &composition_textures.last();
    }
    if (composition_texture->alpha_mode != alpha_mode) {
        if (auto result = composition_texture->texture->SetAlphaMode(alpha_mode); FAILED(result))
            dbgln("Compositor: Unable to set the alpha mode of a composition texture: {}", Error::from_windows_error(result));
        else
            composition_texture->alpha_mode = alpha_mode;
    }
    if (composition_texture->source_rect != source_rect) {
        D2D_RECT_U rect {
            static_cast<UINT32>(source_rect.left()),
            static_cast<UINT32>(source_rect.top()),
            static_cast<UINT32>(source_rect.right()),
            static_cast<UINT32>(source_rect.bottom()),
        };
        if (auto result = composition_texture->texture->SetSourceRect(rect); FAILED(result))
            dbgln("Compositor: Unable to set the source rect of a composition texture: {}", Error::from_windows_error(result));
        else
            composition_texture->source_rect = source_rect;
    }
    return composition_texture->texture.Get();
}

DirectCompositionSurface::DirectCompositionSurface() = default;

DirectCompositionSurface::DirectCompositionSurface(NonnullRefPtr<Gfx::D3DSharedTexture> shared_texture)
    : m_impl(adopt_own(*new Impl {
          .shared_texture = move(shared_texture),
          .texture_opened_on_d3d11_device = nullptr,
          .composition_textures = {},
      }))
{
}

DirectCompositionSurface::~DirectCompositionSurface() = default;

bool DirectCompositionSurface::may_be_read_by_window_server() const
{
    if (!m_impl)
        return false;
    // A composition texture has no fence for as long as any visual has it as its content, even a visual that is no
    // longer in the tree. Once none does, and that is committed, it has a fence that reaches the value it gives once
    // the window server has stopped reading it.
    for (auto const& composition_texture : m_impl->composition_textures) {
        UINT64 value_once_available = 0;
        ComPtr<ID3D11Fence> available_fence;
        if (auto result = composition_texture.texture->GetAvailableFence(&value_once_available, IID_PPV_ARGS(&available_fence)); FAILED(result) || !available_fence)
            return true;
        if (available_fence->GetCompletedValue() < value_once_available)
            return true;
    }
    return false;
}

class DirectCompositionRemoteLayer final : public RemoteLayer {
public:
    static OwnPtr<DirectCompositionRemoteLayer> try_create(DirectCompositionWindowServer&);
    virtual ~DirectCompositionRemoteLayer() override;

    virtual Compositing::RemoteLayerHostHandle host_handle() const override;
    virtual bool can_show(WindowServerPlacement const&, Content const&, double device_pixel_ratio) const override;
    virtual bool show_frame(Frame) override;
    virtual void place_frame(Optional<Gfx::IntSize> fit_within) override;

    void show_changes();

    enum class SublayerKind : u8 {
        // A surface, in a visual of its own inside the visuals that clip it.
        PlacedContent,
        Group,
        Tile,
        Color,
        // A backdrop filter, which the window server does not apply here, so its visual shows nothing.
        Backdrop,
    };

private:
    DirectCompositionRemoteLayer(DirectCompositionWindowServer& window_server, HWND window)
        : m_window_server(window_server)
        , m_window(window)
    {
    }

    struct ShownSublayer {
        SublayerKind kind { SublayerKind::PlacedContent };
        // The visual among the children of the group the sublayer is in, or of the frame: the visual that clips placed
        // content, or the one visual of a group, a tile, a color or a backdrop.
        ComPtr<IDCompositionVisual2> outermost_visual;
        // The visual that shows the content: the innermost visual of placed content, or the one visual of a tile or a
        // color.
        ComPtr<IDCompositionVisual2> content_visual;
        // The clip of the outermost visual of placed content, or of a group that clips.
        ComPtr<IDCompositionRectangleClip> clip;
        ComPtr<IDCompositionVisual2> rounded_clip_visual;
        ComPtr<IDCompositionRectangleClip> rounded_clip;
        OwnPtr<CompositionSwapChain> swap_chain;
        Optional<PresentableSurface::Lease> content;
        bool content_visual_shows_composition_texture_of_surface { false };
        bool outermost_visual_is_child_of_parent { false };
        // What a group, a tile or a color was last shown with, so that a frame sets only what it changes.
        Optional<RemoteSublayer::Group> shown_group;
        Optional<Gfx::IntRect> shown_rect;
        Optional<Gfx::Color> shown_color;
        // Of a group, the sublayers in it, bottom to top.
        Vector<u64> identities_of_sublayers;
    };

    DirectCompositionWindowServer::Impl& window_server_impl() const { return *m_window_server->m_impl; }
    ErrorOr<OwnPtr<CompositionSwapChain>> create_swap_chain(Gfx::IntSize, DXGI_ALPHA_MODE) const;
    IDCompositionTexture* composition_texture_of(PresentableSurface const&, DXGI_ALPHA_MODE, Gfx::IntRect source_rect) const;
    void lay_out_frame();
    void stop_showing_store();
    void stop_showing_frame();
    void show_sublayers(Vector<RemoteSublayer>&);
    ShownSublayer* shown_sublayer_of_kind(u64 identity, SublayerKind);
    ErrorOr<void> update_shown_sublayer(ShownSublayer&, RemoteSublayer&);
    void update_shown_group(ShownSublayer&, RemoteSublayer::Group const&);
    void update_shown_tile(ShownSublayer&, RemoteSublayer::Tile&, Gfx::IntRect);
    void update_shown_color(ShownSublayer&, Gfx::Color, Gfx::IntRect);
    void remove_visuals_of_sublayers(IDCompositionVisual2& parent, Vector<u64> const& identities_of_sublayers);
    void stop_showing_sublayer(ShownSublayer&);
    void stop_showing_sublayers();
    void show_changes_unless_held();

    NonnullRefPtr<DirectCompositionWindowServer> m_window_server;
    HWND m_window { nullptr };
    ComPtr<IDCompositionTarget> m_target;
    ComPtr<IDCompositionVisual2> m_root_visual;
    ComPtr<IDCompositionVisual2> m_frame_visual;
    OwnPtr<CompositionSwapChain> m_frame_swap_chain;
    HashMap<u64, ShownSublayer> m_shown_sublayers;
    // The sublayers of the frame itself, bottom to top.
    Vector<u64> m_identities_of_sublayers_of_frame;
    Gfx::IntSize m_painted_size;
    Optional<Gfx::IntSize> m_fit_within;
    Gfx::IntSize m_window_size;
    Gfx::IntSize m_window_size_to_show;
    bool m_has_changes_to_show { false };
};

OwnPtr<DirectCompositionRemoteLayer> DirectCompositionRemoteLayer::try_create(DirectCompositionWindowServer& window_server)
{
    auto* child_window_thread = ChildWindowThread::the();
    if (!child_window_thread)
        return nullptr;
    auto window = child_window_thread->create_child_window();
    if (!window)
        return nullptr;

    auto layer = adopt_own(*new DirectCompositionRemoteLayer(window_server, window));
    auto& device = *window_server.m_impl->device.Get();
    if (auto result = device.CreateTargetForHwnd(window, TRUE, &layer->m_target); FAILED(result)) {
        dbgln("Compositor: Unable to target the window of a remote layer: {}", Error::from_windows_error(result));
        return nullptr;
    }
    if (FAILED(device.CreateVisual(&layer->m_root_visual)) || FAILED(device.CreateVisual(&layer->m_frame_visual)))
        return nullptr;
    layer->m_frame_visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (FAILED(layer->m_root_visual->AddVisual(layer->m_frame_visual.Get(), FALSE, nullptr)) || FAILED(layer->m_target->SetRoot(layer->m_root_visual.Get())))
        return nullptr;
    window_server.m_layers.append(layer.ptr());
    window_server.commit();
    return layer;
}

DirectCompositionRemoteLayer::~DirectCompositionRemoteLayer()
{
    m_window_server->m_layers.remove_first_matching([this](auto* layer) { return layer == this; });
    stop_showing_sublayers();
    if (m_target)
        m_target->SetRoot(nullptr);
    m_target = nullptr;
    m_frame_visual = nullptr;
    m_root_visual = nullptr;
    m_frame_swap_chain = nullptr;
    m_window_server->commit();
    if (auto* child_window_thread = ChildWindowThread::the())
        child_window_thread->destroy_child_window(m_window);
}

Compositing::RemoteLayerHostHandle DirectCompositionRemoteLayer::host_handle() const
{
    return { .kind = Compositing::RemoteLayerHostKind::Win32ChildWindow, .id = static_cast<u64>(reinterpret_cast<uintptr_t>(m_window)) };
}

bool DirectCompositionRemoteLayer::can_show(WindowServerPlacement const& placement, Content const& content, double) const
{
    if (placement.is_clipped_by_path)
        return false;
    return content.visit(
        [&](NonnullRefPtr<PresentableSurface> const& surface) {
            return surface->window_server_surface() != nullptr && surface->painting_surface().skia_backend_context() == window_server_impl().skia_backend_context.ptr();
        },
        [](NonnullRefPtr<Media::VideoFrame> const&) {
            return false;
        });
}

ErrorOr<OwnPtr<CompositionSwapChain>> DirectCompositionRemoteLayer::create_swap_chain(Gfx::IntSize size, DXGI_ALPHA_MODE alpha_mode) const
{
    return TRY(CompositionSwapChain::create(window_server_impl().skia_backend_context, *window_server_impl().dxgi_factory.Get(), size, alpha_mode));
}

bool DirectCompositionRemoteLayer::show_frame(Frame frame)
{
    if (frame.store.has_value()) {
        auto& store = frame.store->surface();
        Gfx::IntSize painted_size { min(frame.painted_size.width(), store.size().width()), min(frame.painted_size.height(), store.size().height()) };
        if (painted_size.is_empty())
            return true;
        if (!m_frame_swap_chain) {
            auto swap_chain = create_swap_chain(painted_size, DXGI_ALPHA_MODE_IGNORE);
            if (swap_chain.is_error()) {
                dbgln("Compositor: Unable to create the swap chain of a remote layer: {}", swap_chain.error());
                stop_showing_frame();
                return false;
            }
            m_frame_swap_chain = swap_chain.release_value();
            m_frame_visual->SetContent(m_frame_swap_chain->content());
        }
        if (auto result = m_frame_swap_chain->draw(store.painting_surface(), painted_size); result.is_error()) {
            dbgln("Compositor: Unable to show a frame in a remote layer: {}", result.error());
            stop_showing_frame();
            return false;
        }
        m_painted_size = painted_size;
    } else if (frame.shows_no_store) {
        if (frame.painted_size.is_empty())
            return true;
        if (m_frame_swap_chain)
            stop_showing_store();
        m_painted_size = frame.painted_size;
    }
    if (m_painted_size.is_empty())
        return true;
    m_fit_within = frame.fit_within;
    lay_out_frame();
    show_sublayers(frame.sublayers);
    show_changes_unless_held();
    return true;
}

void DirectCompositionRemoteLayer::stop_showing_store()
{
    m_frame_visual->SetContent(nullptr);
    m_frame_swap_chain = nullptr;
}

void DirectCompositionRemoteLayer::stop_showing_frame()
{
    stop_showing_store();
    stop_showing_sublayers();
    m_painted_size = {};
    m_has_changes_to_show = true;
    show_changes_unless_held();
}

static DirectCompositionRemoteLayer::SublayerKind kind_of(RemoteSublayer const& sublayer)
{
    using Kind = DirectCompositionRemoteLayer::SublayerKind;
    return sublayer.content.visit(
        [](RemoteSublayer::Group const&) { return Kind::Group; },
        [](RemoteSublayer::Tile const&) { return Kind::Tile; },
        [](Gfx::Color const&) { return Kind::Color; },
        [](RemoteSublayer::Backdrop const&) { return Kind::Backdrop; },
        [](auto const&) { return Kind::PlacedContent; });
}

DirectCompositionRemoteLayer::ShownSublayer* DirectCompositionRemoteLayer::shown_sublayer_of_kind(u64 identity, SublayerKind kind)
{
    if (auto shown_sublayer = m_shown_sublayers.find(identity); shown_sublayer != m_shown_sublayers.end()) {
        if (shown_sublayer->value.kind == kind)
            return &shown_sublayer->value;
        stop_showing_sublayer(shown_sublayer->value);
        m_shown_sublayers.remove(shown_sublayer);
    }
    auto& device = *window_server_impl().device.Get();
    ShownSublayer shown_sublayer;
    shown_sublayer.kind = kind;
    if (FAILED(device.CreateVisual(&shown_sublayer.outermost_visual))) {
        dbgln("Compositor: Unable to create the visual of a sublayer");
        return nullptr;
    }
    auto& outermost_visual = *shown_sublayer.outermost_visual.Get();
    switch (kind) {
    case SublayerKind::PlacedContent:
        if (FAILED(device.CreateRectangleClip(&shown_sublayer.clip)) || FAILED(device.CreateVisual(&shown_sublayer.content_visual))) {
            dbgln("Compositor: Unable to create the visuals of a sublayer");
            return nullptr;
        }
        outermost_visual.SetClip(shown_sublayer.clip.Get());
        outermost_visual.AddVisual(shown_sublayer.content_visual.Get(), FALSE, nullptr);
        break;
    case SublayerKind::Tile:
        shown_sublayer.content_visual = shown_sublayer.outermost_visual;
        outermost_visual.SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        outermost_visual.SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
        break;
    case SublayerKind::Color:
        shown_sublayer.content_visual = shown_sublayer.outermost_visual;
        outermost_visual.SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        outermost_visual.SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
        break;
    case SublayerKind::Group:
    case SublayerKind::Backdrop:
        break;
    }
    m_shown_sublayers.set(identity, move(shown_sublayer));
    return &m_shown_sublayers.find(identity)->value;
}

void DirectCompositionRemoteLayer::show_sublayers(Vector<RemoteSublayer>& sublayers)
{
    struct SublayersOfParent {
        Vector<u64> identities;
        bool has_sublayer_whose_visual_is_child_of_no_parent { false };
    };
    HashTable<u64> shown_identities;
    // Per sublayer, by its index, the sublayers in it where it is a group, and after those, the sublayers of the frame
    // itself.
    Vector<SublayersOfParent> sublayers_by_group_index;
    sublayers_by_group_index.resize(sublayers.size() + 1);
    auto index_of_frame = sublayers.size();
    for (size_t index = 0; index < sublayers.size(); ++index) {
        auto& sublayer = sublayers[index];
        auto* shown_sublayer = shown_sublayer_of_kind(sublayer.identity, kind_of(sublayer));
        if (!shown_sublayer)
            continue;
        sublayer.content.visit(
            [&](RemoteSublayer::Group& group) { update_shown_group(*shown_sublayer, group); },
            [&](RemoteSublayer::Tile& tile) { update_shown_tile(*shown_sublayer, tile, sublayer.content_rect); },
            [&](Gfx::Color& color) { update_shown_color(*shown_sublayer, color, sublayer.content_rect); },
            [&](RemoteSublayer::Backdrop&) { },
            [&](auto&) {
                if (auto result = update_shown_sublayer(*shown_sublayer, sublayer); result.is_error())
                    dbgln("Compositor: Unable to show a sublayer: {}", result.error());
            });
        auto group_index = sublayer.group_index.value_or(index_of_frame);
        // A group comes before the sublayers in it.
        if (group_index != index_of_frame && (group_index >= index || !sublayers[group_index].content.has<RemoteSublayer::Group>()))
            group_index = index_of_frame;
        auto& sublayers_of_parent = sublayers_by_group_index[group_index];
        sublayers_of_parent.identities.append(sublayer.identity);
        if (!shown_sublayer->outermost_visual_is_child_of_parent)
            sublayers_of_parent.has_sublayer_whose_visual_is_child_of_no_parent = true;
        shown_identities.set(sublayer.identity);
    }

    Vector<u64> identities_no_longer_shown;
    for (auto& it : m_shown_sublayers) {
        if (!shown_identities.contains(it.key))
            identities_no_longer_shown.append(it.key);
    }
    for (auto identity : identities_no_longer_shown) {
        auto shown_sublayer = m_shown_sublayers.take(identity).release_value();
        stop_showing_sublayer(shown_sublayer);
    }

    // A visual that is a child of one parent cannot be added to another, so the children of every parent whose
    // sublayers changed are all removed before any is added again.
    struct ParentWithChangedSublayers {
        IDCompositionVisual2* visual { nullptr };
        Vector<u64>* shown_identities_of_sublayers { nullptr };
        Vector<u64>* identities_of_sublayers { nullptr };
    };
    Vector<ParentWithChangedSublayers> parents_with_changed_sublayers;
    auto remove_visuals_of_sublayers_if_changed = [&](IDCompositionVisual2& parent, Vector<u64>& shown_identities_of_sublayers, SublayersOfParent& sublayers_of_parent) {
        if (!sublayers_of_parent.has_sublayer_whose_visual_is_child_of_no_parent && shown_identities_of_sublayers == sublayers_of_parent.identities)
            return;
        remove_visuals_of_sublayers(parent, shown_identities_of_sublayers);
        parents_with_changed_sublayers.append({ &parent, &shown_identities_of_sublayers, &sublayers_of_parent.identities });
    };
    for (size_t index = 0; index < sublayers.size(); ++index) {
        if (!sublayers[index].content.has<RemoteSublayer::Group>())
            continue;
        auto shown_group = m_shown_sublayers.find(sublayers[index].identity);
        if (shown_group == m_shown_sublayers.end() || shown_group->value.kind != SublayerKind::Group)
            continue;
        remove_visuals_of_sublayers_if_changed(*shown_group->value.outermost_visual.Get(), shown_group->value.identities_of_sublayers, sublayers_by_group_index[index]);
    }
    remove_visuals_of_sublayers_if_changed(*m_frame_visual.Get(), m_identities_of_sublayers_of_frame, sublayers_by_group_index[index_of_frame]);
    for (auto& parent : parents_with_changed_sublayers) {
        IDCompositionVisual2* visual_below = nullptr;
        for (auto identity : *parent.identities_of_sublayers) {
            auto& shown_sublayer = m_shown_sublayers.find(identity)->value;
            auto* visual = shown_sublayer.outermost_visual.Get();
            if (auto result = parent.visual->AddVisual(visual, TRUE, visual_below); FAILED(result)) {
                dbgln("Compositor: Unable to add the visual of a sublayer to its parent: {}", Error::from_windows_error(result));
                continue;
            }
            shown_sublayer.outermost_visual_is_child_of_parent = true;
            visual_below = visual;
        }
        *parent.shown_identities_of_sublayers = move(*parent.identities_of_sublayers);
    }
}

void DirectCompositionRemoteLayer::remove_visuals_of_sublayers(IDCompositionVisual2& parent, Vector<u64> const& identities_of_sublayers)
{
    parent.RemoveAllVisuals();
    for (auto identity : identities_of_sublayers) {
        if (auto shown_sublayer = m_shown_sublayers.find(identity); shown_sublayer != m_shown_sublayers.end())
            shown_sublayer->value.outermost_visual_is_child_of_parent = false;
    }
}

// A composition texture reads as available again only once no visual has it as its content, not even one that is no
// longer in the tree, so a sublayer takes it out of its visual before the visual goes.
void DirectCompositionRemoteLayer::stop_showing_sublayer(ShownSublayer& shown_sublayer)
{
    if (shown_sublayer.content_visual_shows_composition_texture_of_surface) {
        shown_sublayer.content_visual->SetContent(nullptr);
        shown_sublayer.content_visual_shows_composition_texture_of_surface = false;
    }
    if (shown_sublayer.kind == SublayerKind::Group)
        remove_visuals_of_sublayers(*shown_sublayer.outermost_visual.Get(), shown_sublayer.identities_of_sublayers);
    shown_sublayer.content.clear();
}

void DirectCompositionRemoteLayer::stop_showing_sublayers()
{
    for (auto& it : m_shown_sublayers)
        stop_showing_sublayer(it.value);
    if (m_frame_visual)
        m_frame_visual->RemoveAllVisuals();
    m_shown_sublayers.clear();
    m_identities_of_sublayers_of_frame.clear();
}

static D2D_MATRIX_3X2_F to_direct2d_matrix(Gfx::AffineTransform const& transform)
{
    D2D_MATRIX_3X2_F matrix {};
    matrix._11 = transform.a();
    matrix._12 = transform.b();
    matrix._21 = transform.c();
    matrix._22 = transform.d();
    matrix._31 = transform.e();
    matrix._32 = transform.f();
    return matrix;
}

static void set_rectangle(IDCompositionRectangleClip& clip, Gfx::FloatRect rect)
{
    clip.SetLeft(rect.left());
    clip.SetTop(rect.top());
    clip.SetRight(rect.right());
    clip.SetBottom(rect.bottom());
}

static DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode_for_scaling_mode(Gfx::ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case Gfx::ScalingMode::None:
    case Gfx::ScalingMode::NearestNeighbor:
        return DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    case Gfx::ScalingMode::Bilinear:
    case Gfx::ScalingMode::BilinearMipmap:
        return DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> DirectCompositionRemoteLayer::update_shown_sublayer(ShownSublayer& shown_sublayer, RemoteSublayer& sublayer)
{
    auto& device = *window_server_impl().device.Get();
    set_rectangle(*shown_sublayer.clip.Get(), sublayer.clip_rect);

    if (sublayer.rounded_clip.has_value()) {
        auto const& rounded_clip = *sublayer.rounded_clip;
        if (!shown_sublayer.rounded_clip_visual) {
            if (auto result = device.CreateVisual(&shown_sublayer.rounded_clip_visual); FAILED(result))
                return Error::from_windows_error(result);
            if (auto result = device.CreateRectangleClip(&shown_sublayer.rounded_clip); FAILED(result))
                return Error::from_windows_error(result);
            shown_sublayer.rounded_clip_visual->SetClip(shown_sublayer.rounded_clip.Get());
            shown_sublayer.outermost_visual->RemoveAllVisuals();
            shown_sublayer.rounded_clip_visual->AddVisual(shown_sublayer.content_visual.Get(), FALSE, nullptr);
            shown_sublayer.outermost_visual->AddVisual(shown_sublayer.rounded_clip_visual.Get(), FALSE, nullptr);
        }
        auto& clip = *shown_sublayer.rounded_clip.Get();
        set_rectangle(clip, rounded_clip.rect);
        auto radius_if = [&](bool rounds) { return rounds ? rounded_clip.radius : 0.0f; };
        clip.SetTopLeftRadiusX(radius_if(rounded_clip.rounds_top_left));
        clip.SetTopLeftRadiusY(radius_if(rounded_clip.rounds_top_left));
        clip.SetTopRightRadiusX(radius_if(rounded_clip.rounds_top_right));
        clip.SetTopRightRadiusY(radius_if(rounded_clip.rounds_top_right));
        clip.SetBottomRightRadiusX(radius_if(rounded_clip.rounds_bottom_right));
        clip.SetBottomRightRadiusY(radius_if(rounded_clip.rounds_bottom_right));
        clip.SetBottomLeftRadiusX(radius_if(rounded_clip.rounds_bottom_left));
        clip.SetBottomLeftRadiusY(radius_if(rounded_clip.rounds_bottom_left));
    } else if (shown_sublayer.rounded_clip_visual) {
        shown_sublayer.outermost_visual->RemoveAllVisuals();
        shown_sublayer.rounded_clip_visual->RemoveAllVisuals();
        shown_sublayer.outermost_visual->AddVisual(shown_sublayer.content_visual.Get(), FALSE, nullptr);
        shown_sublayer.rounded_clip_visual = nullptr;
        shown_sublayer.rounded_clip = nullptr;
    }

    auto* lease = sublayer.content.get_pointer<PresentableSurface::Lease>();
    if (!lease)
        return Error::from_string_literal("Only surfaces can be shown as sublayers");
    auto& surface = lease->surface();
    auto surface_size = surface.size();
    if (surface_size.is_empty())
        return {};

    auto content_rect = sublayer.content_rect;
    auto transform = sublayer.content_to_frame;
    transform.translate(content_rect.x(), content_rect.y());
    transform.scale(static_cast<float>(content_rect.width()) / surface_size.width(), static_cast<float>(content_rect.height()) / surface_size.height());
    auto& content_visual = *shown_sublayer.content_visual.Get();
    content_visual.SetTransform(to_direct2d_matrix(transform));
    content_visual.SetClip(D2D_RECT_F { 0, 0, static_cast<float>(surface_size.width()), static_cast<float>(surface_size.height()) });
    content_visual.SetBitmapInterpolationMode(interpolation_mode_for_scaling_mode(sublayer.scaling_mode));
    content_visual.SetBorderMode(sublayer.content_to_frame.is_identity_or_translation_or_scale() ? DCOMPOSITION_BORDER_MODE_HARD : DCOMPOSITION_BORDER_MODE_SOFT);

    if (shown_sublayer.content.has_value() && &shown_sublayer.content->surface() == &surface)
        return {};
    if (auto* composition_texture = composition_texture_of(surface, DXGI_ALPHA_MODE_PREMULTIPLIED, { {}, surface_size })) {
        shown_sublayer.swap_chain = nullptr;
        surface.did_hand_to_window_server();
        content_visual.SetContent(composition_texture);
        shown_sublayer.content_visual_shows_composition_texture_of_surface = true;
    } else {
        if (!shown_sublayer.swap_chain) {
            shown_sublayer.swap_chain = TRY(create_swap_chain(surface_size, DXGI_ALPHA_MODE_PREMULTIPLIED));
            content_visual.SetContent(shown_sublayer.swap_chain->content());
            shown_sublayer.content_visual_shows_composition_texture_of_surface = false;
        }
        TRY(shown_sublayer.swap_chain->draw(surface.painting_surface(), surface_size));
        m_has_changes_to_show = true;
    }
    shown_sublayer.content = move(*lease);
    return {};
}

IDCompositionTexture* DirectCompositionRemoteLayer::composition_texture_of(PresentableSurface const& surface, DXGI_ALPHA_MODE alpha_mode, Gfx::IntRect source_rect) const
{
    auto& window_server = window_server_impl();
    if (!window_server.device_showing_composition_textures)
        return nullptr;
    auto const* window_server_surface = static_cast<DirectCompositionSurface const*>(surface.window_server_surface());
    if (!window_server_surface || !window_server_surface->m_impl)
        return nullptr;
    return window_server_surface->m_impl->composition_texture_shown_by(window_server.id, *window_server.device_showing_composition_textures.Get(), alpha_mode, source_rect);
}

static bool transforms_alike(Gfx::AffineTransform const& a, Gfx::AffineTransform const& b)
{
    return a.a() == b.a() && a.b() == b.b() && a.c() == b.c() && a.d() == b.d() && a.e() == b.e() && a.f() == b.f();
}

static void set_corner_radii(IDCompositionRectangleClip& clip, Gfx::CornerRadii const& corner_radii)
{
    auto horizontal_radius_of = [](Gfx::CornerRadius const& corner) { return corner ? static_cast<float>(corner.horizontal_radius) : 0.0f; };
    auto vertical_radius_of = [](Gfx::CornerRadius const& corner) { return corner ? static_cast<float>(corner.vertical_radius) : 0.0f; };
    clip.SetTopLeftRadiusX(horizontal_radius_of(corner_radii.top_left));
    clip.SetTopLeftRadiusY(vertical_radius_of(corner_radii.top_left));
    clip.SetTopRightRadiusX(horizontal_radius_of(corner_radii.top_right));
    clip.SetTopRightRadiusY(vertical_radius_of(corner_radii.top_right));
    clip.SetBottomRightRadiusX(horizontal_radius_of(corner_radii.bottom_right));
    clip.SetBottomRightRadiusY(vertical_radius_of(corner_radii.bottom_right));
    clip.SetBottomLeftRadiusX(horizontal_radius_of(corner_radii.bottom_left));
    clip.SetBottomLeftRadiusY(vertical_radius_of(corner_radii.bottom_left));
}

// A group visual has the group's transform and clips to the group's rect in its own pixels, which a visual's clip is in.
// A frame that only moves the group, as a scroll does, sets nothing else.
void DirectCompositionRemoteLayer::update_shown_group(ShownSublayer& shown_sublayer, RemoteSublayer::Group const& group)
{
    auto const* shown_group = shown_sublayer.shown_group.has_value() ? &shown_sublayer.shown_group.value() : nullptr;
    auto& group_visual = *shown_sublayer.outermost_visual.Get();
    if (!shown_group || !transforms_alike(shown_group->to_parent, group.to_parent))
        group_visual.SetTransform(to_direct2d_matrix(group.to_parent));
    if (!shown_group || shown_group->clip_rect != group.clip_rect || shown_group->corner_radii != group.corner_radii) {
        if (group.clip_rect.has_value()) {
            if (!shown_sublayer.clip) {
                if (auto result = window_server_impl().device->CreateRectangleClip(&shown_sublayer.clip); FAILED(result))
                    dbgln("Compositor: Unable to create the clip of a group: {}", Error::from_windows_error(result));
                else
                    group_visual.SetClip(shown_sublayer.clip.Get());
            }
            if (shown_sublayer.clip) {
                set_rectangle(*shown_sublayer.clip.Get(), *group.clip_rect);
                set_corner_radii(*shown_sublayer.clip.Get(), group.corner_radii);
            }
        } else if (shown_sublayer.clip) {
            group_visual.SetClip(static_cast<IDCompositionClip*>(nullptr));
            shown_sublayer.clip = nullptr;
        }
    }
    // Fading the sublayers of a group together takes a layer of their own, which only a translucent group asks for.
    auto shown_opacity = shown_group ? shown_group->opacity : 1.0f;
    if (!shown_group || (shown_opacity < 1) != (group.opacity < 1))
        group_visual.SetOpacityMode(group.opacity < 1 ? DCOMPOSITION_OPACITY_MODE_LAYER : DCOMPOSITION_OPACITY_MODE_MULTIPLY);
    if (shown_opacity != group.opacity) {
        ComPtr<IDCompositionVisual3> group_visual3;
        if (auto result = shown_sublayer.outermost_visual.As(&group_visual3); FAILED(result))
            dbgln("Compositor: Unable to fade a group: {}", Error::from_windows_error(result));
        else
            group_visual3->SetOpacity(group.opacity);
    }
    shown_sublayer.shown_group = group;
}

// A visual shows its content at the content's own size from its origin, so content of another size than the rect is
// scaled into it.
static D2D_MATRIX_3X2_F transform_placing_content_of_size_at(Gfx::IntSize content_size, Gfx::IntRect rect)
{
    D2D_MATRIX_3X2_F matrix {};
    matrix._11 = static_cast<float>(rect.width()) / static_cast<float>(content_size.width());
    matrix._22 = static_cast<float>(rect.height()) / static_cast<float>(content_size.height());
    matrix._31 = static_cast<float>(rect.x());
    matrix._32 = static_cast<float>(rect.y());
    return matrix;
}

void DirectCompositionRemoteLayer::update_shown_tile(ShownSublayer& shown_sublayer, RemoteSublayer::Tile& tile, Gfx::IntRect rect)
{
    auto& tile_visual = *shown_sublayer.outermost_visual.Get();
    auto& surface = tile.surface.surface();
    auto const* shown_lease = shown_sublayer.content.has_value() ? &shown_sublayer.content.value() : nullptr;
    auto shown_surface_size = shown_lease ? Optional<Gfx::IntSize> { shown_lease->surface().size() } : OptionalNone {};
    if (shown_sublayer.shown_rect != rect || shown_surface_size != surface.size()) {
        tile_visual.SetTransform(transform_placing_content_of_size_at(tile.rect_in_surface.size(), rect));
        shown_sublayer.shown_rect = rect;
    }
    auto* composition_texture = composition_texture_of(surface, tile.every_pixel_is_opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED, tile.rect_in_surface);
    if (shown_lease && &shown_lease->surface() == &surface)
        return;
    if (!composition_texture) {
        dbgln("Compositor: The window server cannot show the surface of a tile");
        if (shown_sublayer.content_visual_shows_composition_texture_of_surface) {
            tile_visual.SetContent(nullptr);
            shown_sublayer.content_visual_shows_composition_texture_of_surface = false;
        }
        shown_sublayer.content.clear();
        return;
    }
    surface.did_hand_to_window_server();
    tile_visual.SetContent(composition_texture);
    shown_sublayer.content_visual_shows_composition_texture_of_surface = true;
    shown_sublayer.content = move(tile.surface);
}

void DirectCompositionRemoteLayer::update_shown_color(ShownSublayer& shown_sublayer, Gfx::Color color, Gfx::IntRect rect)
{
    auto& color_visual = *shown_sublayer.outermost_visual.Get();
    if (shown_sublayer.shown_rect != rect) {
        color_visual.SetTransform(transform_placing_content_of_size_at({ 1, 1 }, rect));
        shown_sublayer.shown_rect = rect;
    }
    if (shown_sublayer.shown_color == color)
        return;
    color_visual.SetContent(window_server_impl().composition_texture_filled_with(color));
    shown_sublayer.shown_color = color;
}

void DirectCompositionRemoteLayer::place_frame(Optional<Gfx::IntSize> fit_within)
{
    m_fit_within = fit_within;
    if (m_painted_size.is_empty())
        return;
    lay_out_frame();
    show_changes_unless_held();
}

void DirectCompositionRemoteLayer::lay_out_frame()
{
    m_window_size_to_show = m_fit_within.value_or(m_painted_size);
    D2D_MATRIX_3X2_F transform {};
    transform._11 = 1;
    transform._22 = 1;
    if (m_fit_within.has_value()) {
        auto fitted_rect = Compositing::rect_fitting_frame(m_painted_size, *m_fit_within);
        transform._11 = static_cast<float>(fitted_rect.width()) / max(m_painted_size.width(), 1);
        transform._22 = static_cast<float>(fitted_rect.height()) / max(m_painted_size.height(), 1);
        transform._31 = static_cast<float>(fitted_rect.x());
        transform._32 = static_cast<float>(fitted_rect.y());
    }
    m_frame_visual->SetTransform(transform);
    m_frame_visual->SetClip(D2D_RECT_F { 0, 0, static_cast<float>(m_painted_size.width()), static_cast<float>(m_painted_size.height()) });
    m_has_changes_to_show = true;
}

void DirectCompositionRemoteLayer::show_changes_unless_held()
{
    if (m_window_server->m_holds_changes_off_screen)
        return;
    show_changes();
    m_window_server->commit();
}

void DirectCompositionRemoteLayer::show_changes()
{
    if (!exchange(m_has_changes_to_show, false))
        return;
    if (m_window_size != m_window_size_to_show) {
        auto dpi_awareness = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!SetWindowPos(m_window, nullptr, 0, 0, m_window_size_to_show.width(), m_window_size_to_show.height(),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOOWNERZORDER))
            dbgln("Compositor: Unable to resize the window of a remote layer: error {}", GetLastError());
        SetThreadDpiAwarenessContext(dpi_awareness);
        m_window_size = m_window_size_to_show;
    }
    if (m_frame_swap_chain)
        m_frame_swap_chain->show_drawing();
    for (auto& [identity, shown_sublayer] : m_shown_sublayers) {
        if (shown_sublayer.swap_chain)
            shown_sublayer.swap_chain->show_drawing();
    }
}

RefPtr<DirectCompositionWindowServer> DirectCompositionWindowServer::try_create()
{
    auto skia_backend_context = Gfx::SkiaBackendContext::the_main_thread_context();
    if (!skia_backend_context || !skia_backend_context->sk_context() || skia_backend_context->sk_context()->backend() != GrBackendApi::kDirect3D)
        return nullptr;

    ComPtr<IDXGIFactory2> dxgi_factory;
    if (auto result = skia_backend_context->direct3d_context().adapter()->GetParent(IID_PPV_ARGS(&dxgi_factory)); FAILED(result)) {
        dbgln("Compositor: Unable to reach the DXGI factory of the GPU: {}", Error::from_windows_error(result));
        return nullptr;
    }

    ComPtr<IDCompositionDesktopDevice> device;
    ComPtr<IDCompositionDevice4> device_showing_composition_textures;
    if (d3d11_device_for_composition_textures()) {
        auto result = DCompositionCreateDevice3(nullptr, IID_PPV_ARGS(&device));
        if (SUCCEEDED(result))
            result = device.As(&device_showing_composition_textures);
        if (FAILED(result)) {
            dbgln("Compositor: Unable to create a DirectComposition device that shows composition textures: {}", Error::from_windows_error(result));
            device = nullptr;
        }
    }
    if (!device) {
        if (auto result = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&device)); FAILED(result)) {
            dbgln("Compositor: Unable to create a DirectComposition device: {}", Error::from_windows_error(result));
            return nullptr;
        }
    }

    static u64 next_window_server_id = 0;
    auto impl = adopt_own(*new Impl {
        .skia_backend_context = skia_backend_context.release_nonnull(),
        .device = move(device),
        .device_showing_composition_textures = move(device_showing_composition_textures),
        .dxgi_factory = move(dxgi_factory),
        .id = next_window_server_id++,
        .composition_textures_filled_with_colors = {},
    });
    return adopt_ref(*new DirectCompositionWindowServer(move(impl)));
}

DirectCompositionWindowServer::DirectCompositionWindowServer(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

DirectCompositionWindowServer::~DirectCompositionWindowServer()
{
    VERIFY(m_layers.is_empty());
}

OwnPtr<RemoteLayer> DirectCompositionWindowServer::try_create_remote_layer(u64)
{
    return DirectCompositionRemoteLayer::try_create(*this);
}

bool DirectCompositionWindowServer::shows_tiles_of_painted_layers() const
{
    return m_impl->device_showing_composition_textures.Get() != nullptr;
}

// The window server reads a composition texture in no order with the GPU work that drew into it, unlike a swap chain,
// which this process copies surfaces into on its own queue.
WindowServer::FrameHandOff DirectCompositionWindowServer::frame_hand_off() const
{
    return m_impl->device_showing_composition_textures ? FrameHandOff::OnceGpuWorkIsFinished : FrameHandOff::OnceGpuWorkIsScheduled;
}

static ErrorOr<NonnullRefPtr<PresentableSurface>> create_presentable_surface(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto painting_surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context));
    return PresentableSurface::create(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft, make<DirectCompositionSurface>());
}

ErrorOr<NonnullRefPtr<PresentableSurface>> DirectCompositionWindowServer::try_create_presentable_surface_on_gpu(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    if (size.is_empty() || &skia_backend_context != Gfx::SkiaBackendContext::the_main_thread_context().ptr() || !d3d11_device_for_composition_textures())
        return create_presentable_surface(size, skia_backend_context);
    auto shared_texture = TRY(Gfx::D3DSharedTexture::create(skia_backend_context.direct3d_context(), size));
    auto painting_surface = TRY(Gfx::PaintingSurface::create_from_d3d_texture_also_drawn_from(skia_backend_context, shared_texture));
    return PresentableSurface::create(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft, make<DirectCompositionSurface>(move(shared_texture)));
}

ErrorOr<NonnullRefPtr<PresentableSurface>> DirectCompositionWindowServer::try_create_presentable_surface(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    return create_presentable_surface(size, skia_backend_context);
}

void DirectCompositionWindowServer::hold_changes_on_screen()
{
    m_holds_changes_off_screen = true;
}

void DirectCompositionWindowServer::release_changes_held_off_screen()
{
    m_holds_changes_off_screen = false;
    for (auto* layer : m_layers)
        layer->show_changes();
    commit();
}

void DirectCompositionWindowServer::commit()
{
    if (m_holds_changes_off_screen)
        return;
    if (auto result = m_impl->device->Commit(); FAILED(result))
        dbgln("Compositor: Unable to commit the visuals of remote layers: {}", Error::from_windows_error(result));
}

}
