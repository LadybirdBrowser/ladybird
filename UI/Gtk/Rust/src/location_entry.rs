/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::RefCell;
use std::ffi::c_void;
use std::{ptr, slice, str};

use glib::translate::{IntoGlibPtr, from_glib_borrow};
use gtk::glib;
use gtk::prelude::*;
use gtk::subclass::prelude::*;
use gtk::{CompositeTemplate, TemplateChild, gdk, pango};

type NavigateCallback = Option<unsafe extern "C" fn(*const u8, usize, *mut c_void)>;
type DestroyNotify = Option<unsafe extern "C" fn(*mut c_void)>;

type AutocompleteCallback = unsafe extern "C" fn(*const *const u8, *const usize, usize, *mut c_void);

unsafe extern "C" {
    fn ladybird_location_entry_placeholder_alloc(out_len: *mut usize) -> *mut u8;
    fn ladybird_location_entry_sanitize_url_alloc(input: *const u8, input_len: usize, out_len: *mut usize) -> *mut u8;
    fn ladybird_location_entry_break_url_into_parts(
        input: *const u8,
        input_len: usize,
        scheme_and_subdomain_len: *mut usize,
        effective_tld_plus_one_len: *mut usize,
    ) -> bool;
    fn ladybird_string_free(p: *mut u8);

    fn ladybird_autocomplete_new(
        callback: AutocompleteCallback,
        user_data: *mut c_void,
        destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> *mut c_void;
    fn ladybird_autocomplete_free(p: *mut c_void);
    fn ladybird_autocomplete_query(p: *mut c_void, query: *const u8, query_len: usize);
}

#[derive(Default)]
struct NavigateHandler {
    callback: NavigateCallback,
    user_data: *mut c_void,
    destroy: DestroyNotify,
}

impl Drop for NavigateHandler {
    fn drop(&mut self) {
        if let Some(destroy) = self.destroy {
            unsafe { destroy(self.user_data) };
        }
    }
}

struct AutocompleteHandle(*mut c_void);

impl AutocompleteHandle {
    const fn null() -> Self {
        Self(ptr::null_mut())
    }
}

impl Drop for AutocompleteHandle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { ladybird_autocomplete_free(self.0) };
        }
    }
}

pub struct LocationEntryState {
    autocomplete: AutocompleteHandle,
    suggestions: Vec<String>,
    selected_index: i32,
    user_text: String,
    is_focused: bool,
    updating_text: bool,
    on_navigate: Option<NavigateHandler>,
}

impl LocationEntryState {
    fn new() -> Self {
        Self {
            autocomplete: AutocompleteHandle::null(),
            suggestions: Vec::new(),
            selected_index: -1,
            user_text: String::new(),
            is_focused: false,
            updating_text: false,
            on_navigate: None,
        }
    }
}

unsafe extern "C" fn autocomplete_complete_trampoline(
    texts: *const *const u8,
    lens: *const usize,
    count: usize,
    user_data: *mut c_void,
) {
    if user_data.is_null() {
        return;
    }
    let weak_ref = unsafe { &*(user_data as *const glib::WeakRef<LadybirdLocationEntry>) };
    let Some(entry) = weak_ref.upgrade() else {
        return;
    };

    let mut suggestions = Vec::with_capacity(count);
    for i in 0..count {
        let ptr = unsafe { *texts.add(i) };
        let len = unsafe { *lens.add(i) };
        let bytes = unsafe { slice::from_raw_parts(ptr, len) };
        if let Ok(s) = str::from_utf8(bytes) {
            suggestions.push(s.to_owned());
        }
    }

    entry.on_autocomplete_query_complete(suggestions);
}

unsafe extern "C" fn autocomplete_destroy_trampoline(user_data: *mut c_void) {
    if !user_data.is_null() {
        drop(unsafe { Box::from_raw(user_data as *mut glib::WeakRef<LadybirdLocationEntry>) });
    }
}

mod imp {
    use super::*;

