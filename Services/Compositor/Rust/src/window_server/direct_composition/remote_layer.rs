/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A tree of DirectComposition visuals that this process commits frames into, which targets a child window that the
//! presentation shows as a child of its own.

use super::Shared;
use super::child_windows::{create_child_window, destroy_child_window};
use super::swap_chain::CompositionSwapChain;
use crate::presenting::{rect_of_size, size_of};
use crate::surfaces::presentable_surface::{PresentableSurface, PresentableSurfaceLease};
use crate::window_server::shown_sublayers::{NativeSublayers, ShownSublayers, SublayerKind};
use crate::window_server::{
    Frame, Group, MediaToShow, PlacedContent, RemoteLayer, SublayerContent, rect_fitting_frame,
};
use libcompositing_rust::display_list::window_server_placement::WindowServerPlacement;
use libcompositing_rust::ipc_types::{Compositing_RemoteLayerHostHandle, Compositing_RemoteLayerHostKind};
use libgfx_rust::{AffineTransform, Color, CornerRadii, CornerRadius, FloatRect, IntRect, IntSize, ScalingMode};
use std::cell::RefCell;
use std::rc::{Rc, Weak};
use windows::Win32::Foundation::{GetLastError, HWND};
use windows::Win32::Graphics::Direct2D::Common::D2D_RECT_F;
use windows::Win32::Graphics::DirectComposition::{
    DCOMPOSITION_BITMAP_INTERPOLATION_MODE, DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR,
    DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, DCOMPOSITION_BORDER_MODE_HARD,
    DCOMPOSITION_BORDER_MODE_SOFT, DCOMPOSITION_OPACITY_MODE_LAYER, DCOMPOSITION_OPACITY_MODE_MULTIPLY,
    IDCompositionClip, IDCompositionRectangleClip, IDCompositionTarget, IDCompositionTexture, IDCompositionVisual,
    IDCompositionVisual2, IDCompositionVisual3,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_ALPHA_MODE, DXGI_ALPHA_MODE_IGNORE, DXGI_ALPHA_MODE_PREMULTIPLIED};
use windows::Win32::UI::HiDpi::{DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext};
use windows::Win32::UI::WindowsAndMessaging::{
    SWP_NOACTIVATE, SWP_NOCOPYBITS, SWP_NOMOVE, SWP_NOOWNERZORDER, SWP_NOREDRAW, SWP_NOSENDCHANGING, SWP_NOZORDER,
    SetWindowPos,
};
use windows::core::{IUnknown, Interface};
use windows_numerics::Matrix3x2;

fn matrix_of(transform: AffineTransform) -> Matrix3x2 {
    let [a, b, c, d, e, f] = transform.values;
    Matrix3x2 {
        M11: a,
        M12: b,
        M21: c,
        M22: d,
        M31: e,
        M32: f,
    }
}

/// A visual shows its content at the content's own size from its origin, so content of another size than the rect is
/// scaled into it.
fn matrix_placing_content_of_size_at(content_size: IntSize, rect: IntRect) -> Matrix3x2 {
    Matrix3x2 {
        M11: rect.width as f32 / content_size.width as f32,
        M12: 0.0,
        M21: 0.0,
        M22: rect.height as f32 / content_size.height as f32,
        M31: rect.x as f32,
        M32: rect.y as f32,
    }
}

fn set_rectangle(clip: &IDCompositionRectangleClip, rect: FloatRect) {
    // SAFETY: The clip is live.
    unsafe {
        let _ = clip.SetLeft2(rect.x);
        let _ = clip.SetTop2(rect.y);
        let _ = clip.SetRight2(rect.x + rect.width);
        let _ = clip.SetBottom2(rect.y + rect.height);
    }
}

