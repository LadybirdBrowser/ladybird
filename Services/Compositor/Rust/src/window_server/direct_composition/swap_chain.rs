/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A swap chain that a surface the window server cannot read as it is gets copied into, on the GPU queue it was painted
//! on, for a visual to show.

use super::Shared;
use crate::presenting::rect_of_size;
use crate::raster_host::{RasterHost, SurfaceBlend, SurfaceSampling};
use crate::surfaces::painting_surface::PaintingSurface;
use libgfx_rust::IntSize;
use std::rc::Rc;
use windows::Win32::Graphics::Direct3D12::ID3D12Resource;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_ALPHA_MODE, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SAMPLE_DESC};
use windows::Win32::Graphics::Dxgi::{
    DXGI_PRESENT, DXGI_SCALING_STRETCH, DXGI_SWAP_CHAIN_DESC1, DXGI_SWAP_CHAIN_FLAG, DXGI_SWAP_EFFECT_FLIP_DISCARD,
    DXGI_USAGE_RENDER_TARGET_OUTPUT, IDXGISwapChain3,
};
use windows::core::{IUnknown, Interface};

const BUFFER_COUNT: usize = 3;
const BUFFER_GROWTH_STEP: i32 = 256;

fn grown_buffer_extent(needed: i32, current: i32) -> i32 {
    if needed <= current {
        return current;
    }
    (needed + BUFFER_GROWTH_STEP - 1) / BUFFER_GROWTH_STEP * BUFFER_GROWTH_STEP
}

pub(super) struct CompositionSwapChain {
    raster: RasterHost,
    swap_chain: IDXGISwapChain3,
    buffer_size: IntSize,
    buffer_surfaces: [Option<Rc<PaintingSurface>>; BUFFER_COUNT],
    has_drawing_to_show: bool,
}

impl CompositionSwapChain {
    pub(super) fn create(shared: &Shared, size: IntSize, alpha_mode: DXGI_ALPHA_MODE) -> windows::core::Result<Self> {
        let description = DXGI_SWAP_CHAIN_DESC1 {
            Width: size.width.max(1) as u32,
            Height: size.height.max(1) as u32,
            Format: DXGI_FORMAT_R8G8B8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
            BufferCount: BUFFER_COUNT as u32,
            Scaling: DXGI_SCALING_STRETCH,
            SwapEffect: DXGI_SWAP_EFFECT_FLIP_DISCARD,
            AlphaMode: alpha_mode,
            ..Default::default()
        };
        // SAFETY: The queue and the factory are live, and the description is complete.
        let swap_chain = unsafe {
            shared
                .dxgi_factory
                .CreateSwapChainForComposition(&shared.gpu.queue, &raw const description, None)
        }?;
        Ok(Self {
            raster: shared.raster.clone(),
            swap_chain: swap_chain.cast()?,
            buffer_size: size,
            buffer_surfaces: Default::default(),
            has_drawing_to_show: false,
        })
    }

    /// What a visual shows the swap chain by.
    pub(super) fn content(&self) -> IUnknown {
        self.swap_chain.cast().expect("a swap chain is an object")
    }

    fn make_buffers_fit(&mut self, size: IntSize) -> windows::core::Result<()> {
        let fits = size.width <= self.buffer_size.width && size.height <= self.buffer_size.height;
        let is_far_too_large = i64::from(size.width) * i64::from(size.height) * 4
            < i64::from(self.buffer_size.width) * i64::from(self.buffer_size.height);
        if fits && !is_far_too_large {
            return Ok(());
        }
        let buffer_size = if is_far_too_large {
            size
        } else {
            IntSize {
                width: grown_buffer_extent(size.width, self.buffer_size.width),
                height: grown_buffer_extent(size.height, self.buffer_size.height),
            }
        };
        // The buffers can be resized only once nothing refers to them and the GPU is done with them.
        self.buffer_surfaces = Default::default();
        self.raster.finish_gpu_work();
        // SAFETY: Nothing refers to the buffers any longer.
        unsafe {
            self.swap_chain.ResizeBuffers(
                BUFFER_COUNT as u32,
                buffer_size.width as u32,
                buffer_size.height as u32,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_SWAP_CHAIN_FLAG(0),
            )
        }?;
        self.buffer_size = buffer_size;
        self.has_drawing_to_show = false;
        Ok(())
    }

    fn surface_of_buffer(&mut self, index: usize) -> windows::core::Result<Rc<PaintingSurface>> {
        if let Some(surface) = &self.buffer_surfaces[index] {
            return Ok(surface.clone());
        }
        // SAFETY: The swap chain has a buffer of the index.
        let buffer: ID3D12Resource = unsafe { self.swap_chain.GetBuffer(index as u32) }?;
        // SAFETY: The buffer is a live resource of the size of the buffers.
        let surface = unsafe {
            self.raster
                .wrap_d3d12_resource_as_surface(buffer.as_raw(), self.buffer_size)
        }
        .ok_or_else(|| {
            windows::core::Error::new(
                windows::Win32::Foundation::E_FAIL,
                "Failed to wrap a buffer of a swap chain in a surface",
            )
        })?;
        self.buffer_surfaces[index] = Some(surface.clone());
        Ok(surface)
    }

    /// Copies `size` of the top left of the source into the next buffer, which `show_drawing` presents.
    pub(super) fn draw(&mut self, source: &Rc<PaintingSurface>, size: IntSize) -> windows::core::Result<()> {
        self.make_buffers_fit(size)?;
        // SAFETY: The swap chain is live.
        let index = unsafe { self.swap_chain.GetCurrentBackBufferIndex() } as usize;
        let target = self.surface_of_buffer(index)?;
        let rect = rect_of_size(size);
        if !self.raster.draw_surface(
            &target,
            source,
            rect,
            rect,
            SurfaceBlend::Source,
            SurfaceSampling::Nearest,
        ) {
            return Err(windows::core::Error::new(
                windows::Win32::Foundation::E_FAIL,
                "Failed to take a snapshot of a surface to show",
            ));
        }
        self.raster.flush_surface_for_presentation(&target);
        self.has_drawing_to_show = true;
        Ok(())
    }

    pub(super) fn show_drawing(&mut self) {
        if !std::mem::take(&mut self.has_drawing_to_show) {
            return;
        }
        // SAFETY: The swap chain is live, and its current buffer was drawn into.
        let result = unsafe { self.swap_chain.Present(0, DXGI_PRESENT(0)) };
        if result.is_err() {
            eprintln!(
                "Compositor: Failed to present a swap chain of a remote layer: {}",
                windows::core::Error::from(result)
            );
        }
    }
}
