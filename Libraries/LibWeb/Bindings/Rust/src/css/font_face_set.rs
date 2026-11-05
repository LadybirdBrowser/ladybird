/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FontFaceSet, https://drafts.csswg.org/css-font-loading/#fontfaceset

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::{HashMap, HashSet};

use ak::Utf16String;
use libjs_rust::api::{
    Array, Gc, GcRefCell, MarkedVec, Must, Object, PrimitiveString, PropertyKey, Realm, Set, Trace, Value, Visitor, Vm,
    length_of_array_like,
};
use libweb_rust::typed_om::font_family_names;

use super::css_rule::wrap_cpp_cell;
use super::font_face_set_load_event::FontFaceSetLoadEvent;
use crate::bindings::conversions::{CppInterface, ToJs};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{
    Ref, Wrappable, allocate, cell_into_abi, with_temporary_execution_context,
    with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, get_promise_for_wait_for_all, in_parallel, is_promise_fulfilled,
    promise_from_abi, queue_a_task, react_to_promise, reject_promise, reject_promise_with_exception, resolve_promise,
};
use crate::bindings::setlike::context_of_set;
use crate::bindings::setlike::{Setlike, WorldEntries};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::event::EventInit;

unsafe extern "C" {
    fn web_css_font_face_of_wrapper(wrapper: *mut c_void) -> *mut c_void;
    fn web_css_font_face_ref(font_face: *mut c_void);
    fn web_css_font_face_unref(font_face: *mut c_void);
    fn web_css_font_face_visit_edges(font_face: *mut c_void, visitor: *mut c_void);
    fn web_css_font_face_cssom_font_face(font_face: *mut c_void) -> *mut c_void;
    fn web_css_font_face_is_css_connected(font_face: *mut c_void) -> bool;
    fn web_css_font_face_css_rule_identity(font_face: *mut c_void, identity: *mut u64) -> bool;
    fn web_css_font_face_is_loading(font_face: *mut c_void) -> bool;
    fn web_css_font_face_is_loaded(font_face: *mut c_void) -> bool;
    fn web_css_font_face_family_name(font_face: *mut c_void) -> usize;
    fn web_css_font_face_unicode_ranges_include_any_of(font_face: *mut c_void, text: usize) -> bool;
    fn web_css_font_face_load(font_face: *mut c_void) -> *mut c_void;
    fn web_css_font_face_did_join_set(font_face: *mut c_void, set: *mut c_void);
    fn web_css_font_face_did_leave_set(font_face: *mut c_void, set: *mut c_void);
    fn web_css_font_face_set_record_loading_fonts(set: *mut c_void, count: usize);
    fn web_css_font_face_set_update_layout(set: *mut c_void, skip_if_running: bool);
    fn web_css_font_face_set_for_each_font_face_rule(
        set: *mut c_void,
        context: *mut c_void,
        callback: unsafe extern "C" fn(*mut c_void, u64),
    ) -> bool;
    fn web_css_font_face_set_synchronize_font_computer(set: *mut c_void, font_faces: *const *mut c_void, count: usize);
}

/// The enumeration FontFaceSetLoadStatus of CSS/FontFaceSet.idl.
#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum FontFaceSetLoadStatus {
    Loading,
    Loaded,
}

/// The reference-counted state of a font face, which LibWeb implements in C++. The FontFace object that script sees is
/// only made from it when something wraps it.
pub(crate) struct CppFontFaceState(NonNull<c_void>);

impl Clone for CppFontFaceState {
    fn clone(&self) -> Self {
        // SAFETY: This holds a reference, so the state is live.
        unsafe { Self::from_raw(self.as_ptr()) }
    }
}

impl Drop for CppFontFaceState {
    fn drop(&mut self) {
        // SAFETY: This holds a reference.
        unsafe { web_css_font_face_unref(self.as_ptr()) };
    }
}

// SAFETY: The state visits every cell it can reach.
unsafe impl Trace for CppFontFaceState {
    fn trace(&self, visitor: &mut Visitor) {
        // SAFETY: This holds a reference, and the visitor is live.
        unsafe { web_css_font_face_visit_edges(self.as_ptr(), visitor.as_raw().cast()) };
    }
}