fn set_corner_radii(clip: &IDCompositionRectangleClip, corner_radii: &CornerRadii) {
    let radii_of = |corner: CornerRadius| {
        if corner.is_present() {
            (corner.horizontal_radius as f32, corner.vertical_radius as f32)
        } else {
            (0.0, 0.0)
        }
    };
    set_horizontal_and_vertical_radii_of_corners(
        clip,
        [
            corner_radii.top_left,
            corner_radii.top_right,
            corner_radii.bottom_right,
            corner_radii.bottom_left,
        ]
        .map(radii_of),
    );
}

/// Sets the horizontal and vertical radii of the clip's corners, given top left, top right, bottom right and bottom
/// left.
fn set_horizontal_and_vertical_radii_of_corners(clip: &IDCompositionRectangleClip, radii: [(f32, f32); 4]) {
    let [
        (top_left_x, top_left_y),
        (top_right_x, top_right_y),
        (bottom_right_x, bottom_right_y),
        (bottom_left_x, bottom_left_y),
    ] = radii;
    // SAFETY: The clip is live.
    unsafe {
        let _ = clip.SetTopLeftRadiusX2(top_left_x);
        let _ = clip.SetTopLeftRadiusY2(top_left_y);
        let _ = clip.SetTopRightRadiusX2(top_right_x);
        let _ = clip.SetTopRightRadiusY2(top_right_y);
        let _ = clip.SetBottomRightRadiusX2(bottom_right_x);
        let _ = clip.SetBottomRightRadiusY2(bottom_right_y);
        let _ = clip.SetBottomLeftRadiusX2(bottom_left_x);
        let _ = clip.SetBottomLeftRadiusY2(bottom_left_y);
    }
}

fn interpolation_mode_for_scaling_mode(scaling_mode: ScalingMode) -> DCOMPOSITION_BITMAP_INTERPOLATION_MODE {
    match scaling_mode {
        ScalingMode::None | ScalingMode::NearestNeighbor => DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
        ScalingMode::Bilinear | ScalingMode::BilinearMipmap => DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR,
    }
}

/// The visuals a sublayer is shown with.
pub(super) struct Visuals {
    /// The visual among the children of the group the sublayer is in, or of the frame: the visual that clips placed
    /// content, or the one visual of a group, a tile, a color or a backdrop.
    outermost_visual: IDCompositionVisual2,
    /// The visual that shows the content: the innermost visual of placed content, or the one visual of a tile or a
    /// color.
    content_visual: Option<IDCompositionVisual2>,
    /// The clip of the outermost visual of placed content, or of a group that clips.
    clip: Option<IDCompositionRectangleClip>,
    /// Of placed content with a rounded clip, the visual between the outermost and the content visual, and its clip.
    rounded_clip_visual_and_clip: Option<(IDCompositionVisual2, IDCompositionRectangleClip)>,
    swap_chain: Option<CompositionSwapChain>,
    content: Option<PresentableSurfaceLease>,
    content_visual_shows_composition_texture_of_surface: bool,
    /// What a group, a tile or a color was last shown with, so that a frame sets only what it changes.
    shown_group: Option<Group>,
    shown_rect: Option<IntRect>,
    shown_color: Option<Color>,
}

/// The visuals of the frame, and what the sublayers of the frame are shown with.
pub(super) struct Natives {
    shared: Rc<Shared>,
    frame_visual: IDCompositionVisual2,
    has_changes_to_show: bool,
}

pub(super) struct LayerState {
    natives: Natives,
    window: HWND,
    target: Option<IDCompositionTarget>,
    // The visual that the target shows, which holds the frame's visual.
    _root_visual: IDCompositionVisual2,
    frame_swap_chain: Option<CompositionSwapChain>,
    sublayers: ShownSublayers<Visuals>,
    painted_size: IntSize,
    window_size: IntSize,
    window_size_to_show: IntSize,
}

pub(super) struct DirectCompositionRemoteLayer {
    state: Rc<RefCell<LayerState>>,
}