    #[derive(Default, CompositeTemplate)]
    #[template(resource = "/org/ladybird/Ladybird/gtk/location-entry.ui")]
    pub struct LadybirdLocationEntry {
        #[template_child]
        pub completion_popover: TemplateChild<gtk::Popover>,
        #[template_child]
        pub completion_list_box: TemplateChild<gtk::ListBox>,
        pub popover_attached: RefCell<bool>,
        pub state: RefCell<Option<LocationEntryState>>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for LadybirdLocationEntry {
        const NAME: &'static str = "LadybirdLocationEntry";
        type Type = super::LadybirdLocationEntry;
        type ParentType = gtk::Entry;

        fn class_init(klass: &mut Self::Class) {
            klass.bind_template();
        }

        fn instance_init(obj: &glib::subclass::InitializingObject<Self>) {
            obj.init_template();
        }
    }

    impl ObjectImpl for LadybirdLocationEntry {
        fn constructed(&self) {
            self.parent_constructed();
            let obj = self.obj();
            obj.initialize();
        }

        fn dispose(&self) {
            if self.popover_attached.replace(false) {
                let popover = self.completion_popover.get();
                popover.popdown();
                popover.unparent();
            }
            *self.state.borrow_mut() = None;
        }
    }

    impl WidgetImpl for LadybirdLocationEntry {}
    impl EditableImpl for LadybirdLocationEntry {}
    impl EntryImpl for LadybirdLocationEntry {}
}

glib::wrapper! {
    pub struct LadybirdLocationEntry(ObjectSubclass<imp::LadybirdLocationEntry>)
        @extends gtk::Entry, gtk::Widget,
        @implements gtk::Accessible, gtk::Buildable, gtk::ConstraintTarget,
                    gtk::Editable, gtk::CellEditable;
}

impl Default for LadybirdLocationEntry {
    fn default() -> Self {
        Self::new()
    }
}

impl LadybirdLocationEntry {
    fn with_state<R>(&self, f: impl FnOnce(&LocationEntryState) -> R) -> R {
        let state = self.imp().state.borrow();
        f(state.as_ref().expect("LocationEntryState is uninitialized"))
    }

    fn with_state_mut<R>(&self, f: impl FnOnce(&mut LocationEntryState) -> R) -> R {
        let mut state = self.imp().state.borrow_mut();
        f(state.as_mut().expect("LocationEntryState is uninitialized"))
    }

    fn set_entry_text_suppressed(&self, text: &str, move_cursor_to_end: bool) {
        self.with_state_mut(|state| state.updating_text = true);
        self.set_text(text);
        if move_cursor_to_end {
            self.set_position(-1);
        }
        self.with_state_mut(|state| state.updating_text = false);
    }

    fn initialize(&self) {
        let imp = self.imp();
        *imp.state.borrow_mut() = Some(LocationEntryState::new());

        let mut placeholder_len: usize = 0;
        let placeholder_ptr = unsafe { ladybird_location_entry_placeholder_alloc(&mut placeholder_len) };
        if !placeholder_ptr.is_null() {
            let placeholder_bytes = unsafe { slice::from_raw_parts(placeholder_ptr, placeholder_len) };
            if let Ok(placeholder) = str::from_utf8(placeholder_bytes) {
                self.set_placeholder_text(Some(placeholder));
            }
            unsafe { ladybird_string_free(placeholder_ptr) };
        }

        let popover = imp.completion_popover.get();
        popover.set_parent(self);
        *imp.popover_attached.borrow_mut() = true;
        let list_box = imp.completion_list_box.get();

        // Clicking a suggestion navigates to it
        list_box.connect_row_activated(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_list, row| {
                let index = row.index();
                let suggestion = obj.with_state(|state| {
                    if index >= 0 && (index as usize) < state.suggestions.len() {
                        Some(state.suggestions[index as usize].clone())
                    } else {
                        None
                    }
                });
                if let Some(suggestion) = suggestion {
                    obj.set_entry_text_suppressed(&suggestion, false);
                    obj.hide_completions();
                    obj.navigate();
                }
            }
        ));