impl CppFontFaceState {
    /// # Safety
    ///
    /// `font_face` must be a live FontFaceState, which this takes a reference to.
    pub(crate) unsafe fn from_raw(font_face: *mut c_void) -> Self {
        let font_face = NonNull::new(font_face).expect("C++ passes a font face");
        // SAFETY: The caller passes a live state.
        unsafe { web_css_font_face_ref(font_face.as_ptr()) };
        Self(font_face)
    }

    pub(crate) fn of_wrapper(wrapper: Gc<Object>) -> Self {
        // SAFETY: The caller passes a wrapper of a FontFace, whose state is live.
        unsafe { Self::from_raw(web_css_font_face_of_wrapper(wrapper.as_ptr().cast())) }
    }

    fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    fn is(&self, other: &Self) -> bool {
        self.as_ptr() == other.as_ptr()
    }

    pub(crate) fn interface(&self, cx: &CallContext) -> CppInterface {
        // SAFETY: This holds a reference, and LibWeb returns a live FontFace.
        let font_face = unsafe { CppCell::from_raw(web_css_font_face_cssom_font_face(self.as_ptr())) };
        wrap_cpp_cell(cx, &font_face)
    }

    fn wrapper(&self, cx: &CallContext) -> Value {
        Value::from_object(self.interface(cx).wrapper())
    }

    fn is_css_connected(&self) -> bool {
        // SAFETY: The FontFace is live.
        unsafe { web_css_font_face_is_css_connected(self.as_ptr()) }
    }

    fn css_rule_identity(&self) -> Option<u64> {
        let mut identity = 0;
        // SAFETY: The FontFace is live, and the identity outlives the call.
        unsafe { web_css_font_face_css_rule_identity(self.as_ptr(), &raw mut identity) }.then_some(identity)
    }

    fn is_loading(&self) -> bool {
        // SAFETY: The FontFace is live.
        unsafe { web_css_font_face_is_loading(self.as_ptr()) }
    }

    fn is_loaded(&self) -> bool {
        // SAFETY: The FontFace is live.
        unsafe { web_css_font_face_is_loaded(self.as_ptr()) }
    }

    fn family_name(&self) -> Utf16String {
        // SAFETY: The FontFace is live, and LibWeb hands over the string.
        unsafe { Utf16String::from_raw_owned(web_css_font_face_family_name(self.as_ptr())) }
    }

    fn unicode_ranges_include_any_of(&self, text: &Utf16String) -> bool {
        // SAFETY: The FontFace is live, and this hands over a reference to the string.
        unsafe { web_css_font_face_unicode_ranges_include_any_of(self.as_ptr(), text.clone().into_raw()) }
    }

    /// Calls load() on the FontFace, and returns its [[FontStatusPromise]].
    fn load(&self) -> WebIdlPromise {
        // SAFETY: The FontFace is live, and LibWeb returns a live promise.
        unsafe { promise_from_abi(web_css_font_face_load(self.as_ptr())) }
    }
}

fn contains(font_faces: &[CppFontFaceState], font_face: &CppFontFaceState) -> bool {
    font_faces.iter().any(|entry| entry.is(font_face))
}

fn take_font_faces<'vm>(vm: &'vm Vm, list: &GcRefCell<Vec<CppFontFaceState>>) -> MarkedVec<'vm, CppFontFaceState> {
    let font_faces = MarkedVec::with_capacity(vm, list.borrow().len());
    font_faces.extend_from_slice(&list.borrow());
    list.borrow_mut(vm).clear();
    font_faces
}

// https://drafts.csswg.org/css-font-loading/#fontfaceset
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct FontFaceSet {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
    environment: CppCell,
    this: Cell<Option<Ref<FontFaceSet>>>,

    /// The set entries of this setlike object, in insertion order.
    font_faces: GcRefCell<Vec<CppFontFaceState>>,
    set_entries: WorldEntries<Set>,

    ready_promise: Cell<Option<WebIdlPromise>>,      // [[ReadyPromise]]
    loading_fonts: GcRefCell<Vec<CppFontFaceState>>, // [[LoadingFonts]]
    loaded_fonts: GcRefCell<Vec<CppFontFaceState>>,  // [[LoadedFonts]]
    failed_fonts: GcRefCell<Vec<CppFontFaceState>>,  // [[FailedFonts]]

    #[gc(untraced)]
    status: Cell<FontFaceSetLoadStatus>,
    is_pending_on_the_environment: Cell<bool>,
}