impl DirectCompositionRemoteLayer {
    pub(super) fn create(shared: &Rc<Shared>) -> Option<Self> {
        let window = create_child_window()?;
        let created =
            (|| -> windows::core::Result<(IDCompositionTarget, IDCompositionVisual2, IDCompositionVisual2)> {
                // SAFETY: The device and the window are live.
                unsafe {
                    let target = shared.device.CreateTargetForHwnd(window, true)?;
                    let root_visual = shared.device.CreateVisual()?;
                    let frame_visual = shared.device.CreateVisual()?;
                    frame_visual.SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR)?;
                    root_visual.AddVisual(&frame_visual, false, None)?;
                    target.SetRoot(&root_visual)?;
                    Ok((target, root_visual, frame_visual))
                }
            })();
        let (target, root_visual, frame_visual) = match created {
            Ok(created) => created,
            Err(error) => {
                eprintln!("Compositor: Unable to target the window of a remote layer: {error}");
                destroy_child_window(window);
                return None;
            }
        };
        Some(Self {
            state: Rc::new(RefCell::new(LayerState {
                natives: Natives {
                    shared: shared.clone(),
                    frame_visual,
                    has_changes_to_show: false,
                },
                window,
                target: Some(target),
                _root_visual: root_visual,
                frame_swap_chain: None,
                sublayers: ShownSublayers::default(),
                painted_size: IntSize::default(),
                window_size: IntSize::default(),
                window_size_to_show: IntSize::default(),
            })),
        })
    }

    pub(super) fn state_reference(&self) -> Weak<RefCell<LayerState>> {
        Rc::downgrade(&self.state)
    }
}

impl Drop for DirectCompositionRemoteLayer {
    fn drop(&mut self) {
        let mut state = self.state.borrow_mut();
        let state = &mut *state;
        state.sublayers.stop_showing_all(&mut state.natives);
        if let Some(target) = state.target.take() {
            // SAFETY: The target is live.
            let _ = unsafe { target.SetRoot(None) };
        }
        state.frame_swap_chain = None;
        state.natives.shared.commit();
        destroy_child_window(state.window);
    }
}

impl RemoteLayer for DirectCompositionRemoteLayer {
    fn host_handle(&self) -> Compositing_RemoteLayerHostHandle {
        Compositing_RemoteLayerHostHandle {
            kind: Compositing_RemoteLayerHostKind::Win32ChildWindow,
            id: self.state.borrow().window.0 as usize as u64,
        }
    }

    fn can_show(&self, placement: &WindowServerPlacement, media: MediaToShow<'_>) -> bool {
        if placement.path_clip.is_some() {
            return false;
        }
        match media {
            MediaToShow::Surface(surface) => {
                surface.can_be_shown_by_window_server()
                    && surface.painting_surface().gpu_backend_context()
                        == self.state.borrow().natives.shared.gpu.backend_context
            }
            MediaToShow::Video(_) => false,
        }
    }

    fn show_frame(&self, frame: Frame) -> bool {
        let mut state = self.state.borrow_mut();
        let state = &mut *state;
        if let Some(store) = &frame.store {
            let store = store.surface();
            let store_size = store.size();
            let painted_size = IntSize {
                width: frame.painted_size.width.min(store_size.width),
                height: frame.painted_size.height.min(store_size.height),
            };
            if painted_size.is_empty() {
                return true;
            }
            if state.frame_swap_chain.is_none() {
                match CompositionSwapChain::create(&state.natives.shared, painted_size, DXGI_ALPHA_MODE_IGNORE) {
                    Ok(swap_chain) => {
                        // SAFETY: The visual and the swap chain are live.
                        let _ = unsafe { state.natives.frame_visual.SetContent(&swap_chain.content()) };
                        state.frame_swap_chain = Some(swap_chain);
                    }
                    Err(error) => {
                        eprintln!("Compositor: Unable to create the swap chain of a remote layer: {error}");
                        state.stop_showing_frame();
                        return false;
                    }
                }
            }
            let swap_chain = state
                .frame_swap_chain
                .as_mut()
                .expect("a swap chain the frame is shown in");
            if let Err(error) = swap_chain.draw(store.painting_surface(), painted_size) {
                eprintln!("Compositor: Unable to show a frame in a remote layer: {error}");
                state.stop_showing_frame();
                return false;
            }
            state.painted_size = painted_size;
        } else if frame.shows_no_store {
            if frame.painted_size.is_empty() {
                return true;
            }
            if state.frame_swap_chain.is_some() {
                state.stop_showing_store();
            }
            state.painted_size = frame.painted_size;
        }
        if state.painted_size.is_empty() {
            return true;
        }
        state.lay_out_frame(frame.fit_within);
        state.sublayers.show(&mut state.natives, frame.sublayers);
        state.show_changes_unless_held();
        true
    }