        // Autocomplete results callback
        let weak: Box<glib::WeakRef<LadybirdLocationEntry>> = Box::new(self.downgrade());
        let user_data = Box::into_raw(weak) as *mut c_void;
        let autocomplete_ptr = unsafe {
            ladybird_autocomplete_new(
                autocomplete_complete_trampoline,
                user_data,
                Some(autocomplete_destroy_trampoline),
            )
        };
        self.with_state_mut(|state| state.autocomplete = AutocompleteHandle(autocomplete_ptr));

        // Text changed -> query autocomplete
        self.connect_changed(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                let (is_focused, updating_text, autocomplete) =
                    obj.with_state(|state| (state.is_focused, state.updating_text, state.autocomplete.0));
                if !is_focused || updating_text {
                    return;
                }
                let text = obj.text().to_string();
                if text.is_empty() {
                    obj.hide_completions();
                    return;
                }
                obj.with_state_mut(|state| state.user_text = text.clone());
                if !autocomplete.is_null() {
                    unsafe { ladybird_autocomplete_query(autocomplete, text.as_ptr(), text.len()) };
                }
            }
        ));

        // Enter navigates
        self.connect_activate(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                obj.hide_completions();
                obj.navigate();
            }
        ));

        // Key controller for Up/Down/Escape
        let key_controller = gtk::EventControllerKey::new();
        key_controller.connect_key_pressed(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            #[upgrade_or]
            glib::Propagation::Proceed,
            move |_controller, keyval, _code, _state| {
                if !obj.imp().completion_popover.get().is_visible() {
                    return glib::Propagation::Proceed;
                }

                match keyval {
                    gdk::Key::Down => {
                        obj.move_selection(1);
                        glib::Propagation::Stop
                    }
                    gdk::Key::Up => {
                        obj.move_selection(-1);
                        glib::Propagation::Stop
                    }
                    gdk::Key::Escape => {
                        obj.hide_completions();
                        let user_text = obj.with_state(|state| state.user_text.clone());
                        if !user_text.is_empty() {
                            obj.set_entry_text_suppressed(&user_text, false);
                        }
                        glib::Propagation::Stop
                    }
                    _ => glib::Propagation::Proceed,
                }
            }
        ));
        self.add_controller(key_controller);

        // Focus tracking
        let focus_controller = gtk::EventControllerFocus::new();
        focus_controller.connect_enter(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                obj.with_state_mut(|state| state.is_focused = true);
                obj.set_attributes(&pango::AttrList::new());
            }
        ));
        focus_controller.connect_leave(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                obj.with_state_mut(|state| state.is_focused = false);
                obj.hide_completions();
                obj.update_display_attributes();
            }
        ));
        self.add_controller(focus_controller);
    }

    // Public API

    pub fn new() -> Self {
        glib::Object::builder().build()
    }

    pub fn set_url(&self, url: Option<&str>) {
        self.set_entry_text_suppressed(url.unwrap_or(""), false);

        // Extract scheme for security icon
        let scheme = url.and_then(|s| s.split_once(':').map(|(scheme, _)| scheme));
        self.set_security_icon(scheme);

        if !self.with_state(|state| state.is_focused) {
            self.update_display_attributes();
        }
    }

    pub fn set_plain_text(&self, text: Option<&str>) {
        self.set_entry_text_suppressed(text.unwrap_or(""), false);
        self.set_attributes(&pango::AttrList::new());
        self.set_security_icon(None);
    }

    pub fn set_security_icon(&self, scheme: Option<&str>) {
        let Some(scheme) = scheme else {
            self.set_icon_from_icon_name(gtk::EntryIconPosition::Primary, None);
            return;
        };

        if scheme == "https" {
            self.set_icon_from_icon_name(gtk::EntryIconPosition::Primary, Some("channel-secure-symbolic"));
            self.set_icon_tooltip_text(gtk::EntryIconPosition::Primary, Some("Secure connection"));
        } else if scheme == "file" || scheme == "resource" || scheme == "about" || scheme == "data" {
            self.set_icon_from_icon_name(gtk::EntryIconPosition::Primary, None);
        } else {
            self.set_icon_from_icon_name(gtk::EntryIconPosition::Primary, Some("channel-insecure-symbolic"));
            self.set_icon_tooltip_text(gtk::EntryIconPosition::Primary, Some("Insecure connection"));
        }
    }

    pub fn focus_and_select_all(&self) {
        self.grab_focus();
        self.select_region(0, -1);
    }

    pub fn set_on_navigate(&self, callback: NavigateCallback, user_data: *mut c_void, destroy: DestroyNotify) {
        self.with_state_mut(|state| {
            state.on_navigate = Some(NavigateHandler {
                callback,
                user_data,
                destroy,
            });
        });
    }

    // Internal helpers

    fn update_display_attributes(&self) {
        let text = self.text().to_string();
        if text.is_empty() {
            self.set_attributes(&pango::AttrList::new());
            return;
        }

        let mut scheme_and_subdomain_len: usize = 0;
        let mut effective_tld_plus_one_len: usize = 0;
        let parsed = unsafe {
            ladybird_location_entry_break_url_into_parts(
                text.as_ptr(),
                text.len(),
                &mut scheme_and_subdomain_len,
                &mut effective_tld_plus_one_len,
            )
        };

        let attrs = pango::AttrList::new();
        if parsed {
            let dim = pango::AttrInt::new_foreground_alpha(40000);
            attrs.insert(dim);

            let highlight_start = scheme_and_subdomain_len as u32;
            let highlight_end = highlight_start + effective_tld_plus_one_len as u32;

            if highlight_start < highlight_end {
                let mut domain_alpha = pango::AttrInt::new_foreground_alpha(65535);
                domain_alpha.set_start_index(highlight_start);
                domain_alpha.set_end_index(highlight_end);
                attrs.insert(domain_alpha);

                let mut semi_bold = pango::AttrInt::new_weight(pango::Weight::Medium);
                semi_bold.set_start_index(highlight_start);
                semi_bold.set_end_index(highlight_end);
                attrs.insert(semi_bold);
            }
        }

        self.set_attributes(&attrs);
    }

    fn navigate(&self) {
        let text = self.text().to_string();
        if text.is_empty() {
            return;
        }
        let mut url_len: usize = 0;
        let url = unsafe { ladybird_location_entry_sanitize_url_alloc(text.as_ptr(), text.len(), &mut url_len) };
        if url.is_null() {
            return;
        }

        let on_navigate = self.with_state(|state| {
            state
                .on_navigate
                .as_ref()
                .and_then(|handler| handler.callback.map(|cb| (cb, handler.user_data)))
        });
        if let Some((callback, user_data)) = on_navigate {
            unsafe { callback(url, url_len, user_data) };
        }

        unsafe { ladybird_string_free(url) };
    }

    fn on_autocomplete_query_complete(&self, suggestions: Vec<String>) {
        let is_focused = self.with_state(|state| state.is_focused);
        if suggestions.is_empty() || !is_focused {
            self.hide_completions();
            return;
        }
        self.with_state_mut(|state| {
            state.suggestions = suggestions;
            state.selected_index = -1;
        });
        self.show_completions();
    }

    fn show_completions(&self) {
        let imp = self.imp();
        let list_box = imp.completion_list_box.get();
        let popover = imp.completion_popover.get();

        while let Some(child) = list_box.first_child() {
            list_box.remove(&child);
        }

        let suggestions: Vec<String> = self.with_state(|state| state.suggestions.clone());
        for suggestion in &suggestions {
            let label = gtk::Label::new(Some(suggestion));
            label.set_xalign(0.0);
            label.set_ellipsize(pango::EllipsizeMode::End);
            label.set_margin_start(8);
            label.set_margin_end(8);
            label.set_margin_top(4);
            label.set_margin_bottom(4);
            list_box.append(&label);
        }

        list_box.unselect_all();

        let entry_width = self.width();
        if entry_width > 0 {
            popover.set_size_request(entry_width, -1);
        }

        popover.popup();
    }

    fn hide_completions(&self) {
        self.with_state_mut(|state| {
            state.suggestions.clear();
            state.selected_index = -1;
        });
        self.imp().completion_popover.get().popdown();
    }

    fn move_selection(&self, delta: i32) {
        let suggestions_len = self.with_state(|state| state.suggestions.len()) as i32;
        if suggestions_len == 0 {
            return;
        }

        let new_index = self.with_state_mut(|state| {
            let mut new_index = state.selected_index + delta;
            if new_index < -1 {
                new_index = suggestions_len - 1;
            }
            if new_index >= suggestions_len {
                new_index = -1;
            }
            state.selected_index = new_index;
            new_index
        });

        let list_box = self.imp().completion_list_box.get();

        if new_index >= 0 {
            let row = list_box.row_at_index(new_index);
            list_box.select_row(row.as_ref());
            self.apply_selected_suggestion();
        } else {
            list_box.unselect_all();
            let user_text = self.with_state(|state| state.user_text.clone());
            self.set_entry_text_suppressed(&user_text, true);
        }
    }

    fn apply_selected_suggestion(&self) {
        let suggestion = self.with_state(|state| {
            if state.selected_index < 0 || (state.selected_index as usize) >= state.suggestions.len() {
                return None;
            }
            Some(state.suggestions[state.selected_index as usize].clone())
        });

        if let Some(suggestion) = suggestion {
            self.set_entry_text_suppressed(&suggestion, true);
        }
    }
}

