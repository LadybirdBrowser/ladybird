/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Direct3D 11 device that opens the surfaces that DirectComposition reads as they are, shared by every window
//! server: DirectComposition makes composition textures only of Direct3D 11 textures, while Skia paints with Direct3D
//! 12.

use std::cell::OnceCell;
use windows::Win32::Foundation::HMODULE;
use windows::Win32::Graphics::Direct3D::{
    D3D_DRIVER_TYPE_UNKNOWN, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_12_1,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_SDK_VERSION, D3D11CreateDevice, ID3D11Device, ID3D11Device1,
};
use windows::Win32::Graphics::DirectComposition::{
    DCompositionCreateDevice3, IDCompositionDesktopDevice, IDCompositionDevice4,
};
use windows::Win32::Graphics::Dxgi::IDXGIAdapter1;
use windows::core::{IUnknown, Interface};

thread_local! {
    static D3D11_DEVICE_FOR_COMPOSITION_TEXTURES: OnceCell<Option<ID3D11Device1>> = const { OnceCell::new() };
}

fn create_d3d11_device_for_composition_textures(adapter: &IDXGIAdapter1) -> Option<ID3D11Device1> {
    // The device opens the textures that the main thread's Skia context paints, so it is created on the same adapter.
    let feature_levels = [
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    ];
    let mut device: Option<ID3D11Device> = None;
    // SAFETY: The adapter is live, and the device is written on success.
    let created = unsafe {
        D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            HMODULE::default(),
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            Some(&feature_levels),
            D3D11_SDK_VERSION,
            Some(&raw mut device),
            None,
            None,
        )
    };
    if let Err(error) = created {
        eprintln!("Compositor: Unable to create a Direct3D 11 device for composition textures: {error}");
        return None;
    }
    let device = device?;
    let device1 = match device.cast::<ID3D11Device1>() {
        Ok(device1) => device1,
        Err(error) => {
            eprintln!("Compositor: Unable to reach the Direct3D 11.1 interface of a device: {error}");
            return None;
        }
    };

    // SAFETY: A device without a rendering device of its own is asked for.
    let composition_device =
        match unsafe { DCompositionCreateDevice3::<_, IDCompositionDesktopDevice>(None::<&IUnknown>) } {
            Ok(composition_device) => composition_device,
            Err(error) => {
                eprintln!(
                    "Compositor: Unable to create a DirectComposition device to check for composition textures: {error}"
                );
                return None;
            }
        };
    // Only a device from DCompositionCreateDevice3 has IDCompositionDevice4; one from DCompositionCreateDevice2 answers
    // E_NOINTERFACE.
    let composition_device4 = match composition_device.cast::<IDCompositionDevice4>() {
        Ok(composition_device4) => composition_device4,
        Err(error) => {
            eprintln!("Compositor: This DirectComposition has no composition textures: {error}");
            return None;
        }
    };
    // SAFETY: The device is live.
    match unsafe { composition_device4.CheckCompositionTextureSupport(&device) } {
        Ok(supported) if supported.as_bool() => Some(device1),
        Ok(_) => {
            eprintln!("Compositor: DirectComposition shows no composition textures of this GPU");
            None
        }
        Err(error) => {
            eprintln!("Compositor: Unable to check for composition textures: {error}");
            None
        }
    }
}

/// Makes the device on the adapter, where it was not made yet, and answers whether the window server can show surfaces
/// shared with it as they are.
pub(super) fn make_d3d11_device_for_composition_textures(adapter: &IDXGIAdapter1) -> bool {
    D3D11_DEVICE_FOR_COMPOSITION_TEXTURES.with(|device| {
        device
            .get_or_init(|| create_d3d11_device_for_composition_textures(adapter))
            .is_some()
    })
}

/// The device, or none where it was not made, or where the window server cannot read surfaces as they are.
pub(super) fn d3d11_device_for_composition_textures() -> Option<ID3D11Device1> {
    D3D11_DEVICE_FOR_COMPOSITION_TEXTURES.with(|device| device.get().cloned().flatten())
}