    fn place_frame(&self, fit_within: Option<IntSize>) {
        let mut state = self.state.borrow_mut();
        if state.painted_size.is_empty() {
            return;
        }
        state.lay_out_frame(fit_within);
        state.show_changes_unless_held();
    }
}

impl LayerState {
    fn stop_showing_store(&mut self) {
        // SAFETY: The visual is live.
        let _ = unsafe { self.natives.frame_visual.SetContent(None) };
        self.frame_swap_chain = None;
    }

    fn stop_showing_frame(&mut self) {
        self.stop_showing_store();
        self.sublayers.stop_showing_all(&mut self.natives);
        self.painted_size = IntSize::default();
        self.natives.has_changes_to_show = true;
        self.show_changes_unless_held();
    }

    /// Lays out a frame of a painted size that is not empty.
    fn lay_out_frame(&mut self, fit_within: Option<IntSize>) {
        self.window_size_to_show = fit_within.unwrap_or(self.painted_size);
        let rect_of_frame_in_window = fit_within.map_or(rect_of_size(self.painted_size), |fit_within| {
            rect_fitting_frame(self.painted_size, fit_within)
        });
        let transform = matrix_placing_content_of_size_at(self.painted_size, rect_of_frame_in_window);
        let clip = D2D_RECT_F {
            left: 0.0,
            top: 0.0,
            right: self.painted_size.width as f32,
            bottom: self.painted_size.height as f32,
        };
        // SAFETY: The visual is live.
        unsafe {
            let _ = self.natives.frame_visual.SetTransform2(&raw const transform);
            let _ = self.natives.frame_visual.SetClip2(&raw const clip);
        }
        self.natives.has_changes_to_show = true;
    }

    fn show_changes_unless_held(&mut self) {
        if self.natives.shared.holds_changes_off_screen() {
            return;
        }
        self.show_changes();
        self.natives.shared.commit();
    }

    pub(super) fn show_changes(&mut self) {
        if !std::mem::take(&mut self.natives.has_changes_to_show) {
            return;
        }
        if self.window_size != self.window_size_to_show {
            // SAFETY: The window is live, and the DPI awareness of the thread is put back after.
            unsafe {
                let dpi_awareness = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                if SetWindowPos(
                    self.window,
                    None,
                    0,
                    0,
                    self.window_size_to_show.width,
                    self.window_size_to_show.height,
                    SWP_NOMOVE
                        | SWP_NOZORDER
                        | SWP_NOACTIVATE
                        | SWP_NOCOPYBITS
                        | SWP_NOREDRAW
                        | SWP_NOSENDCHANGING
                        | SWP_NOOWNERZORDER,
                )
                .is_err()
                {
                    eprintln!(
                        "Compositor: Unable to resize the window of a remote layer: error {}",
                        GetLastError().0
                    );
                }
                SetThreadDpiAwarenessContext(dpi_awareness);
            }
            self.window_size = self.window_size_to_show;
        }
        if let Some(swap_chain) = &mut self.frame_swap_chain {
            swap_chain.show_drawing();
        }
        self.sublayers.for_each_native_mut(|visuals| {
            if let Some(swap_chain) = &mut visuals.swap_chain {
                swap_chain.show_drawing();
            }
        });
    }
}

