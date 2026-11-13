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

struct DirectCompositionWindowServer::Impl {
    AK_ALLOC_WITH_KMALLOC;

    NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context;
    ComPtr<IDCompositionDesktopDevice> device;
    ComPtr<IDXGIFactory2> dxgi_factory;
};

class DirectCompositionRemoteLayer final : public RemoteLayer {
public:
    static OwnPtr<DirectCompositionRemoteLayer> try_create(DirectCompositionWindowServer&);
    virtual ~DirectCompositionRemoteLayer() override;

    virtual Compositing::RemoteLayerHostHandle host_handle() const override;
    virtual bool can_show(WindowServerPlacement const&, Content const&, double device_pixel_ratio) const override;
    virtual bool show_frame(Frame) override;
    virtual void place_frame(Optional<Gfx::IntSize> fit_within) override;

    void show_changes();

private:
    DirectCompositionRemoteLayer(DirectCompositionWindowServer& window_server, HWND window)
        : m_window_server(window_server)
        , m_window(window)
    {
    }

    struct ShownSublayer {
        ComPtr<IDCompositionVisual2> clip_visual;
        ComPtr<IDCompositionRectangleClip> clip;
        ComPtr<IDCompositionVisual2> rounded_clip_visual;
        ComPtr<IDCompositionRectangleClip> rounded_clip;
        ComPtr<IDCompositionVisual2> content_visual;
        OwnPtr<CompositionSwapChain> swap_chain;
        Optional<PresentableSurface::Lease> content;
    };

    DirectCompositionWindowServer::Impl& window_server_impl() const { return *m_window_server->m_impl; }
    ErrorOr<OwnPtr<CompositionSwapChain>> create_swap_chain(Gfx::IntSize, DXGI_ALPHA_MODE) const;
    void lay_out_frame();
    void stop_showing_frame();
    void show_sublayers(Vector<RemoteSublayer>&);
    ErrorOr<void> update_shown_sublayer(ShownSublayer&, RemoteSublayer&);
    void show_changes_unless_held();

    NonnullRefPtr<DirectCompositionWindowServer> m_window_server;
    HWND m_window { nullptr };
    ComPtr<IDCompositionTarget> m_target;
    ComPtr<IDCompositionVisual2> m_root_visual;
    ComPtr<IDCompositionVisual2> m_frame_visual;
    OwnPtr<CompositionSwapChain> m_frame_swap_chain;
    HashMap<u64, ShownSublayer> m_shown_sublayers;
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
    if (m_target)
        m_target->SetRoot(nullptr);
    m_target = nullptr;
    m_shown_sublayers.clear();
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

bool DirectCompositionRemoteLayer::can_show(WindowServerPlacement const&, Content const& content, double) const
{
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
    }
    if (m_painted_size.is_empty())
        return true;
    m_fit_within = frame.fit_within;
    lay_out_frame();
    show_sublayers(frame.sublayers);
    show_changes_unless_held();
    return true;
}

void DirectCompositionRemoteLayer::stop_showing_frame()
{
    m_frame_visual->SetContent(nullptr);
    m_frame_visual->RemoveAllVisuals();
    m_shown_sublayers.clear();
    m_frame_swap_chain = nullptr;
    m_painted_size = {};
    m_has_changes_to_show = true;
    show_changes_unless_held();
}

void DirectCompositionRemoteLayer::show_sublayers(Vector<RemoteSublayer>& sublayers)
{
    auto& device = *window_server_impl().device.Get();
    HashTable<u64> shown_identities;
    m_frame_visual->RemoveAllVisuals();
    IDCompositionVisual2* visual_below = nullptr;
    for (auto& sublayer : sublayers) {
        auto& shown_sublayer = m_shown_sublayers.ensure(sublayer.identity, [] { return ShownSublayer {}; });
        if (!shown_sublayer.clip_visual) {
            if (FAILED(device.CreateVisual(&shown_sublayer.clip_visual)) || FAILED(device.CreateRectangleClip(&shown_sublayer.clip))
                || FAILED(device.CreateVisual(&shown_sublayer.content_visual))) {
                dbgln("Compositor: Unable to create the visuals of a sublayer");
                m_shown_sublayers.remove(sublayer.identity);
                continue;
            }
            shown_sublayer.clip_visual->SetClip(shown_sublayer.clip.Get());
            shown_sublayer.clip_visual->AddVisual(shown_sublayer.content_visual.Get(), FALSE, nullptr);
        }
        if (auto result = update_shown_sublayer(shown_sublayer, sublayer); result.is_error())
            dbgln("Compositor: Unable to show a sublayer: {}", result.error());
        m_frame_visual->AddVisual(shown_sublayer.clip_visual.Get(), TRUE, visual_below);
        visual_below = shown_sublayer.clip_visual.Get();
        shown_identities.set(sublayer.identity);
    }
    m_shown_sublayers.remove_all_matching([&](u64 identity, auto&) { return !shown_identities.contains(identity); });
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
            shown_sublayer.clip_visual->RemoveAllVisuals();
            shown_sublayer.rounded_clip_visual->AddVisual(shown_sublayer.content_visual.Get(), FALSE, nullptr);
            shown_sublayer.clip_visual->AddVisual(shown_sublayer.rounded_clip_visual.Get(), FALSE, nullptr);
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
        shown_sublayer.clip_visual->RemoveAllVisuals();
        shown_sublayer.rounded_clip_visual->RemoveAllVisuals();
        shown_sublayer.clip_visual->AddVisual(shown_sublayer.content_visual.Get(), FALSE, nullptr);
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
    if (!shown_sublayer.swap_chain) {
        shown_sublayer.swap_chain = TRY(create_swap_chain(surface_size, DXGI_ALPHA_MODE_PREMULTIPLIED));
        content_visual.SetContent(shown_sublayer.swap_chain->content());
    }
    TRY(shown_sublayer.swap_chain->draw(surface.painting_surface(), surface_size));
    shown_sublayer.content = move(*lease);
    m_has_changes_to_show = true;
    return {};
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
    if (auto result = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&device)); FAILED(result)) {
        dbgln("Compositor: Unable to create a DirectComposition device: {}", Error::from_windows_error(result));
        return nullptr;
    }

    auto impl = make<Impl>(skia_backend_context.release_nonnull(), move(device), move(dxgi_factory));
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

static ErrorOr<NonnullRefPtr<PresentableSurface>> create_presentable_surface(Gfx::IntSize size, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto painting_surface = TRY(Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context));
    return PresentableSurface::create(move(painting_surface), Gfx::PaintingSurface::Origin::TopLeft, make<DirectCompositionSurface>());
}

ErrorOr<NonnullRefPtr<PresentableSurface>> DirectCompositionWindowServer::try_create_presentable_surface_on_gpu(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    return create_presentable_surface(size, skia_backend_context);
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
