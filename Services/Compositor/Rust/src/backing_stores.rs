/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The backing stores of a context: the surfaces its frames are rendered into, each of which is in turn rendered into,
//! shown, and free again. The surfaces and the images they share with the presentation are made by the raster host.

use crate::surfaces::painting_surface::PaintingSurface;
use crate::surfaces::presentable_surface::PresentableSurface;
use libgfx_rust::IntSize;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;

/// Who shows the backing stores of a context.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum StorePublication {
    #[default]
    Unpublished,
    /// The presentation shows the stores, and releases each when it shows another.
    Published,
    /// The compositor shows the stores itself, in a remote layer, so a store is replaced on screen by the next one.
    ShownInRemoteLayer,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StoreResizeOutcome {
    Unchanged,
    /// The stores were reallocated, and nothing that shows them was given them.
    ReallocatedOffscreen,
    /// The presentation or the remote layer that shows the stores was given the new ones.
    ReallocatedForViewer,
    /// The stores of a remote layer could not be allocated.
    Failed,
}

/// How a store is made.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum StoreKind {
    /// A surface nobody else sees.
    Offscreen,
    /// A bitmap in memory shared with the presentation, which a GPU surface is copied into on flush where there is one.
    SharedBitmap,
    /// A GPU image shared with the presentation, where the platform has those.
    SharedGpuImage,
    /// A surface the window server of the remote layer that shows the context shows by itself.
    ShownByWindowServer,
}

/// The memory of a store that is shared with the presentation, which is C++ and which the store owns.
pub struct SharedImageBuffer {
    buffer: NonNull<c_void>,
    is_in_use: unsafe extern "C" fn(*mut c_void) -> bool,
    destroy: unsafe extern "C" fn(*mut c_void),
}

impl SharedImageBuffer {
    fn is_in_use(&self) -> bool {
        // SAFETY: The buffer is live for as long as this.
        unsafe { (self.is_in_use)(self.buffer.as_ptr()) }
    }
}

impl Drop for SharedImageBuffer {
    fn drop(&mut self) {
        // SAFETY: This owns the buffer.
        unsafe { (self.destroy)(self.buffer.as_ptr()) };
    }
}

/// An image of a store for the presentation to import, which is C++, and which is destroyed unless it is handed over.
pub struct SharedImageToPublish {
    image: NonNull<c_void>,
    destroy: unsafe extern "C" fn(*mut c_void),
}

impl SharedImageToPublish {
    /// Hands the image over to whoever publishes it.
    pub fn into_raw(self) -> *mut c_void {
        let image = self.image.as_ptr();
        std::mem::forget(self);
        image
    }
}

impl Drop for SharedImageToPublish {
    fn drop(&mut self) {
        // SAFETY: This owns the image.
        unsafe { (self.destroy)(self.image.as_ptr()) };
    }
}

/// A store the raster host made, as it hands it over: one reference to each surface, and the C++ objects with the
/// functions that answer for and destroy them.
#[repr(C)]
pub struct FfiNewStore {
    pub surface: *const PaintingSurface,
    /// Null for a store the window server does not show by itself.
    pub window_server_surface: *const PresentableSurface,
    /// Null for a store whose memory is not shared with the presentation.
    pub shared_image_buffer: *mut c_void,
    pub shared_image_buffer_is_in_use: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    pub destroy_shared_image_buffer: Option<unsafe extern "C" fn(*mut c_void)>,
    /// Null for a store the presentation does not import.
    pub shared_image: *mut c_void,
    pub destroy_shared_image: Option<unsafe extern "C" fn(*mut c_void)>,
}

impl Default for FfiNewStore {
    fn default() -> Self {
        Self {
            surface: std::ptr::null(),
            window_server_surface: std::ptr::null(),
            shared_image_buffer: std::ptr::null_mut(),
            shared_image_buffer_is_in_use: None,
            destroy_shared_image_buffer: None,
            shared_image: std::ptr::null_mut(),
            destroy_shared_image: None,
        }
    }
}