unsafe fn entry_from_ptr(entry: *mut LadybirdLocationEntry) -> glib::translate::Borrowed<LadybirdLocationEntry> {
    unsafe { from_glib_borrow(entry.cast::<glib::subclass::basic::InstanceStruct<imp::LadybirdLocationEntry>>()) }
}

unsafe fn utf8_bytes_to_option<'a>(value: *const u8, len: usize) -> Option<&'a str> {
    if value.is_null() {
        return None;
    }

    let bytes = unsafe { slice::from_raw_parts(value, len) };
    str::from_utf8(bytes).ok()
}

#[unsafe(no_mangle)]
pub extern "C" fn ladybird_location_entry_new() -> *mut LadybirdLocationEntry {
    let raw: *mut glib::subclass::basic::InstanceStruct<imp::LadybirdLocationEntry> =
        LadybirdLocationEntry::new().into_glib_ptr();
    raw.cast::<LadybirdLocationEntry>()
}

/// # Safety
///
/// `entry` must be a valid pointer to a `LadybirdLocationEntry`. `url` may be null;
/// otherwise it must point to `url_len` bytes of UTF-8 data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_set_url(
    entry: *mut LadybirdLocationEntry,
    url: *const u8,
    url_len: usize,
) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.set_url(unsafe { utf8_bytes_to_option(url, url_len) });
}