impl Natives {
    fn composition_texture_of(
        &self,
        surface: &PresentableSurface,
        alpha_mode: DXGI_ALPHA_MODE,
        source_rect: IntRect,
    ) -> Option<IDCompositionTexture> {
        let device = self.shared.device_showing_composition_textures.as_ref()?;
        surface
            .shared_texture()?
            .composition_texture_shown_by(self.shared.id, device, alpha_mode, source_rect)
    }

    fn create_visuals(&mut self, kind: SublayerKind) -> windows::core::Result<Visuals> {
        let device = &self.shared.device;
        // SAFETY: The device is live.
        let outermost_visual = unsafe { device.CreateVisual() }?;
        let mut visuals = Visuals {
            outermost_visual,
            content_visual: None,
            clip: None,
            rounded_clip_visual_and_clip: None,
            swap_chain: None,
            content: None,
            content_visual_shows_composition_texture_of_surface: false,
            shown_group: None,
            shown_rect: None,
            shown_color: None,
        };
        let outermost_visual = &visuals.outermost_visual;
        // SAFETY: The device and the visuals are live.
        unsafe {
            match kind {
                SublayerKind::PlacedContent => {
                    let clip = device.CreateRectangleClip()?;
                    let content_visual = device.CreateVisual()?;
                    outermost_visual.SetClip(&clip)?;
                    outermost_visual.AddVisual(&content_visual, false, None)?;
                    visuals.clip = Some(clip);
                    visuals.content_visual = Some(content_visual);
                }
                SublayerKind::Tile => {
                    outermost_visual.SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR)?;
                    outermost_visual.SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD)?;
                    visuals.content_visual = Some(outermost_visual.clone());
                }
                SublayerKind::Color => {
                    outermost_visual
                        .SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR)?;
                    outermost_visual.SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD)?;
                    visuals.content_visual = Some(outermost_visual.clone());
                }
                // A backdrop filter, which the window server does not apply here, so its visual shows nothing.
                SublayerKind::Group | SublayerKind::Backdrop => {}
            }
        }
        Ok(visuals)
    }

    fn update_placed_content(
        &mut self,
        visuals: &mut Visuals,
        content: PlacedContent,
        placement: &WindowServerPlacement,
    ) -> windows::core::Result<()> {
        let PlacedContent::Surface(lease) = content else {
            return Err(windows::core::Error::new(
                windows::Win32::Foundation::E_INVALIDARG,
                "Only surfaces can be shown as sublayers",
            ));
        };
        let device = self.shared.device.clone();
        let (Some(clip), Some(content_visual)) = (visuals.clip.clone(), visuals.content_visual.clone()) else {
            return Ok(());
        };
        set_rectangle(&clip, placement.clip_rect);

        // SAFETY: The device and the visuals are live.
        unsafe {
            if let Some(rounded_clip) = placement.rounded_clip {
                if visuals.rounded_clip_visual_and_clip.is_none() {
                    let rounded_clip_visual = device.CreateVisual()?;
                    let rectangle_clip = device.CreateRectangleClip()?;
                    rounded_clip_visual.SetClip(&rectangle_clip)?;
                    visuals.outermost_visual.RemoveAllVisuals()?;
                    rounded_clip_visual.AddVisual(&content_visual, false, None)?;
                    visuals.outermost_visual.AddVisual(&rounded_clip_visual, false, None)?;
                    visuals.rounded_clip_visual_and_clip = Some((rounded_clip_visual, rectangle_clip));
                }
                if let Some((_, rectangle_clip)) = &visuals.rounded_clip_visual_and_clip {
                    set_rectangle(rectangle_clip, rounded_clip.rect);
                    let corners = rounded_clip.rounded_corners;
                    set_horizontal_and_vertical_radii_of_corners(
                        rectangle_clip,
                        [
                            corners.top_left,
                            corners.top_right,
                            corners.bottom_right,
                            corners.bottom_left,
                        ]
                        .map(|rounds| {
                            let radius = if rounds { rounded_clip.radius } else { 0.0 };
                            (radius, radius)
                        }),
                    );
                }
            } else if let Some((rounded_clip_visual, _)) = visuals.rounded_clip_visual_and_clip.take() {
                visuals.outermost_visual.RemoveAllVisuals()?;
                rounded_clip_visual.RemoveAllVisuals()?;
                visuals.outermost_visual.AddVisual(&content_visual, false, None)?;
            }
        }

        let surface = lease.surface().clone();
        let surface_size = surface.size();
        if surface_size.is_empty() {
            return Ok(());
        }

        let content_rect = placement.content_rect;
        let transform = placement
            .content_to_viewport
            .translated(content_rect.x as f32, content_rect.y as f32)
            .scaled(
                content_rect.width as f32 / surface_size.width as f32,
                content_rect.height as f32 / surface_size.height as f32,
            );
        let content_to_viewport = placement.content_to_viewport;
        let border_mode = if content_to_viewport.b() == 0.0 && content_to_viewport.c() == 0.0 {
            DCOMPOSITION_BORDER_MODE_HARD
        } else {
            DCOMPOSITION_BORDER_MODE_SOFT
        };
        let surface_clip = D2D_RECT_F {
            left: 0.0,
            top: 0.0,
            right: surface_size.width as f32,
            bottom: surface_size.height as f32,
        };
        let matrix = matrix_of(transform);
        // SAFETY: The visual is live.
        unsafe {
            content_visual.SetTransform2(&raw const matrix)?;
            content_visual.SetClip2(&raw const surface_clip)?;
            content_visual.SetBitmapInterpolationMode(interpolation_mode_for_scaling_mode(placement.scaling_mode))?;
            content_visual.SetBorderMode(border_mode)?;
        }

        if visuals
            .content
            .as_ref()
            .is_some_and(|shown| Rc::ptr_eq(shown.surface(), &surface))
        {
            return Ok(());
        }
        if let Some(composition_texture) =
            self.composition_texture_of(&surface, DXGI_ALPHA_MODE_PREMULTIPLIED, rect_of_size(surface_size))
        {
            visuals.swap_chain = None;
            surface.did_hand_to_window_server();
            // SAFETY: The visual and the texture are live.
            unsafe { content_visual.SetContent(&composition_texture) }?;
            visuals.content_visual_shows_composition_texture_of_surface = true;
        } else {
            if visuals.swap_chain.is_none() {
                let swap_chain =
                    CompositionSwapChain::create(&self.shared, surface_size, DXGI_ALPHA_MODE_PREMULTIPLIED)?;
                // SAFETY: The visual and the swap chain are live.
                unsafe { content_visual.SetContent(&swap_chain.content()) }?;
                visuals.swap_chain = Some(swap_chain);
                visuals.content_visual_shows_composition_texture_of_surface = false;
            }
            visuals
                .swap_chain
                .as_mut()
                .expect("a swap chain the surface is copied into")
                .draw(surface.painting_surface(), surface_size)?;
            self.has_changes_to_show = true;
        }
        visuals.content = Some(lease);
        Ok(())
    }

    /// A group visual has the group's transform and clips to the group's rect in its own pixels, which a visual's clip
    /// is in. A frame that only moves the group, as a scroll does, sets nothing else.
    fn update_shown_group(&mut self, visuals: &mut Visuals, group: &Group) {
        let shown_group = visuals.shown_group;
        let group_visual = &visuals.outermost_visual;
        // SAFETY: The device and the visuals are live.
        unsafe {
            if shown_group.is_none_or(|shown_group| shown_group.to_parent.values != group.to_parent.values) {
                let matrix = matrix_of(group.to_parent);
                let _ = group_visual.SetTransform2(&raw const matrix);
            }
            if shown_group.is_none_or(|shown_group| {
                shown_group.clip_rect != group.clip_rect || shown_group.corner_radii != group.corner_radii
            }) {
                if let Some(clip_rect) = group.clip_rect {
                    if visuals.clip.is_none() {
                        match self.shared.device.CreateRectangleClip() {
                            Ok(clip) => {
                                let _ = group_visual.SetClip(&clip);
                                visuals.clip = Some(clip);
                            }
                            Err(error) => eprintln!("Compositor: Unable to create the clip of a group: {error}"),
                        }
                    }
                    if let Some(clip) = &visuals.clip {
                        set_rectangle(clip, clip_rect);
                        set_corner_radii(clip, &group.corner_radii);
                    }
                } else if visuals.clip.take().is_some() {
                    let _ = group_visual.SetClip(None::<&IDCompositionClip>);
                }
            }
            // Fading the sublayers of a group together takes a layer of their own, which only a translucent group asks
            // for.
            let shown_opacity = shown_group.map_or(1.0, |shown_group| shown_group.opacity);
            if shown_group.is_none() || (shown_opacity < 1.0) != (group.opacity < 1.0) {
                let _ = group_visual.SetOpacityMode(if group.opacity < 1.0 {
                    DCOMPOSITION_OPACITY_MODE_LAYER
                } else {
                    DCOMPOSITION_OPACITY_MODE_MULTIPLY
                });
            }
            if shown_opacity != group.opacity {
                match group_visual.cast::<IDCompositionVisual3>() {
                    Ok(group_visual3) => {
                        let _ = group_visual3.SetOpacity2(group.opacity);
                    }
                    Err(error) => eprintln!("Compositor: Unable to fade a group: {error}"),
                }
            }
        }
        visuals.shown_group = Some(*group);
    }

    fn update_shown_tile(
        &mut self,
        visuals: &mut Visuals,
        surface: PresentableSurfaceLease,
        rect: IntRect,
        rect_in_surface: IntRect,
        every_pixel_is_opaque: bool,
    ) {
        let tile_visual = visuals.outermost_visual.clone();
        let surface_size = surface.surface().size();
        let shown_surface_size = visuals.content.as_ref().map(|shown| shown.surface().size());
        if visuals.shown_rect != Some(rect) || shown_surface_size != Some(surface_size) {
            let matrix = matrix_placing_content_of_size_at(size_of(rect_in_surface), rect);
            // SAFETY: The visual is live.
            let _ = unsafe { tile_visual.SetTransform2(&raw const matrix) };
            visuals.shown_rect = Some(rect);
        }
        let alpha_mode = if every_pixel_is_opaque {
            DXGI_ALPHA_MODE_IGNORE
        } else {
            DXGI_ALPHA_MODE_PREMULTIPLIED
        };
        let composition_texture = self.composition_texture_of(surface.surface(), alpha_mode, rect_in_surface);
        if visuals
            .content
            .as_ref()
            .is_some_and(|shown| Rc::ptr_eq(shown.surface(), surface.surface()))
        {
            return;
        }
        let Some(composition_texture) = composition_texture else {
            eprintln!("Compositor: The window server cannot show the surface of a tile");
            if visuals.content_visual_shows_composition_texture_of_surface {
                // SAFETY: The visual is live.
                let _ = unsafe { tile_visual.SetContent(None) };
                visuals.content_visual_shows_composition_texture_of_surface = false;
            }
            visuals.content = None;
            return;
        };
        surface.surface().did_hand_to_window_server();
        // SAFETY: The visual and the texture are live.
        let _ = unsafe { tile_visual.SetContent(&composition_texture) };
        visuals.content_visual_shows_composition_texture_of_surface = true;
        visuals.content = Some(surface);
    }

    fn update_shown_color(&mut self, visuals: &mut Visuals, color: Color, rect: IntRect) {
        let color_visual = &visuals.outermost_visual;
        if visuals.shown_rect != Some(rect) {
            let matrix = matrix_placing_content_of_size_at(IntSize { width: 1, height: 1 }, rect);
            // SAFETY: The visual is live.
            let _ = unsafe { color_visual.SetTransform2(&raw const matrix) };
            visuals.shown_rect = Some(rect);
        }
        if visuals.shown_color == Some(color) {
            return;
        }
        let texture = self.shared.composition_texture_filled_with(color);
        // SAFETY: The visual and the texture are live.
        let _ = unsafe {
            match &texture {
                Some(texture) => color_visual.SetContent(texture),
                None => color_visual.SetContent(None::<&IUnknown>),
            }
        };
        visuals.shown_color = Some(color);
    }
}