pub struct NewStore {
    pub surface: Rc<PaintingSurface>,
    pub window_server_surface: Option<Rc<PresentableSurface>>,
    pub shared_image_buffer: Option<SharedImageBuffer>,
    pub shared_image: Option<SharedImageToPublish>,
}

impl NewStore {
    /// # Safety
    ///
    /// Every pointer of the store must be live and handed over, with the functions that go with it.
    pub unsafe fn adopt(store: FfiNewStore) -> Option<NewStore> {
        // SAFETY: The caller hands over one reference to each surface, or null.
        let (surface, window_server_surface) = unsafe {
            (
                PaintingSurface::adopted_from_ffi(store.surface),
                PresentableSurface::adopted_from_ffi(store.window_server_surface),
            )
        };
        let shared_image_buffer = NonNull::new(store.shared_image_buffer).map(|buffer| SharedImageBuffer {
            buffer,
            is_in_use: store
                .shared_image_buffer_is_in_use
                .expect("a shared buffer comes with its use check"),
            destroy: store
                .destroy_shared_image_buffer
                .expect("a shared buffer comes with its destroy function"),
        });
        let shared_image = NonNull::new(store.shared_image).map(|image| SharedImageToPublish {
            image,
            destroy: store
                .destroy_shared_image
                .expect("a shared image comes with its destroy function"),
        });
        Some(NewStore {
            surface: surface?,
            window_server_surface,
            shared_image_buffer,
            shared_image,
        })
    }
}

/// Makes the stores of a context, or answers none where one could not be made.
pub trait StoreFactory {
    fn create_store(&mut self, kind: StoreKind, size: IntSize) -> Option<NewStore>;
}

impl<F: FnMut(StoreKind, IntSize) -> Option<NewStore>> StoreFactory for F {
    fn create_store(&mut self, kind: StoreKind, size: IntSize) -> Option<NewStore> {
        self(kind, size)
    }
}

/// Stores that were allocated or added, with the images the presentation imports, if it shows the stores.
#[derive(Default)]
pub struct Publication {
    pub bitmap_ids: Vec<i32>,
    pub shared_images: Vec<SharedImageToPublish>,
}

pub enum AllocationOutcome {
    /// Nothing that shows the stores was given them.
    Offscreen,
    /// The presentation or the remote layer that shows the stores is to be given the new ones.
    ForViewer(Publication),
    /// The stores of a remote layer could not be made.
    Failed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum StoreState {
    Available,
    Rendering,
    Presented,
}

struct BackingStore {
    surface: Rc<PaintingSurface>,
    shared_image_buffer: Option<SharedImageBuffer>,
    window_server_surface: Option<Rc<PresentableSurface>>,
    bitmap_id: i32,
    state: StoreState,
    was_shown_on_screen: bool,
    was_rendered_into_since_last_retirement_check: bool,
}

impl BackingStore {
    fn new(store: NewStore, bitmap_id: i32, state: StoreState) -> (BackingStore, Option<SharedImageToPublish>) {
        (
            BackingStore {
                surface: store.surface,
                shared_image_buffer: store.shared_image_buffer,
                window_server_surface: store.window_server_surface,
                bitmap_id,
                state,
                was_shown_on_screen: false,
                was_rendered_into_since_last_retirement_check: false,
            },
            store.shared_image,
        )
    }

    fn shared_surface_is_in_use(&self) -> bool {
        if let Some(window_server_surface) = &self.window_server_surface {
            return window_server_surface.is_read_by_window_server();
        }
        // The share exported to the presentation counts as use too, so a store published a moment ago reads as in use
        // until the presentation imported it.
        self.shared_image_buffer
            .as_ref()
            .is_some_and(SharedImageBuffer::is_in_use)
    }