/// # Safety
///
/// `entry` must be a valid pointer to a `LadybirdLocationEntry`. `text` may be null;
/// otherwise it must point to `text_len` bytes of UTF-8 data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_set_text(
    entry: *mut LadybirdLocationEntry,
    text: *const u8,
    text_len: usize,
) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.set_plain_text(unsafe { utf8_bytes_to_option(text, text_len) });
}

/// # Safety
///
/// `entry` must be a valid pointer to a `LadybirdLocationEntry`. `scheme` may be null;
/// otherwise it must point to `scheme_len` bytes of UTF-8 data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_set_security_icon(
    entry: *mut LadybirdLocationEntry,
    scheme: *const u8,
    scheme_len: usize,
) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.set_security_icon(unsafe { utf8_bytes_to_option(scheme, scheme_len) });
}

/// # Safety
///
/// `entry` must be a valid pointer to a `LadybirdLocationEntry`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_focus_and_select_all(entry: *mut LadybirdLocationEntry) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.focus_and_select_all();
}

/// # Safety
///
/// `entry` must be a valid pointer to a `LadybirdLocationEntry`. `callback` may be null,
/// in which case `user_data` and `destroy` are ignored. Otherwise the entry takes ownership
/// of `user_data`, calling `destroy(user_data)` when the entry is dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_set_on_navigate(
    entry: *mut LadybirdLocationEntry,
    callback: NavigateCallback,
    user_data: *mut c_void,
    destroy: DestroyNotify,
) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.set_on_navigate(callback, user_data, destroy);
}