impl Wrappable for FontFaceSet {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for FontFaceSet {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl FontFaceSet {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, environment: CppCell) -> Ref<Self> {
        let object = allocate(vm, |_| FontFaceSet {
            event_target: CppEventTarget::new(),
            realm,
            environment,
            this: Cell::new(None),
            font_faces: GcRefCell::new(Vec::new()),
            set_entries: WorldEntries::default(),
            ready_promise: Cell::new(None),
            loading_fonts: GcRefCell::new(Vec::new()),
            loaded_fonts: GcRefCell::new(Vec::new()),
            failed_fonts: GcRefCell::new(Vec::new()),
            status: Cell::new(FontFaceSetLoadStatus::Loaded),
            is_pending_on_the_environment: Cell::new(true),
        });
        object.this.set(Some(object));
        object.event_target.set_owner(object);
        object
            .ready_promise
            .set(Some(with_temporary_execution_context(realm, || {
                create_promise(vm, realm)
            })));
        object
    }

    fn this(&self) -> Ref<Self> {
        self.this.get().expect("a FontFaceSet knows itself once it is created")
    }

    fn as_abi(&self) -> *mut c_void {
        cell_into_abi(self.this())
    }

    fn ready_promise(&self) -> WebIdlPromise {
        self.ready_promise.get().expect("a FontFaceSet has a [[ReadyPromise]]")
    }

    pub(crate) fn relevant_settings_object(&self) -> &CppCell {
        &self.environment
    }

    pub(crate) fn size(&self) -> usize {
        self.font_faces.borrow().len()
    }

    fn did_change_loading_fonts(&self) {
        // SAFETY: The set is live.
        unsafe { web_css_font_face_set_record_loading_fonts(self.as_abi(), self.loading_fonts.borrow().len()) };
    }