    fn can_be_rendered_into(&self) -> bool {
        if self.state != StoreState::Available {
            return false;
        }
        // Only a store shown on screen is handed to the window server, so whatever uses a store that never was is the
        // share on its way to the presentation.
        if !self.was_shown_on_screen {
            return true;
        }
        !self.shared_surface_is_in_use()
    }
}

const MAXIMUM_BACKING_STORE_COUNT: usize = 8;
const RESIZE_PADDING: i32 = 256;

// A store shown on screen is let go of when the next one is shown, but on macOS the window server is handed the surface
// itself, and keeps reading it until it has composited the replacement. A third store lets the compositor keep
// rendering through that window instead of waiting for it.
fn backing_store_count_for(publication: StorePublication) -> usize {
    if cfg!(target_os = "macos") && publication != StorePublication::Unpublished {
        return 3;
    }
    2
}

// Whether the memory of a 32-bit bitmap of the size could not be addressed.
fn bitmap_size_would_overflow(size: IntSize) -> bool {
    if size.width < 0 || size.height < 0 || size.width > i32::from(u16::MAX) || size.height > i32::from(u16::MAX) {
        return true;
    }
    (size.width * 4).checked_mul(size.height).is_none()
}

/// An allocation of new stores of the size, which replaces the stores there are.
pub struct Allocation {
    size: IntSize,
    bitmap_ids: Vec<i32>,
}

#[derive(Default)]
pub struct BackingStores {
    next_bitmap_id: i32,
    // What the stores were allocated for, which a new allocation replaces. Empty when the stores need allocating.
    allocated_size: IntSize,
    store_size: IntSize,
    publication: StorePublication,
    initial_store_count: usize,
    stores: Vec<BackingStore>,
    rendering_store_index: Option<usize>,
    latest_rendered_store_index: Option<usize>,
}

impl BackingStores {
    pub fn invalidate(&mut self) {
        self.allocated_size = IntSize::default();
    }

    pub fn publication(&self) -> StorePublication {
        self.publication
    }

    pub fn is_valid(&self) -> bool {
        !self.stores.is_empty()
    }

    pub fn is_rendering(&self) -> bool {
        self.rendering_store_index.is_some()
    }

    pub fn has_available_store(&self) -> bool {
        self.stores.iter().any(BackingStore::can_be_rendered_into)
    }

    pub fn has_surplus_stores(&self) -> bool {
        self.stores.len() > self.initial_store_count
    }

    pub fn resize_if_needed(
        &mut self,
        viewport_size: IntSize,
        window_resize_in_progress: bool,
        publication: StorePublication,
    ) -> Option<Allocation> {
        if viewport_size.is_empty() {
            return None;
        }
        let mut allocation_size = viewport_size;
        let mut needs_allocation = self.allocated_size.is_empty();
        if window_resize_in_progress {
            // The stores are padded so that they need not be reallocated while the window is resized. Ones that still
            // hold the viewport are kept.
            allocation_size = IntSize {
                width: viewport_size.width + RESIZE_PADDING,
                height: viewport_size.height + RESIZE_PADDING,
            };
            needs_allocation |= !(self.allocated_size.width >= viewport_size.width
                && self.allocated_size.height >= viewport_size.height);
        } else {
            needs_allocation |= self.allocated_size != viewport_size;
        }
        if !needs_allocation {
            return None;
        }
        self.allocated_size = allocation_size;
        let bitmap_ids = (0..backing_store_count_for(publication))
            .map(|_| self.take_bitmap_id())
            .collect();
        Some(Allocation {
            size: allocation_size,
            bitmap_ids,
        })
    }

    fn take_bitmap_id(&mut self) -> i32 {
        let bitmap_id = self.next_bitmap_id;
        self.next_bitmap_id += 1;
        bitmap_id
    }

