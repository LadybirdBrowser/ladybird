/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::{Cell, OnceCell, RefCell};
use std::ffi::c_void;
use std::{slice, str};

use glib::translate::{IntoGlibPtr, from_glib_borrow};
use gtk::glib;
use gtk::prelude::*;
use gtk::subclass::prelude::*;
use gtk::{gdk, pango};

type NavigateCallback = Option<unsafe extern "C" fn(*const u8, usize, *mut c_void)>;
type DestroyNotify = Option<unsafe extern "C" fn(*mut c_void)>;

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SchemeShouldShowLock {
    Yes,
    No,
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

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct LadybirdLocationEntry {
        pub popover: OnceCell<gtk::Popover>,
        pub list_box: OnceCell<gtk::ListBox>,
        pub suggestions: RefCell<Vec<String>>,
        pub history: RefCell<Vec<String>>,
        pub selected_index: Cell<i32>,
        pub user_text: RefCell<String>,
        pub is_focused: Cell<bool>,
        pub updating_text: Cell<bool>,
        pub(super) on_navigate: RefCell<Option<NavigateHandler>>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for LadybirdLocationEntry {
        const NAME: &'static str = "LadybirdLocationEntry";
        type Type = super::LadybirdLocationEntry;
        type ParentType = gtk::Entry;
    }

    impl ObjectImpl for LadybirdLocationEntry {
        fn constructed(&self) {
            self.parent_constructed();
            let obj = self.obj();
            obj.initialize();
        }

        fn dispose(&self) {
            if let Some(popover) = self.popover.get() {
                popover.popdown();
                popover.unparent();
            }
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

impl LadybirdLocationEntry {
    pub fn new() -> Self {
        glib::Object::builder().build()
    }

    fn initialize(&self) {
        self.set_hexpand(true);
        self.set_placeholder_text(Some("Enter URL or search..."));

        let builder = gtk::Builder::from_resource("/org/ladybird/Ladybird/gtk/location-entry.ui");
        let popover = builder
            .object::<gtk::Popover>("completion_popover")
            .expect("location-entry.ui is missing required object 'completion_popover'");
        let list_box = builder
            .object::<gtk::ListBox>("completion_list_box")
            .expect("location-entry.ui is missing required object 'completion_list_box'");

        popover.set_parent(self);
        let imp = self.imp();
        let _ = imp.popover.set(popover.clone());
        let _ = imp.list_box.set(list_box.clone());

        list_box.connect_row_activated(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_list, row| {
                let index = row.index();
                if index < 0 {
                    return;
                }
                let suggestion = {
                    let suggestions = obj.imp().suggestions.borrow();
                    suggestions.get(index as usize).cloned()
                };
                if let Some(suggestion) = suggestion {
                    obj.set_entry_text_suppressed(&suggestion, false);
                    obj.hide_completions();
                    obj.navigate();
                }
            }
        ));

        self.connect_changed(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                obj.on_changed();
            }
        ));

        self.connect_activate(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                obj.hide_completions();
                obj.navigate();
            }
        ));

        let key_controller = gtk::EventControllerKey::new();
        key_controller.connect_key_pressed(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            #[upgrade_or]
            glib::Propagation::Proceed,
            move |_controller, key, _code, _state| {
                if let Some(popover) = obj.imp().popover.get()
                    && !popover.is_visible()
                {
                    return glib::Propagation::Proceed;
                }

                match key {
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
                        let user_text = obj.imp().user_text.borrow().clone();
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

        let focus_controller = gtk::EventControllerFocus::new();
        focus_controller.connect_enter(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                obj.imp().is_focused.set(true);
                obj.set_attributes(&pango::AttrList::new());
            }
        ));
        focus_controller.connect_leave(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            move |_| {
                obj.imp().is_focused.set(false);
                obj.hide_completions();
                obj.update_display_attributes();
            }
        ));
        self.add_controller(focus_controller);
    }

    fn set_entry_text_suppressed(&self, text: &str, move_cursor_to_end: bool) {
        let imp = self.imp();
        imp.updating_text.set(true);
        self.set_text(text);
        if move_cursor_to_end {
            self.set_position(-1);
        }
        imp.updating_text.set(false);
    }

    fn set_security_icon_internal(&self, should_show_lock: SchemeShouldShowLock, has_url: bool) {
        if !has_url {
            self.set_icon_from_icon_name(gtk::EntryIconPosition::Primary, None);
            self.set_icon_tooltip_text(gtk::EntryIconPosition::Primary, None);
            return;
        }

        match should_show_lock {
            SchemeShouldShowLock::Yes => {
                self.set_icon_from_icon_name(
                    gtk::EntryIconPosition::Primary,
                    Some("channel-secure-symbolic"),
                );
                self.set_icon_tooltip_text(
                    gtk::EntryIconPosition::Primary,
                    Some("Secure connection"),
                );
            }
            SchemeShouldShowLock::No => {
                self.set_icon_from_icon_name(
                    gtk::EntryIconPosition::Primary,
                    Some("channel-insecure-symbolic"),
                );
                self.set_icon_tooltip_text(
                    gtk::EntryIconPosition::Primary,
                    Some("Insecure connection"),
                );
            }
        }
    }

    pub fn set_url(&self, url: Option<&str>, should_show_lock: SchemeShouldShowLock) {
        let text = url.unwrap_or_default();
        self.set_entry_text_suppressed(text, false);

        self.set_security_icon_internal(should_show_lock, !text.is_empty());

        if !self.imp().is_focused.get() {
            self.update_display_attributes();
        }
    }

    pub fn clear_text(&self) {
        self.set_entry_text_suppressed("", false);
        self.set_attributes(&pango::AttrList::new());
        self.set_security_icon_internal(SchemeShouldShowLock::No, false);
    }

    fn update_display_attributes(&self) {
        // Keep this as a no-op style reset for now; domain highlighting can be added back
        // once URL part extraction is bridged to Rust.
        self.set_attributes(&pango::AttrList::new());
    }

    fn on_changed(&self) {
        let imp = self.imp();
        if !imp.is_focused.get() || imp.updating_text.get() {
            return;
        }

        let text = self.text().to_string();
        if text.is_empty() {
            self.hide_completions();
            return;
        }

        *imp.user_text.borrow_mut() = text.clone();

        let mut suggestions = Vec::new();
        for candidate in imp.history.borrow().iter() {
            if candidate.starts_with(&text) && candidate != &text {
                suggestions.push(candidate.clone());
                if suggestions.len() >= 20 {
                    break;
                }
            }
        }

        if suggestions.is_empty() {
            self.hide_completions();
            return;
        }

        *imp.suggestions.borrow_mut() = suggestions;
        imp.selected_index.set(-1);
        self.show_completions();
    }

    fn navigate(&self) {
        let text = self.text().to_string();
        if text.is_empty() {
            return;
        }

        {
            let mut history = self.imp().history.borrow_mut();
            history.retain(|item| item != &text);
            history.insert(0, text.clone());
            if history.len() > 100 {
                history.truncate(100);
            }
        }

        if let Some(handler) = self.imp().on_navigate.borrow().as_ref()
            && let Some(callback) = handler.callback
        {
            unsafe { callback(text.as_ptr(), text.len(), handler.user_data) };
        }
    }

    fn show_completions(&self) {
        let imp = self.imp();
        let Some(list_box) = imp.list_box.get() else {
            return;
        };
        let Some(popover) = imp.popover.get() else {
            return;
        };

        while let Some(child) = list_box.first_child() {
            list_box.remove(&child);
        }

        for suggestion in imp.suggestions.borrow().iter() {
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

        let width = self.width();
        if width > 0 {
            popover.set_size_request(width, -1);
        }

        popover.popup();
    }

    fn hide_completions(&self) {
        let imp = self.imp();
        imp.suggestions.borrow_mut().clear();
        imp.selected_index.set(-1);
        if let Some(popover) = imp.popover.get() {
            popover.popdown();
        }
    }

    fn move_selection(&self, delta: i32) {
        let imp = self.imp();

        let suggestions_len = imp.suggestions.borrow().len() as i32;
        if suggestions_len == 0 {
            return;
        }

        let mut new_index = imp.selected_index.get() + delta;
        if new_index < -1 {
            new_index = suggestions_len - 1;
        }
        if new_index >= suggestions_len {
            new_index = -1;
        }
        imp.selected_index.set(new_index);

        let Some(list_box) = imp.list_box.get() else {
            return;
        };

        if new_index >= 0 {
            if let Some(row) = list_box.row_at_index(new_index) {
                list_box.select_row(Some(&row));
                self.apply_selected_suggestion();
            }
        } else {
            list_box.unselect_all();
            let user_text = imp.user_text.borrow().clone();
            self.set_entry_text_suppressed(&user_text, true);
        }
    }

    fn apply_selected_suggestion(&self) {
        let imp = self.imp();
        let index = imp.selected_index.get();
        if index < 0 {
            return;
        }

        let Some(suggestion) = imp.suggestions.borrow().get(index as usize).cloned() else {
            return;
        };

        self.set_entry_text_suppressed(&suggestion, true);
    }

    fn set_on_navigate_handler(
        &self,
        callback: NavigateCallback,
        user_data: *mut c_void,
        destroy: DestroyNotify,
    ) {
        *self.imp().on_navigate.borrow_mut() = Some(NavigateHandler {
            callback,
            user_data,
            destroy,
        });
    }
}

unsafe fn entry_from_ptr(
    entry: *mut LadybirdLocationEntry,
) -> glib::translate::Borrowed<LadybirdLocationEntry> {
    unsafe {
        from_glib_borrow(
            entry.cast::<glib::subclass::basic::InstanceStruct<imp::LadybirdLocationEntry>>(),
        )
    }
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_set_url(
    entry: *mut LadybirdLocationEntry,
    url: *const u8,
    url_len: usize,
    should_show_lock: SchemeShouldShowLock,
) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.set_url(
        unsafe { utf8_bytes_to_option(url, url_len) },
        should_show_lock,
    );
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_clear_text(entry: *mut LadybirdLocationEntry) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.clear_text();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_focus_and_select_all(
    entry: *mut LadybirdLocationEntry,
) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.grab_focus();
    entry.select_region(0, -1);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_location_entry_set_on_navigate(
    entry: *mut LadybirdLocationEntry,
    callback: NavigateCallback,
    user_data: *mut c_void,
    destroy: DestroyNotify,
) {
    let entry = unsafe { entry_from_ptr(entry) };
    entry.set_on_navigate_handler(callback, user_data, destroy);
}
