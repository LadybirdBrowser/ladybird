/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A surface that DirectComposition shows as it is, as the content of a visual: a Direct3D 12 texture shared with a
//! Direct3D 11 device, which a composition texture of each window server that shows it is made of.

use super::device::d3d11_device_for_composition_textures;
use crate::cpp_object::CppObject;
use libgfx_rust::IntRect;
use std::cell::RefCell;
use std::ffi::c_void;
use windows::Win32::Foundation::HANDLE;
use windows::Win32::Graphics::Direct2D::Common::D2D_RECT_U;
use windows::Win32::Graphics::Direct3D11::{ID3D11Fence, ID3D11Texture2D};
use windows::Win32::Graphics::DirectComposition::{IDCompositionDevice4, IDCompositionTexture};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_ALPHA_MODE, DXGI_ALPHA_MODE_UNSPECIFIED};

struct CompositionTexture {
    window_server_id: u64,
    texture: IDCompositionTexture,
    alpha_mode: DXGI_ALPHA_MODE,
    source_rect: Option<IntRect>,
}

pub struct SharedTexture {
    shared_handle: HANDLE,
    // The `Gfx::D3DSharedTexture` that owns the texture and its handle.
    _owner: CppObject,
    texture_opened_on_d3d11_device: RefCell<Option<ID3D11Texture2D>>,
    // One per window server that showed the surface.
    composition_textures: RefCell<Vec<CompositionTexture>>,
}

impl SharedTexture {
    /// # Safety
    ///
    /// `shared_handle` must be the NT handle of the texture that `owner` owns, which lives for as long as it does.
    pub unsafe fn new(shared_handle: *mut c_void, owner: CppObject) -> Self {
        Self {
            shared_handle: HANDLE(shared_handle),
            _owner: owner,
            texture_opened_on_d3d11_device: RefCell::new(None),
            composition_textures: RefCell::new(Vec::new()),
        }
    }

    /// The composition texture that the window server shows `source_rect` of the surface by, made the first time it
    /// asks for one.
    pub(super) fn composition_texture_shown_by(
        &self,
        window_server_id: u64,
        device: &IDCompositionDevice4,
        alpha_mode: DXGI_ALPHA_MODE,
        source_rect: IntRect,
    ) -> Option<IDCompositionTexture> {
        let mut composition_textures = self.composition_textures.borrow_mut();
        let index = match composition_textures
            .iter()
            .position(|texture| texture.window_server_id == window_server_id)
        {
            Some(index) => index,
            None => {
                let mut opened = self.texture_opened_on_d3d11_device.borrow_mut();
                if opened.is_none() {
                    let d3d11_device = d3d11_device_for_composition_textures()?;
                    // SAFETY: The handle is the NT handle of a live shared texture.
                    match unsafe { d3d11_device.OpenSharedResource1::<ID3D11Texture2D>(self.shared_handle) } {
                        Ok(texture) => *opened = Some(texture),
                        Err(error) => {
                            eprintln!("Compositor: Unable to open a surface on the Direct3D 11 device: {error}");
                            return None;
                        }
                    }
                }
                let opened = opened.as_ref()?;
                // A composition texture made of the Direct3D 12 resource itself shows nothing, so it is made of the
                // texture that the Direct3D 11 device opened.
                // SAFETY: The texture is live, and was opened on the device the composition textures are checked for.
                let texture = match unsafe { device.CreateCompositionTexture(opened) } {
                    Ok(texture) => texture,
                    Err(error) => {
                        eprintln!("Compositor: Unable to create a composition texture of a surface: {error}");
                        return None;
                    }
                };
                composition_textures.push(CompositionTexture {
                    window_server_id,
                    texture,
                    alpha_mode: DXGI_ALPHA_MODE_UNSPECIFIED,
                    source_rect: None,
                });
                composition_textures.len() - 1
            }
        };
        let composition_texture = &mut composition_textures[index];
        if composition_texture.alpha_mode != alpha_mode {
            // SAFETY: The texture is live.
            match unsafe { composition_texture.texture.SetAlphaMode(alpha_mode) } {
                Ok(()) => composition_texture.alpha_mode = alpha_mode,
                Err(error) => eprintln!("Compositor: Unable to set the alpha mode of a composition texture: {error}"),
            }
        }
        if composition_texture.source_rect != Some(source_rect) {
            let rect = D2D_RECT_U {
                left: u32::try_from(source_rect.x).unwrap_or(0),
                top: u32::try_from(source_rect.y).unwrap_or(0),
                right: u32::try_from(source_rect.right()).unwrap_or(0),
                bottom: u32::try_from(source_rect.bottom()).unwrap_or(0),
            };
            // SAFETY: The texture is live, and reads the rect during the call.
            match unsafe { composition_texture.texture.SetSourceRect(&raw const rect) } {
                Ok(()) => composition_texture.source_rect = Some(source_rect),
                Err(error) => eprintln!("Compositor: Unable to set the source rect of a composition texture: {error}"),
            }
        }
        Some(composition_texture.texture.clone())
    }

    /// A composition texture has no fence for as long as any visual has it as its content, even a visual that is no
    /// longer in the tree. Once none does, and that is committed, it has a fence that reaches the value it gives once
    /// the window server has stopped reading it.
    pub fn may_be_read_by_window_server(&self) -> bool {
        self.composition_textures.borrow().iter().any(|composition_texture| {
            let mut value_once_available = 0;
            // SAFETY: The texture is live, and writes the value its fence reaches once it is available.
            let fence = unsafe {
                composition_texture
                    .texture
                    .GetAvailableFence::<ID3D11Fence>(&raw mut value_once_available)
            };
            let Ok(fence) = fence else {
                return true;
            };
            // SAFETY: The fence is live.
            let completed_value = unsafe { fence.GetCompletedValue() };
            completed_value < value_once_available
        })
    }
}