    pub fn allocate(
        &mut self,
        allocation: Allocation,
        publication: StorePublication,
        shares_gpu_images: bool,
        factory: &mut dyn StoreFactory,
    ) -> AllocationOutcome {
        self.stores.clear();
        self.rendering_store_index = None;
        self.latest_rendered_store_index = None;
        self.publication = publication;
        if bitmap_size_would_overflow(allocation.size) {
            return AllocationOutcome::Offscreen;
        }
        self.store_size = allocation.size;
        self.initial_store_count = allocation.bitmap_ids.len();

        match publication {
            StorePublication::Unpublished => {
                for &bitmap_id in &allocation.bitmap_ids {
                    let Some(store) = factory.create_store(StoreKind::Offscreen, allocation.size) else {
                        self.stores.clear();
                        self.invalidate();
                        break;
                    };
                    self.stores
                        .push(BackingStore::new(store, bitmap_id, StoreState::Available).0);
                }
                AllocationOutcome::Offscreen
            }
            StorePublication::ShownInRemoteLayer => {
                for &bitmap_id in &allocation.bitmap_ids {
                    let Some(store) = factory.create_store(StoreKind::ShownByWindowServer, allocation.size) else {
                        self.stores.clear();
                        self.invalidate();
                        return AllocationOutcome::Failed;
                    };
                    self.stores
                        .push(BackingStore::new(store, bitmap_id, StoreState::Available).0);
                }
                AllocationOutcome::ForViewer(Publication {
                    bitmap_ids: allocation.bitmap_ids,
                    shared_images: Vec::new(),
                })
            }
            StorePublication::Published => {
                if shares_gpu_images
                    && let Some(publication) = self.allocate_published(&allocation, StoreKind::SharedGpuImage, factory)
                {
                    return AllocationOutcome::ForViewer(publication);
                }
                match self.allocate_published(&allocation, StoreKind::SharedBitmap, factory) {
                    Some(publication) => AllocationOutcome::ForViewer(publication),
                    None => {
                        self.invalidate();
                        AllocationOutcome::Offscreen
                    }
                }
            }
        }
    }

    // The presentation installs the first published store as its first front store, and keeps it until it presents
    // another one.
    fn allocate_published(
        &mut self,
        allocation: &Allocation,
        kind: StoreKind,
        factory: &mut dyn StoreFactory,
    ) -> Option<Publication> {
        let mut shared_images = Vec::with_capacity(allocation.bitmap_ids.len());
        for (index, &bitmap_id) in allocation.bitmap_ids.iter().enumerate() {
            let state = if index == 0 {
                StoreState::Presented
            } else {
                StoreState::Available
            };
            let Some(store) = factory.create_store(kind, allocation.size) else {
                self.stores.clear();
                return None;
            };
            let (store, shared_image) = BackingStore::new(store, bitmap_id, state);
            self.stores.push(store);
            shared_images.push(shared_image.expect("a published store has an image for the presentation"));
        }
        Some(Publication {
            bitmap_ids: allocation.bitmap_ids.clone(),
            shared_images,
        })
    }

    // GPU stores shared with the presentation are only ever allocated together, as the presentation installs them.
    fn stores_can_be_added_one_at_a_time(&self) -> bool {
        let Some(first) = self.stores.first() else {
            return false;
        };
        self.publication != StorePublication::Published || first.shared_image_buffer.is_some()
    }

    fn kind_of_added_stores(&self) -> StoreKind {
        match self.publication {
            StorePublication::Unpublished => StoreKind::Offscreen,
            StorePublication::Published => StoreKind::SharedBitmap,
            StorePublication::ShownInRemoteLayer => StoreKind::ShownByWindowServer,
        }
    }

    fn add_store(&mut self, factory: &mut dyn StoreFactory, publication: &mut Publication) -> bool {
        let Some(store) = factory.create_store(self.kind_of_added_stores(), self.store_size) else {
            return false;
        };
        let bitmap_id = self.take_bitmap_id();
        let (store, shared_image) = BackingStore::new(store, bitmap_id, StoreState::Available);
        self.stores.push(store);
        publication.bitmap_ids.push(bitmap_id);
        if self.publication == StorePublication::Published {
            publication
                .shared_images
                .push(shared_image.expect("a published store has an image for the presentation"));
        }
        true
    }