    fn entries<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, CppFontFaceState> {
        let entries = MarkedVec::with_capacity(vm, self.font_faces.borrow().len());
        entries.extend_from_slice(&self.font_faces.borrow());
        entries
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-add
    pub(crate) fn add(&self, cx: &mut CallContext, font: CppInterface) -> WebIdlResult<Ref<FontFaceSet>> {
        let font = CppFontFaceState::of_wrapper(font.wrapper());

        // 1. If font is already in the FontFaceSet’s set entries, skip to the last step of this algorithm immediately.
        if contains(&self.font_faces.borrow(), &font) {
            return Ok(self.this());
        }

        // 2. If font is CSS-connected, throw an InvalidModificationError exception and exit this algorithm immediately.
        if font.is_css_connected() {
            return Err(Exception::Dom(
                "InvalidModificationError",
                Utf16String::from_ascii(b"Cannot add a CSS-connected FontFace to a FontFaceSet"),
            ));
        }

        // NB: This method implements steps 3 and 4.
        self.add_css_connected_font(cx.vm(), font);

        // 5. Return the FontFaceSet.
        Ok(self.this())
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-add
    pub(crate) fn add_css_connected_font(&self, vm: &Vm, font: CppFontFaceState) {
        // 3. Add the font argument to the FontFaceSet’s set entries.
        if contains(&self.font_faces.borrow(), &font) {
            return;
        }

        self.font_faces.borrow_mut(vm).push(font.clone());
        let cx = CallContext::new(vm, self.realm);
        self.set_entries.for_each_live(vm, |set| {
            set.set_add(vm, font.wrapper(&context_of_set(&cx, set)));
        });
        // SAFETY: The FontFace and the set are live.
        unsafe { web_css_font_face_did_join_set(font.as_ptr(), self.as_abi()) };

        // 4. If font’s status attribute is "loading"
        if font.is_loading() {
            self.font_started_loading(vm, font);
        }
    }

    /// Removes the font face of a CSS @font-face rule from the set entries.
    pub(crate) fn remove_css_connected_font(&self, vm: &Vm, font: &CppFontFaceState) {
        self.remove_font_face(vm, font, AllowCSSConnected::Yes);
    }

    /// What every FontFaceSet does when a font in it starts to load, or when a font that is loading joins it.
    pub(crate) fn font_started_loading(&self, vm: &Vm, font: CppFontFaceState) {
        // 1. If the FontFaceSet’s [[LoadingFonts]] list is empty, switch the FontFaceSet to loading.
        if self.loading_fonts.borrow().is_empty() {
            self.switch_to_loading(vm);
        }

        // 2. Append font to the FontFaceSet’s [[LoadingFonts]] list.
        self.loading_fonts.borrow_mut(vm).push(font);
        self.did_change_loading_fonts();
    }

    /// What every FontFaceSet does when a font in it loaded, or failed to.
    pub(crate) fn font_finished_loading(&self, vm: &Vm, font: CppFontFaceState, succeeded: bool) {
        // 1. Add font face to the FontFaceSet’s [[LoadedFonts]] list, or to its [[FailedFonts]] list.
        let list = if succeeded {
            &self.loaded_fonts
        } else {
            &self.failed_fonts
        };
        list.borrow_mut(vm).push(font.clone());

        // 2. Remove font face from the FontFaceSet’s [[LoadingFonts]] list. If font was the last item in that list
        //    (and so the list is now empty), switch the FontFaceSet to loaded.
        self.loading_fonts.borrow_mut(vm).retain(|entry| !entry.is(&font));
        self.did_change_loading_fonts();
        if self.loading_fonts.borrow().is_empty() {
            self.switch_to_loaded(vm);
        }
    }

    // https://drafts.csswg.org/css-font-loading/#document-font-face-set
    // The FontFaceSet entries for a document's font source must be initially populated with all the CSS-connected
    // FontFace objects from all of the CSS @font-face rules in the document or shadow root CSS style sheets, in
    // document order. As @font-face rules are added or removed, the corresponding objects must be added or removed and
    // maintain this ordering. Any manually-added FontFace objects must be ordered after the CSS-connected ones.
    pub(crate) fn synchronize_css_connected_font_order(&self, vm: &Vm) {
        unsafe extern "C" fn append_identity(context: *mut c_void, identity: u64) {
            // SAFETY: The context is the list below, which outlives the call.
            unsafe { &mut *context.cast::<Vec<u64>>() }.push(identity);
        }

        let font_faces = self.entries(vm);
        let font_faces = font_faces.values();
        let mut connected_font_faces = HashMap::new();
        for (index, font_face) in font_faces.iter().enumerate() {
            if let Some(identity) = font_face.css_rule_identity() {
                connected_font_faces.insert(identity, index);
            }
        }

        // Every sheet added or removed asks again. Without a face from a rule the walk over their rules finds nothing.
        let mut rule_identities = Vec::new();
        if !connected_font_faces.is_empty() {
            // SAFETY: The set is live, and the list outlives the call.
            let has_window = unsafe {
                web_css_font_face_set_for_each_font_face_rule(
                    self.as_abi(),
                    (&raw mut rule_identities).cast(),
                    append_identity,
                )
            };
            if !has_window {
                return;
            }
        }

        let ordered_font_faces = MarkedVec::with_capacity(vm, font_faces.len());
        let mut seen = HashSet::new();
        for identity in rule_identities {
            if let Some(index) = connected_font_faces.get(&identity)
                && seen.insert(font_faces[*index].as_ptr())
            {
                ordered_font_faces.push(font_faces[*index].clone());
            }
        }
        for font_face in font_faces.iter() {
            if font_face.is_css_connected() && seen.insert(font_face.as_ptr()) {
                ordered_font_faces.push(font_face.clone());
            }
        }
        for font_face in font_faces.iter() {
            if !font_face.is_css_connected() {
                ordered_font_faces.push(font_face.clone());
            }
        }

        let order_changed = ordered_font_faces.len() != font_faces.len()
            || ordered_font_faces
                .values()
                .iter()
                .zip(font_faces.iter())
                .any(|(ordered, entry)| !ordered.is(entry));
        if !order_changed {
            return;
        }

        *self.font_faces.borrow_mut(vm) = ordered_font_faces.to_vec();
        let cx = CallContext::new(vm, self.realm);
        self.set_entries.for_each_live(vm, |set| {
            set.set_clear(vm);
            let cx = context_of_set(&cx, set);
            for font_face in ordered_font_faces.values().iter() {
                set.set_add(vm, font_face.wrapper(&cx));
            }
        });
        let cells: Vec<*mut c_void> = ordered_font_faces
            .values()
            .iter()
            .map(CppFontFaceState::as_ptr)
            .collect();
        // SAFETY: The set and the FontFaces are live, and the list outlives the call.
        unsafe { web_css_font_face_set_synchronize_font_computer(self.as_abi(), cells.as_ptr(), cells.len()) };
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-delete
    pub(crate) fn delete(&self, cx: &mut CallContext, font: CppInterface) -> WebIdlResult<bool> {
        let font = CppFontFaceState::of_wrapper(font.wrapper());
        Ok(self.remove_font_face(cx.vm(), &font, AllowCSSConnected::No))
    }

    fn remove_font_face(&self, vm: &Vm, font: &CppFontFaceState, allow_css_connected: AllowCSSConnected) -> bool {
        // 1. If font is CSS-connected, return false and exit this algorithm immediately.
        if allow_css_connected == AllowCSSConnected::No && font.is_css_connected() {
            return false;
        }

        // 2. Let deleted be the result of removing font from the FontFaceSet’s set entries.
        if !contains(&self.font_faces.borrow(), font) {
            return false;
        }
        self.font_faces.borrow_mut(vm).retain(|entry| !entry.is(font));
        self.did_remove_from_set_entries(vm, font);

        // 3. If font is present in the FontFaceSet’s [[LoadedFonts]], or [[FailedFonts]] lists, remove it.
        self.loaded_fonts.borrow_mut(vm).retain(|entry| !entry.is(font));
        self.failed_fonts.borrow_mut(vm).retain(|entry| !entry.is(font));

        // 4. If font is present in the FontFaceSet’s [[LoadingFonts]] list, remove it.
        //    If font was the last item in that list (and so the list is now empty), switch the FontFaceSet to loaded.
        let was_in_loading_fonts = contains(&self.loading_fonts.borrow(), font);
        self.loading_fonts.borrow_mut(vm).retain(|entry| !entry.is(font));
        self.did_change_loading_fonts();
        if was_in_loading_fonts && self.loading_fonts.borrow().is_empty() {
            self.switch_to_loaded(vm);
        }

        true
    }

    fn did_remove_from_set_entries(&self, vm: &Vm, font: &CppFontFaceState) {
        let cx = CallContext::new(vm, self.realm);
        self.set_entries.for_each_live(vm, |set| {
            set.set_remove(vm, font.wrapper(&context_of_set(&cx, set)));
        });
        // SAFETY: The FontFace and the set are live.
        unsafe { web_css_font_face_did_leave_set(font.as_ptr(), self.as_abi()) };
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-clear
    pub(crate) fn clear(&self, cx: &mut CallContext) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Remove all non-CSS-connected items from the FontFaceSet's set entries,
        //    its [[LoadedFonts]] list, and its [[FailedFonts]] list.
        for font_face in self
            .entries(vm)
            .values()
            .iter()
            .filter(|font_face| !font_face.is_css_connected())
        {
            self.did_remove_from_set_entries(vm, font_face);
        }
        self.font_faces
            .borrow_mut(vm)
            .retain(CppFontFaceState::is_css_connected);
        self.loaded_fonts
            .borrow_mut(vm)
            .retain(CppFontFaceState::is_css_connected);
        self.failed_fonts
            .borrow_mut(vm)
            .retain(CppFontFaceState::is_css_connected);

        // 2. If the FontFaceSet's [[LoadingFonts]] list is non-empty, remove all items from it,
        //    then switch the FontFaceSet to loaded.
        if !self.loading_fonts.borrow().is_empty() {
            self.loading_fonts.borrow_mut(vm).clear();
            self.did_change_loading_fonts();
            self.switch_to_loaded(vm);
        }
        Ok(())
    }

    // https://drafts.csswg.org/css-font-loading/#find-the-matching-font-faces
    fn find_matching_font_faces<'vm>(
        &self,
        vm: &'vm Vm,
        font: &Utf16String,
        text: &Utf16String,
    ) -> WebIdlResult<MarkedVec<'vm, CppFontFaceState>> {
        // 1. Parse font using the CSS value syntax of the font property. If a syntax error occurs, return a syntax
        //    error.
        // If the parsed value is a CSS-wide keyword, return a syntax error.
        // NB: This case is already caught by font_family_names().

        // FIXME: Absolutize all relative lengths against the initial values of the corresponding properties. (For
        //        example, a relative font weight like bolder is evaluated against the initial value normal.)

        // FIXME: 2. If text was not explicitly provided, let it be a string containing a single space character
        //           (U+0020 SPACE).

        // 3. Let font family list be the list of font families parsed from font, and font style be the other font
        //    style attributes parsed from font.
        // NB: The generic font family keywords are skipped.
        let Some(font_family_list) = font_family_names(font) else {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_ascii(b"Unable to parse font"),
            ));
        };