impl NativeSublayers for Natives {
    type Native = Visuals;

    fn create(&mut self, kind: SublayerKind) -> Option<Visuals> {
        match self.create_visuals(kind) {
            Ok(visuals) => Some(visuals),
            Err(error) => {
                eprintln!("Compositor: Unable to create the visuals of a sublayer: {error}");
                None
            }
        }
    }

    fn update(&mut self, visuals: &mut Visuals, content: SublayerContent<PresentableSurfaceLease, PlacedContent>) {
        match content {
            SublayerContent::Group(group) => self.update_shown_group(visuals, &group),
            SublayerContent::Tile {
                surface,
                rect,
                rect_in_surface,
                every_pixel_is_opaque,
            } => self.update_shown_tile(visuals, surface, rect, rect_in_surface, every_pixel_is_opaque),
            SublayerContent::Color { color, rect } => self.update_shown_color(visuals, color, rect),
            SublayerContent::Backdrop(_) => {}
            SublayerContent::Placed { content, placement } => {
                if let Err(error) = self.update_placed_content(visuals, content, &placement) {
                    eprintln!("Compositor: Unable to show a sublayer: {error}");
                }
            }
        }
    }

    /// A composition texture reads as available again only once no visual has it as its content, not even one that is
    /// no longer in the tree, so a sublayer takes it out of its visual before the visual goes.
    fn stop_showing(&mut self, visuals: Visuals) {
        if visuals.content_visual_shows_composition_texture_of_surface
            && let Some(content_visual) = &visuals.content_visual
        {
            // SAFETY: The visual is live.
            let _ = unsafe { content_visual.SetContent(None) };
        }
    }