    /// The window server lets go of a store some time after the presentation replaced it on screen, which at high
    /// refresh rates can be later than the next frame is due. Rather than hold that frame back, it is given a store of
    /// its own. While the presentation has yet to take a frame, or to release a store, there is no telling what the
    /// window server holds, and the frame waits as it always did.
    pub fn add_store_if_window_server_still_reads_every_released_store(
        &mut self,
        factory: &mut dyn StoreFactory,
    ) -> Option<Publication> {
        let latest_rendered_store_index = self.latest_rendered_store_index?;
        if self.is_rendering() || self.stores.len() >= MAXIMUM_BACKING_STORE_COUNT {
            return None;
        }
        for (index, store) in self.stores.iter().enumerate() {
            if store.shared_image_buffer.is_none() && store.window_server_surface.is_none() {
                return None;
            }
            let presentation_shows_store = index == latest_rendered_store_index;
            if presentation_shows_store && store.state != StoreState::Presented {
                return None;
            }
            if !presentation_shows_store && (store.state != StoreState::Available || store.can_be_rendered_into()) {
                return None;
            }
        }
        let mut publication = Publication::default();
        self.add_store(factory, &mut publication).then_some(publication)
    }

    fn remove_store(&mut self, index: usize) -> i32 {
        let store = self.stores.remove(index);
        if let Some(latest) = self.latest_rendered_store_index
            && latest > index
        {
            self.latest_rendered_store_index = Some(latest - 1);
        }
        store.bitmap_id
    }

    /// A store added for a burst of frames is given back once a whole interval between two checks went by without it
    /// being rendered into. The stores that stay are the ones first in line for the next frame. Answers the stores
    /// that were retired.
    pub fn retire_idle_surplus_stores(&mut self) -> Vec<i32> {
        let mut retired_bitmap_ids = Vec::new();
        if self.is_rendering() {
            return retired_bitmap_ids;
        }
        for index in (0..self.stores.len()).rev() {
            if !self.has_surplus_stores() {
                break;
            }
            let store = &self.stores[index];
            if self.latest_rendered_store_index == Some(index)
                || store.state != StoreState::Available
                || store.was_rendered_into_since_last_retirement_check
                || store.shared_surface_is_in_use()
            {
                continue;
            }
            retired_bitmap_ids.push(self.remove_store(index));
        }
        for store in &mut self.stores {
            store.was_rendered_into_since_last_retirement_check = false;
        }
        retired_bitmap_ids
    }

    /// A hidden context keeps only the store that holds its latest frame, which is what shows it until it is shown
    /// again, and what a snapshot of it is taken from. Before any frame, the store reserved as the presentation's first
    /// front store stays too. Answers the stores that were retired.
    pub fn retire_stores_except_the_latest_rendered(&mut self) -> Vec<i32> {
        let mut retired_bitmap_ids = Vec::new();
        if self.is_rendering() {
            return retired_bitmap_ids;
        }
        for index in (0..self.stores.len()).rev() {
            let store = &self.stores[index];
            if self.latest_rendered_store_index == Some(index)
                || store.state != StoreState::Available
                || store.shared_surface_is_in_use()
            {
                continue;
            }
            retired_bitmap_ids.push(self.remove_store(index));
        }
        // Stores that never held a frame are allocated anew when the context is shown.
        if self.stores.is_empty() {
            self.invalidate();
        }
        retired_bitmap_ids
    }