        // 5. Let matched font faces initially be an empty list.
        let matched_font_faces = MarkedVec::new(vm);

        // 6. For each family in font family list, use the font matching rules to select the font faces from available
        //    font faces that match the font style, and add them to matched font faces. The use of the unicodeRange
        //    attribute means that this may be more than just a single font face.
        let font_faces = self.entries(vm);
        let font_faces = font_faces.values();
        let family_names: Vec<_> = font_faces.iter().map(CppFontFaceState::family_name).collect();
        for font_family_name in &font_family_list {
            // FIXME: The matching below is super basic. We currently just match font family names by their string value.
            for (font_face, family_name) in font_faces.iter().zip(&family_names) {
                if family_name == font_family_name && !contains(&matched_font_faces.values(), font_face) {
                    matched_font_faces.push(font_face.clone());
                }
            }
        }

        // FIXME: 7. If matched font faces is empty, set the found faces flag to false. Otherwise, set it to true.

        // 8. For each font face in matched font faces, if its defined unicode-range does not include the codepoint of
        //    at least one character in text, remove it from the list.
        let remaining_font_faces = MarkedVec::with_capacity(vm, matched_font_faces.len());
        for font_face in matched_font_faces.values().iter() {
            if font_face.unicode_ranges_include_any_of(text) {
                remaining_font_faces.push(font_face.clone());
            }
        }

