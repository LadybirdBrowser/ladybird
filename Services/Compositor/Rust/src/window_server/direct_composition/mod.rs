/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The window server of Windows, driven through DirectComposition: each remote layer is a child window that a tree of
//! visuals targets, which the presentation takes as a child of its own window.

mod child_windows;
mod device;
mod remote_layer;
mod surface;
mod swap_chain;

use super::{RemoteLayer, WindowServer};
use crate::raster_host::{FrameHandOff, RasterHost};
use libcompositing_rust::display_list::layers::WindowServerCompositing;
use libgfx_rust::Color;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_void;
use std::rc::{Rc, Weak};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_RENDER_TARGET, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED,
    D3D11_RESOURCE_MISC_SHARED_NTHANDLE, D3D11_SUBRESOURCE_DATA, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT,
    ID3D11Texture2D,
};
use windows::Win32::Graphics::Direct3D12::ID3D12CommandQueue;
use windows::Win32::Graphics::DirectComposition::{
    DCompositionCreateDevice2, DCompositionCreateDevice3, IDCompositionDesktopDevice, IDCompositionDevice4,
    IDCompositionTexture,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_ALPHA_MODE_PREMULTIPLIED, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{IDXGIAdapter1, IDXGIFactory2};
use windows::core::{IUnknown, Interface};

pub use surface::SharedTexture;

/// The GPU that the main thread's Skia context paints with on Direct3D 12.
pub(super) struct GpuDevice {
    pub adapter: IDXGIAdapter1,
    pub queue: ID3D12CommandQueue,
    /// The Skia backend context, which tells the surfaces it paints apart from others.
    pub backend_context: *const c_void,
}

const MAXIMUM_COUNT_OF_COMPOSITION_TEXTURES_FILLED_WITH_COLORS: usize = 256;

/// What a window server and its remote layers share.
pub(super) struct Shared {
    pub raster: RasterHost,
    pub gpu: GpuDevice,
    pub device: IDCompositionDesktopDevice,
    /// None where the window server cannot show surfaces as they are, which it then shows by copying them into swap
    /// chains.
    pub device_showing_composition_textures: Option<IDCompositionDevice4>,
    pub dxgi_factory: IDXGIFactory2,
    /// Tells apart the window servers that the composition textures of a surface belong to, as each belongs to the
    /// device that made it.
    pub id: u64,
    composition_textures_filled_with_colors: RefCell<HashMap<Color, IDCompositionTexture>>,
    layers: RefCell<Vec<Weak<RefCell<remote_layer::LayerState>>>>,
    holds_changes_off_screen: Cell<bool>,
}

impl Shared {
    pub(super) fn holds_changes_off_screen(&self) -> bool {
        self.holds_changes_off_screen.get()
    }

    pub(super) fn commit(&self) {
        if self.holds_changes_off_screen.get() {
            return;
        }
        // SAFETY: The device is live.
        if let Err(error) = unsafe { self.device.Commit() } {
            eprintln!("Compositor: Unable to commit the visuals of remote layers: {error}");
        }
    }

    /// A texture of one pixel of the color, which a visual scales up into a fill.
    pub(super) fn composition_texture_filled_with(&self, color: Color) -> Option<IDCompositionTexture> {
        let device = self.device_showing_composition_textures.as_ref()?;
        if let Some(texture) = self.composition_textures_filled_with_colors.borrow().get(&color) {
            return Some(texture.clone());
        }
        let texture = match create_composition_texture_filled_with(device, color) {
            Ok(texture) => texture,
            Err(error) => {
                eprintln!("Compositor: Unable to create a composition texture of a color: {error}");
                return None;
            }
        };
        let mut textures = self.composition_textures_filled_with_colors.borrow_mut();
        if textures.len() >= MAXIMUM_COUNT_OF_COMPOSITION_TEXTURES_FILLED_WITH_COLORS {
            textures.clear();
        }
        textures.insert(color, texture.clone());
        Some(texture)
    }
}

fn create_composition_texture_filled_with(
    device: &IDCompositionDevice4,
    color: Color,
) -> windows::core::Result<IDCompositionTexture> {
    let d3d11_device = device::d3d11_device_for_composition_textures()
        .ok_or_else(|| windows::core::Error::from(windows::Win32::Foundation::E_FAIL))?;
    let description = D3D11_TEXTURE2D_DESC {
        Width: 1,
        Height: 1,
        MipLevels: 1,
        ArraySize: 1,
        Format: DXGI_FORMAT_R8G8B8A8_UNORM,
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        Usage: D3D11_USAGE_DEFAULT,
        BindFlags: (D3D11_BIND_RENDER_TARGET.0 | D3D11_BIND_SHADER_RESOURCE.0) as u32,
        CPUAccessFlags: 0,
        // A texture that cannot be shared is not taken as a composition texture.
        MiscFlags: (D3D11_RESOURCE_MISC_SHARED.0 | D3D11_RESOURCE_MISC_SHARED_NTHANDLE.0) as u32,
    };
    let premultiplied = |channel: u8| ((u32::from(channel) * u32::from(color.alpha()) + 127) / 255) as u8;
    let pixel = [
        premultiplied(color.red()),
        premultiplied(color.green()),
        premultiplied(color.blue()),
        color.alpha(),
    ];
    let initial_data = D3D11_SUBRESOURCE_DATA {
        pSysMem: pixel.as_ptr().cast(),
        SysMemPitch: pixel.len() as u32,
        SysMemSlicePitch: 0,
    };
    let mut texture: Option<ID3D11Texture2D> = None;
    // SAFETY: The description and the initial data are complete, and the texture is written on success.
    unsafe {
        d3d11_device.CreateTexture2D(
            &raw const description,
            Some(&raw const initial_data),
            Some(&raw mut texture),
        )
    }?;
    // Another device sees what this one wrote into a shared texture only once its work is flushed.
    // SAFETY: The device is live.
    unsafe { d3d11_device.GetImmediateContext()?.Flush() };
    let texture = texture.ok_or_else(|| windows::core::Error::from(windows::Win32::Foundation::E_FAIL))?;
    // SAFETY: The texture is live, and made on the device the composition textures are checked for.
    let composition_texture = unsafe { device.CreateCompositionTexture(&texture) }?;
    // SAFETY: The texture is live.
    unsafe { composition_texture.SetAlphaMode(DXGI_ALPHA_MODE_PREMULTIPLIED) }?;
    Ok(composition_texture)
}

pub(super) struct DirectCompositionWindowServer {
    shared: Rc<Shared>,
}

impl DirectCompositionWindowServer {
    /// None where the main thread's Skia context does not paint with Direct3D 12.
    pub(super) fn create(raster: &RasterHost) -> Option<Rc<dyn WindowServer>> {
        static NEXT_WINDOW_SERVER_ID: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

        let gpu = raster.window_server_gpu_device()?;
        // SAFETY: The host hands out live objects, which live for as long as the process.
        let gpu = unsafe {
            GpuDevice {
                adapter: IDXGIAdapter1::from_raw_borrowed(&gpu.adapter)?.clone(),
                queue: ID3D12CommandQueue::from_raw_borrowed(&gpu.queue)?.clone(),
                backend_context: gpu.backend_context,
            }
        };
        // SAFETY: The adapter is live.
        let dxgi_factory = match unsafe { gpu.adapter.GetParent::<IDXGIFactory2>() } {
            Ok(factory) => factory,
            Err(error) => {
                eprintln!("Compositor: Unable to reach the DXGI factory of the GPU: {error}");
                return None;
            }
        };
        let mut device = None;
        let mut device_showing_composition_textures = None;
        if device::make_d3d11_device_for_composition_textures(&gpu.adapter) {
            // SAFETY: A device without a rendering device of its own is asked for.
            let created = unsafe { DCompositionCreateDevice3::<_, IDCompositionDesktopDevice>(None::<&IUnknown>) }
                .and_then(|created| Ok((created.cast::<IDCompositionDevice4>()?, created)));
            match created {
                Ok((device4, created)) => {
                    device_showing_composition_textures = Some(device4);
                    device = Some(created);
                }
                Err(error) => eprintln!(
                    "Compositor: Unable to create a DirectComposition device that shows composition textures: {error}"
                ),
            }
        }
        let device = match device {
            Some(device) => device,
            // SAFETY: A device without a rendering device of its own is asked for.
            None => match unsafe { DCompositionCreateDevice2::<_, IDCompositionDesktopDevice>(None::<&IUnknown>) } {
                Ok(device) => device,
                Err(error) => {
                    eprintln!("Compositor: Unable to create a DirectComposition device: {error}");
                    return None;
                }
            },
        };
        Some(Rc::new(Self {
            shared: Rc::new(Shared {
                raster: raster.clone(),
                gpu,
                device,
                device_showing_composition_textures,
                dxgi_factory,
                id: NEXT_WINDOW_SERVER_ID.fetch_add(1, std::sync::atomic::Ordering::Relaxed),
                composition_textures_filled_with_colors: RefCell::new(HashMap::new()),
                layers: RefCell::new(Vec::new()),
                holds_changes_off_screen: Cell::new(false),
            }),
        }))
    }
}

impl WindowServer for DirectCompositionWindowServer {
    fn compositing(&self) -> WindowServerCompositing {
        WindowServerCompositing {
            shows_tiles_of_painted_layers: self.shared.device_showing_composition_textures.is_some(),
            // DirectComposition has no backdrop filters, so those effects stay with the compositor.
            filters_what_is_below_layers: false,
        }
    }

    /// The window server reads a composition texture in no order with the GPU work that drew into it, unlike a swap
    /// chain, which this process copies surfaces into on its own queue.
    fn frame_hand_off(&self) -> FrameHandOff {
        if self.shared.device_showing_composition_textures.is_some() {
            FrameHandOff::OnceGpuWorkIsFinished
        } else {
            FrameHandOff::OnceGpuWorkIsScheduled
        }
    }

    fn create_remote_layer(&self) -> Option<Rc<dyn RemoteLayer>> {
        let layer = remote_layer::DirectCompositionRemoteLayer::create(&self.shared)?;
        let mut layers = self.shared.layers.borrow_mut();
        layers.retain(|layer| layer.strong_count() > 0);
        layers.push(layer.state_reference());
        drop(layers);
        self.shared.commit();
        Some(Rc::new(layer))
    }

    fn hold_changes_on_screen(&self) {
        self.shared.holds_changes_off_screen.set(true);
    }

    fn release_changes_held_off_screen(&self) {
        self.shared.holds_changes_off_screen.set(false);
        let layers: Vec<_> = self.shared.layers.borrow().iter().filter_map(Weak::upgrade).collect();
        self.shared.layers.borrow_mut().retain(|layer| layer.strong_count() > 0);
        for layer in layers {
            layer.borrow_mut().show_changes();
        }
        self.shared.commit();
    }
}

/// Whether the window server can show the surfaces shared with it as they are, which are then textures shared between
/// the Skia context on the adapter and the window server.
///
/// # Safety
///
/// `adapter` must be a live `IDXGIAdapter1`.
pub unsafe fn can_show_surfaces_shared_with_it(adapter: *mut c_void) -> bool {
    // SAFETY: The caller passes a live adapter.
    let Some(adapter) = (unsafe { IDXGIAdapter1::from_raw_borrowed(&adapter) }) else {
        return false;
    };
    device::make_d3d11_device_for_composition_textures(adapter)
}