    /// Whether a store that the hidden context does not keep is still rendered into, shown, or read by the window
    /// server, and can be retired once it no longer is.
    pub fn has_stores_left_to_retire_while_hidden(&self) -> bool {
        self.stores.iter().enumerate().any(|(index, store)| {
            if self.latest_rendered_store_index == Some(index) {
                return false;
            }
            match store.state {
                StoreState::Rendering => true,
                // Before any frame, a presented store is the one reserved as the presentation's first front store,
                // which the presentation holds until it is shown a frame.
                StoreState::Presented => self.latest_rendered_store_index.is_some(),
                StoreState::Available => store.shared_surface_is_in_use(),
            }
        })
    }

    /// Gives a context that was shown again the stores it had before it was hidden. Answers none where they can't be
    /// added to the ones it kept, or one could not be made, as the context then needs a whole new allocation.
    pub fn add_stores_up_to_initial_count(&mut self, factory: &mut dyn StoreFactory) -> Option<Publication> {
        if !self.stores_can_be_added_one_at_a_time() {
            return None;
        }
        let mut publication = Publication::default();
        while self.stores.len() < self.initial_store_count {
            if !self.add_store(factory, &mut publication) {
                return None;
            }
        }
        Some(publication)
    }

    /// The surface of the store that `acquire_render_target` takes next.
    pub fn surface_of_next_render_target(&self) -> Option<Rc<PaintingSurface>> {
        self.stores
            .iter()
            .find(|store| store.can_be_rendered_into())
            .map(|store| store.surface.clone())
    }

    pub fn acquire_render_target(&mut self) -> Option<(i32, Rc<PaintingSurface>)> {
        assert!(
            self.rendering_store_index.is_none(),
            "one store is rendered into at a time"
        );
        let index = self.stores.iter().position(BackingStore::can_be_rendered_into)?;
        let store = &mut self.stores[index];
        store.state = StoreState::Rendering;
        store.was_rendered_into_since_last_retirement_check = true;
        self.rendering_store_index = Some(index);
        Some((store.bitmap_id, store.surface.clone()))
    }

    pub fn complete_rendering(&mut self, bitmap_id: i32) {
        let index = self.rendering_store_index.take().expect("a store is rendered into");
        assert_eq!(
            self.stores[index].bitmap_id, bitmap_id,
            "the store rendered into completes"
        );
        assert_eq!(self.stores[index].state, StoreState::Rendering);
        let presentation_releases_presented_stores = self.publication == StorePublication::Published;
        if !presentation_releases_presented_stores && let Some(latest) = self.latest_rendered_store_index {
            self.stores[latest].state = StoreState::Available;
        }
        let store = &mut self.stores[index];
        store.state = StoreState::Presented;
        if self.publication != StorePublication::Unpublished {
            store.was_shown_on_screen = true;
        }
        self.latest_rendered_store_index = Some(index);
    }

    /// Answers whether a store that was presented became available.
    pub fn release_store(&mut self, bitmap_id: i32) -> bool {
        let Some(store) = self
            .stores
            .iter_mut()
            .find(|store| store.bitmap_id == bitmap_id && store.state == StoreState::Presented)
        else {
            return false;
        };
        store.state = StoreState::Available;
        true
    }

    pub fn release_all_presented_stores(&mut self) {
        for store in &mut self.stores {
            if store.state == StoreState::Presented {
                store.state = StoreState::Available;
            }
        }
    }

    pub fn latest_rendered_surface(&self) -> Option<Rc<PaintingSurface>> {
        Some(self.stores[self.latest_rendered_store_index?].surface.clone())
    }