        // 9. Return matched font faces and the found faces flag.
        Ok(remaining_font_faces)
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-load
    pub(crate) fn load(
        &self,
        cx: &mut CallContext,
        font: Utf16String,
        text: Utf16String,
    ) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = self.realm;

        // 1. Let font face set be the FontFaceSet object this method was called on. Let promise be a newly-created
        //    promise object.
        let promise = create_promise(vm, realm);

        // NB: What steps capture is copied, so the strings travel as JS strings.
        let font = PrimitiveString::create(vm, font);
        let text = PrimitiveString::create(vm, text);
        in_parallel(
            vm,
            realm,
            (self.this(), promise, font, text),
            |vm, (this, promise, font, text)| {
                let realm = this.realm;

                // 3. Find the matching font faces from font face set using the font and text arguments passed to the
                //    function, and let font face list be the return value (ignoring the found faces flag). If a syntax
                //    error was returned, reject promise with a SyntaxError exception and terminate these steps.
                let font_face_list = match this.find_matching_font_faces(vm, &font.utf16_string(), &text.utf16_string())
                {
                    Ok(font_face_list) => font_face_list,
                    Err(exception) => {
                        with_temporary_execution_context_and_callbacks(realm, || {
                            reject_promise_with_exception(vm, promise, exception);
                        });
                        return;
                    }
                };

                // NB: The list travels as the array that promise is resolved with, of the wrappers of the font faces.
                let cx = CallContext::new(vm, realm);
                let wrappers = MarkedVec::with_capacity(vm, font_face_list.len());
                for font_face in font_face_list.values().iter() {
                    wrappers.push(font_face.wrapper(&cx));
                }
                let font_face_list = Array::create_from(vm, realm, &wrappers.to_vec());

                // 4. Queue a task to run the following steps synchronously:
                queue_a_task(
                    vm,
                    realm,
                    TaskSource::FontLoading,
                    (realm, promise, font_face_list),
                    |vm, (realm, promise, font_face_list)| {
                        // 1. For all of the font faces in the font face list, call their load() method.
                        let promises = MarkedVec::new(vm);
                        let font_face_list_object = font_face_list.upcast::<Object>();
                        let length = length_of_array_like(vm, &font_face_list_object).must();
                        for index in 0..length {
                            let wrapper =
                                font_face_list_object.get_without_side_effects(vm, &PropertyKey::from(index as u32));
                            promises.push(CppFontFaceState::of_wrapper(wrapper.as_object()).load());
                        }

                        // 2. Resolve promise with the result of waiting for all of the [[FontStatusPromise]]s of each font
                        //    face in the font face list, in order.
                        with_temporary_execution_context_and_callbacks(realm, || {
                            react_to_promise(
                                vm,
                                get_promise_for_wait_for_all(vm, realm, &promises.values()),
                                (promise, font_face_list),
                                Some(|cx, (promise, font_face_list), _| {
                                    resolve_promise(
                                        cx.vm(),
                                        promise,
                                        Value::from_object(font_face_list.upcast::<Object>()),
                                    );
                                    Ok(Value::UNDEFINED)
                                }),
                                Some(|cx, (promise, _), reason| {
                                    reject_promise(cx.vm(), promise, reason);
                                    Ok(Value::UNDEFINED)
                                }),
                            );
                        });
                    },
                );
            },
        );