    fn remove_sublayers_of_group(&mut self, group: &Visuals) {
        // SAFETY: The visual is live.
        let _ = unsafe { group.outermost_visual.RemoveAllVisuals() };
    }

    fn remove_sublayers_of_frame(&mut self) {
        // SAFETY: The visual is live.
        let _ = unsafe { self.frame_visual.RemoveAllVisuals() };
    }

    fn add_sublayers_to_group(&mut self, group: &Visuals, sublayers: &[&Visuals]) {
        add_visuals(&group.outermost_visual, sublayers);
    }

    fn add_sublayers_to_frame(&mut self, sublayers: &[&Visuals]) {
        add_visuals(&self.frame_visual, sublayers);
    }
}

/// Adds the outermost visuals of the sublayers to the parent, bottom to top.
fn add_visuals(parent: &IDCompositionVisual2, sublayers: &[&Visuals]) {
    let mut visual_below: Option<&IDCompositionVisual> = None;
    for sublayer in sublayers {
        let visual: &IDCompositionVisual = &sublayer.outermost_visual;
        // SAFETY: The visuals are live, and the visual is in no parent.
        if let Err(error) = unsafe { parent.AddVisual(visual, true, visual_below) } {
            eprintln!("Compositor: Unable to add the visual of a sublayer to its parent: {error}");
            continue;
        }
        visual_below = Some(visual);
    }
}