    pub fn window_server_surface_of_latest_rendered_store(&self) -> Option<&Rc<PresentableSurface>> {
        self.stores[self.latest_rendered_store_index?]
            .window_server_surface
            .as_ref()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surfaces::painting_surface::testing::surface_for_testing;
    use std::cell::Cell;

    unsafe extern "C" fn destroy_nothing(_: *mut c_void) {}

    thread_local! {
        static SHARED_BUFFERS_IN_USE: Cell<bool> = const { Cell::new(false) };
    }

    unsafe extern "C" fn shared_buffer_in_use(_: *mut c_void) -> bool {
        SHARED_BUFFERS_IN_USE.with(Cell::get)
    }

    fn factory(fail_kind: Option<StoreKind>) -> impl FnMut(StoreKind, IntSize) -> Option<NewStore> {
        move |kind, size| {
            if Some(kind) == fail_kind {
                return None;
            }
            let shares_memory = kind == StoreKind::SharedBitmap;
            let published = matches!(kind, StoreKind::SharedBitmap | StoreKind::SharedGpuImage);
            Some(NewStore {
                surface: surface_for_testing(size, 0),
                window_server_surface: None,
                shared_image_buffer: shares_memory.then(|| SharedImageBuffer {
                    buffer: NonNull::dangling(),
                    is_in_use: shared_buffer_in_use,
                    destroy: destroy_nothing,
                }),
                shared_image: published.then(|| SharedImageToPublish {
                    image: NonNull::dangling(),
                    destroy: destroy_nothing,
                }),
            })
        }
    }

    const VIEWPORT: IntSize = IntSize { width: 100, height: 50 };

    // Each store completes its frame before the next is rendered into, as many as the platform allocates.
    fn render_into_every_available_store(stores: &mut BackingStores) -> Vec<i32> {
        let mut rendered = Vec::new();
        while let Some((bitmap_id, _)) = stores.acquire_render_target() {
            stores.complete_rendering(bitmap_id);
            rendered.push(bitmap_id);
        }
        rendered
    }

    fn allocated(publication: StorePublication, shares_gpu_images: bool) -> (BackingStores, AllocationOutcome) {
        let mut stores = BackingStores::default();
        let allocation = stores.resize_if_needed(VIEWPORT, false, publication).unwrap();
        let outcome = stores.allocate(allocation, publication, shares_gpu_images, &mut factory(None));
        (stores, outcome)
    }

    #[test]
    fn stores_are_reallocated_only_when_the_viewport_changes() {
        let (mut stores, _) = allocated(StorePublication::Unpublished, false);
        assert!(
            stores
                .resize_if_needed(VIEWPORT, false, StorePublication::Unpublished)
                .is_none()
        );
        let larger = IntSize { width: 120, height: 50 };
        assert!(
            stores
                .resize_if_needed(larger, false, StorePublication::Unpublished)
                .is_some()
        );
    }

    #[test]
    fn stores_are_padded_while_the_window_is_resized() {
        let mut stores = BackingStores::default();
        let allocation = stores
            .resize_if_needed(VIEWPORT, true, StorePublication::Unpublished)
            .unwrap();
        assert_eq!(
            allocation.size,
            IntSize {
                width: 356,
                height: 306
            }
        );
        stores.allocate(allocation, StorePublication::Unpublished, false, &mut factory(None));
        let a_bit_larger = IntSize { width: 150, height: 80 };
        assert!(
            stores
                .resize_if_needed(a_bit_larger, true, StorePublication::Unpublished)
                .is_none()
        );
    }

    #[test]
    fn the_first_published_store_is_held_by_the_presentation() {
        let (mut stores, outcome) = allocated(StorePublication::Published, false);
        let AllocationOutcome::ForViewer(publication) = outcome else {
            panic!("published stores go to the presentation");
        };
        assert_eq!(publication.bitmap_ids.len(), publication.shared_images.len());
        let (bitmap_id, _) = stores.acquire_render_target().unwrap();
        assert_ne!(bitmap_id, publication.bitmap_ids[0]);
    }

    #[test]
    fn shared_gpu_images_fall_back_to_shared_bitmaps() {
        let mut stores = BackingStores::default();
        let allocation = stores
            .resize_if_needed(VIEWPORT, false, StorePublication::Published)
            .unwrap();
        let outcome = stores.allocate(
            allocation,
            StorePublication::Published,
            true,
            &mut factory(Some(StoreKind::SharedGpuImage)),
        );
        assert!(matches!(outcome, AllocationOutcome::ForViewer(_)));
        assert!(stores.stores_can_be_added_one_at_a_time());
    }

    #[test]
    fn stores_of_a_remote_layer_that_cannot_be_made_fail_the_allocation() {
        let mut stores = BackingStores::default();
        let allocation = stores
            .resize_if_needed(VIEWPORT, false, StorePublication::ShownInRemoteLayer)
            .unwrap();
        let outcome = stores.allocate(
            allocation,
            StorePublication::ShownInRemoteLayer,
            false,
            &mut factory(Some(StoreKind::ShownByWindowServer)),
        );
        assert!(matches!(outcome, AllocationOutcome::Failed));
        assert!(!stores.is_valid());
    }

    #[test]
    fn an_unpublished_store_becomes_available_once_the_next_frame_completes() {
        let (mut stores, _) = allocated(StorePublication::Unpublished, false);
        let (first, _) = stores.acquire_render_target().unwrap();
        stores.complete_rendering(first);
        let (second, _) = stores.acquire_render_target().unwrap();
        assert_ne!(first, second);
        stores.complete_rendering(second);
        let (third, _) = stores.acquire_render_target().unwrap();
        assert_eq!(third, first);
    }

    #[test]
    fn a_published_store_waits_for_the_presentation_to_release_it() {
        let (mut stores, _) = allocated(StorePublication::Published, false);
        let rendered = render_into_every_available_store(&mut stores);
        assert!(!stores.has_available_store());
        let latest = *rendered.last().unwrap();
        assert!(stores.release_store(latest));
        assert!(!stores.release_store(latest));
        assert!(stores.has_available_store());
    }

    #[test]
    fn surplus_stores_are_retired_after_an_idle_interval() {
        let (mut stores, _) = allocated(StorePublication::Published, false);
        let (rendered, _) = stores.acquire_render_target().unwrap();
        stores.complete_rendering(rendered);
        let first_front_store_bitmap_id = 0;
        assert!(stores.release_store(first_front_store_bitmap_id));
        // A store that is not shown is free, so no store is added.
        assert!(
            stores
                .add_store_if_window_server_still_reads_every_released_store(&mut factory(None))
                .is_none()
        );
        // Every store that is not shown was shown before, and the window server still reads it.
        SHARED_BUFFERS_IN_USE.with(|in_use| in_use.set(true));
        let latest_rendered_store_index = stores.latest_rendered_store_index;
        for (index, store) in stores.stores.iter_mut().enumerate() {
            if Some(index) != latest_rendered_store_index {
                store.was_shown_on_screen = true;
            }
        }
        let added = stores
            .add_store_if_window_server_still_reads_every_released_store(&mut factory(None))
            .unwrap();
        SHARED_BUFFERS_IN_USE.with(|in_use| in_use.set(false));
        assert_eq!(added.bitmap_ids.len(), 1);
        assert!(stores.has_surplus_stores());
        // The added store was not rendered into since it was added, so it goes at the next check.
        assert_eq!(stores.retire_idle_surplus_stores(), added.bitmap_ids);
        assert!(!stores.has_surplus_stores());
    }

    #[test]
    fn a_hidden_context_keeps_only_its_latest_frame_and_gets_its_stores_back() {
        let (mut stores, _) = allocated(StorePublication::Unpublished, false);
        let (rendered, _) = stores.acquire_render_target().unwrap();
        stores.complete_rendering(rendered);
        assert_eq!(stores.retire_stores_except_the_latest_rendered().len(), 1);
        assert!(!stores.has_stores_left_to_retire_while_hidden());
        let restored = stores.add_stores_up_to_initial_count(&mut factory(None)).unwrap();
        assert_eq!(restored.bitmap_ids.len(), 1);
        assert_eq!(
            stores.latest_rendered_surface().map(|surface| surface.size()),
            Some(VIEWPORT)
        );
    }
}