        // 2. Return promise. Complete the rest of these steps asynchronously.
        Ok(promise)
    }

    // https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-check
    pub(crate) fn check(&self, cx: &mut CallContext, font: Utf16String, text: Utf16String) -> WebIdlResult<bool> {
        // 1. Let font face set be the FontFaceSet object this method was called on.
        // 2. Find the matching font faces from font face set using the font and text arguments passed to the function,
        //    and including system fonts, and let font face list be the returned list of font faces, and found faces be
        //    the returned found faces flag. If a syntax error was returned, throw a SyntaxError exception and terminate
        //    these steps.
        let font_face_list = self.find_matching_font_faces(cx.vm(), &font, &text)?;

        // 3. If font face list is empty, or all fonts in the font face list either have a status attribute of "loaded"
        //    or are system fonts, return true. Otherwise, return false.
        // FIXME: We should check if the font face is a system font here.
        Ok(font_face_list.values().iter().all(CppFontFaceState::is_loaded))
    }

    // https://drafts.csswg.org/css-font-loading/#font-face-set-ready
    pub(crate) fn ready(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // Pending style or layout changes can start a deferred font load. Flush them before returning an already
        // fulfilled promise so switching the FontFaceSet to loading can replace it with a new pending promise.
        if is_promise_fulfilled(self.ready_promise()) {
            // SAFETY: The set is live.
            unsafe { web_css_font_face_set_update_layout(self.as_abi(), false) };
        }

        Ok(self.ready_promise())
    }

    pub(crate) fn status(&self, _: &mut CallContext) -> WebIdlResult<FontFaceSetLoadStatus> {
        Ok(self.status.get())
    }

    // https://drafts.csswg.org/css-font-loading/#fire-a-font-load-event
    fn fire_a_font_load_event(&self, vm: &Vm, name: &str, font_faces: &MarkedVec<'_, CppFontFaceState>) {
        // To fire a font load event named e at a FontFaceSet target with optional font faces means to fire a simple
        // event named e using the FontFaceSetLoadEvent interface that also meets these conditions:
        // 1. The fontfaces attribute is initialized to the result of filtering font faces to only contain FontFace
        //    objects contained in target.
        let fontfaces = MarkedVec::with_capacity(vm, font_faces.len());
        for font_face in font_faces.values().iter() {
            if contains(&self.font_faces.borrow(), font_face) {
                fontfaces.push(font_face.clone());
            }
        }
        let event_init = EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        };
        let event = FontFaceSetLoadEvent::create(vm, self.realm, &Utf16String::from_utf8(name), event_init, &fontfaces);
        event.event().set_is_trusted(true);
        self.event_target.dispatch_event(event.event());
    }

    // https://drafts.csswg.org/css-font-loading/#ref-for-fontfaceset-pending-on-the-environment%E2%91%A1
    pub(crate) fn set_is_pending_on_the_environment(&self, vm: &Vm, is_pending_on_the_environment: bool) {
        if is_pending_on_the_environment == self.is_pending_on_the_environment.get() {
            return;
        }
        self.is_pending_on_the_environment.set(is_pending_on_the_environment);

        // Whenever a FontFaceSet goes from pending on the environment to not pending on the environment, the user agent
        // must run the following steps:
        if !is_pending_on_the_environment {
            // 1. If the FontFaceSet is stuck on the environment and its [[LoadingFonts]] list is empty, switch the
            //    FontFaceSet to loaded.
            // FIXME: We also need to mark empty FontFaceSets as loaded, so that the [[ReadyPromise]] gets resolved.
            //        Spec issue: https://github.com/w3c/csswg-drafts/issues/13538#issuecomment-3933951987
            // AD-HOC: Also switch when nothing has ever entered the LoadingFonts list: an empty set, or a set whose
            //         entries all have deferred unicode-ranges that no rendered codepoint matched. Without this the
            //         ready promise stays pending forever.
            if self.loading_fonts.borrow().is_empty() {
                self.switch_to_loaded(vm);
            }

            // 2. If the FontFaceSet is stuck on the environment, unmark it as such.
            // NB: The AD-HOC condition above covers being stuck, so nothing records it.
        }
    }

    // https://drafts.csswg.org/css-font-loading/#switch-the-fontfaceset-to-loading
    fn switch_to_loading(&self, vm: &Vm) {
        // 1. Let font face set be the given FontFaceSet.
        // 2. Set the status attribute of font face set to "loading".
        self.status.set(FontFaceSetLoadStatus::Loading);

        // 3. If font face set’s [[ReadyPromise]] slot currently holds a fulfilled promise, replace it with a fresh
        //    pending promise.
        if is_promise_fulfilled(self.ready_promise()) {
            let realm = self.realm;
            self.ready_promise.set(Some(with_temporary_execution_context(realm, || {
                create_promise(vm, realm)
            })));
        }

        // 4. Queue a task to fire a font load event named loading at font face set.
        queue_a_task(vm, self.realm, TaskSource::FontLoading, self.this(), |vm, this| {
            this.fire_a_font_load_event(vm, "loading", &MarkedVec::new(vm));
        });
    }

    // https://drafts.csswg.org/css-font-loading/#switch-the-fontfaceset-to-loaded
    fn switch_to_loaded(&self, vm: &Vm) {
        // 1. Let font face set be the given FontFaceSet.
        // 2. If font face set is pending on the environment, mark it as stuck on the environment, and exit this
        //    algorithm.
        if self.is_pending_on_the_environment.get() {
            return;
        }

        // NB: Pending style or layout changes can depend on a recently loaded font or start another deferred font load.
        // SAFETY: The set is live.
        unsafe { web_css_font_face_set_update_layout(self.as_abi(), true) };
        if !self.loading_fonts.borrow().is_empty() {
            return;
        }

        // 3. Set font face set’s status attribute to "loaded".
        let was_loading = self.status.get() == FontFaceSetLoadStatus::Loading;
        self.status.set(FontFaceSetLoadStatus::Loaded);

        // 4. Fulfill font face set’s [[ReadyPromise]] attribute’s value with font face set.
        let realm = self.realm;
        with_temporary_execution_context_and_callbacks(realm, || {
            let cx = CallContext::new(vm, realm);
            let value = self.this().to_js(&cx).expect("wrapping a FontFaceSet can't throw");
            resolve_promise(vm, self.ready_promise(), value);
        });

        // AD-HOC: A set that never switched to loading only fulfills its ready promise. Firing loadingdone for it would
        //         report a load that never happened.
        if !was_loading {
            return;
        }

        // 5. Queue a task to perform the following steps synchronously:
        queue_a_task(vm, realm, TaskSource::FontLoading, self.this(), |vm, this| {
            // 1. Let loaded fonts be the (possibly empty) contents of font face set’s [[LoadedFonts]] slot.
            // 2. Let failed fonts be the (possibly empty) contents of font face set’s [[FailedFonts]] slot.
            // 3. Reset the [[LoadedFonts]] and [[FailedFonts]] slots to empty lists.
            let loaded_fonts = take_font_faces(vm, &this.loaded_fonts);
            let failed_fonts = take_font_faces(vm, &this.failed_fonts);

            // 4. Fire a font load event named loadingdone at font face set with loaded fonts.
            this.fire_a_font_load_event(vm, "loadingdone", &loaded_fonts);

            // 5. If font face set’s failed fonts is non-empty, fire a font load event named loadingerror at font face
            //    set with failed fonts.
            if !failed_fonts.is_empty() {
                this.fire_a_font_load_event(vm, "loadingerror", &failed_fonts);
            }
        });
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum AllowCSSConnected {
    No,
    Yes,
}

impl Setlike for FontFaceSet {
    fn set_entries(&self, cx: &CallContext) -> Gc<Set> {
        self.set_entries.get_or_create(cx, || {
            let set = Set::create(cx.vm(), cx.realm());
            for font_face in self.entries(cx.vm()).values().iter() {
                set.set_add(cx.vm(), font_face.wrapper(cx));
            }
            set
        })
    }

    fn set_entries_size(&self) -> usize {
        self.font_faces.borrow().len()
    }

    fn set_entries_contain(&self, value: Value) -> bool {
        contains(
            &self.font_faces.borrow(),
            &CppFontFaceState::of_wrapper(value.as_object()),
        )
    }
}
