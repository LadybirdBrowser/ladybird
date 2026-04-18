/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use adw::subclass::prelude::*;
use glib::object::Cast;
use glib::translate::{IntoGlibPtr, ToGlibPtr, from_glib_borrow};
use gtk::{CompositeTemplate, TemplateChild, gio, glib};

mod imp {
    use super::*;

    #[derive(Default, CompositeTemplate)]
    #[template(resource = "/org/ladybird/Ladybird/gtk/browser-window.ui")]
    pub struct LadybirdBrowserWindow {
        #[template_child]
        pub toolbar_view: TemplateChild<adw::ToolbarView>,
        #[template_child]
        pub header_bar: TemplateChild<adw::HeaderBar>,
        #[template_child]
        pub back_button: TemplateChild<gtk::Button>,
        #[template_child]
        pub forward_button: TemplateChild<gtk::Button>,
        #[template_child]
        pub reload_button: TemplateChild<gtk::Button>,
        #[template_child]
        pub restore_button: TemplateChild<gtk::Button>,
        #[template_child]
        pub menu_button: TemplateChild<gtk::MenuButton>,
        #[template_child]
        pub tab_view: TemplateChild<adw::TabView>,
        #[template_child]
        pub tab_bar: TemplateChild<adw::TabBar>,
        #[template_child]
        pub toast_overlay: TemplateChild<adw::ToastOverlay>,
        #[template_child]
        pub find_bar_revealer: TemplateChild<gtk::Revealer>,
        #[template_child]
        pub find_entry: TemplateChild<gtk::SearchEntry>,
        #[template_child]
        pub find_result_label: TemplateChild<gtk::Label>,
        #[template_child]
        pub zoom_reset_button: TemplateChild<gtk::Button>,
        #[template_child]
        pub zoom_label: TemplateChild<gtk::Label>,
        #[template_child]
        pub devtools_banner: TemplateChild<adw::Banner>,
        #[template_child]
        pub hamburger_menu: TemplateChild<gio::Menu>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for LadybirdBrowserWindow {
        const NAME: &'static str = "LadybirdBrowserWindow";
        type Type = super::LadybirdBrowserWindow;
        type ParentType = adw::ApplicationWindow;

        fn class_init(klass: &mut Self::Class) {
            klass.bind_template();
        }

        fn instance_init(obj: &glib::subclass::InitializingObject<Self>) {
            obj.init_template();
        }
    }

    impl ObjectImpl for LadybirdBrowserWindow {}
    impl WidgetImpl for LadybirdBrowserWindow {}
    impl WindowImpl for LadybirdBrowserWindow {}
    impl ApplicationWindowImpl for LadybirdBrowserWindow {}
    impl AdwApplicationWindowImpl for LadybirdBrowserWindow {}
}

glib::wrapper! {
    pub struct LadybirdBrowserWindow(ObjectSubclass<imp::LadybirdBrowserWindow>)
        @extends adw::ApplicationWindow, gtk::ApplicationWindow, gtk::Window, gtk::Widget,
        @implements gtk::Accessible, gtk::Buildable, gtk::ConstraintTarget,
                    gtk::Native, gtk::Root, gtk::ShortcutManager,
                    gio::ActionGroup, gio::ActionMap;
}

impl LadybirdBrowserWindow {
    pub fn new(app: &adw::Application) -> Self {
        glib::Object::builder().property("application", app).build()
    }

    pub fn header_bar(&self) -> adw::HeaderBar {
        self.imp().header_bar.get()
    }
    pub fn tab_view(&self) -> adw::TabView {
        self.imp().tab_view.get()
    }
    pub fn restore_button(&self) -> gtk::Button {
        self.imp().restore_button.get()
    }
    pub fn zoom_label(&self) -> gtk::Label {
        self.imp().zoom_label.get()
    }
    pub fn devtools_banner(&self) -> adw::Banner {
        self.imp().devtools_banner.get()
    }
    pub fn find_bar_revealer(&self) -> gtk::Revealer {
        self.imp().find_bar_revealer.get()
    }
    pub fn find_entry(&self) -> gtk::SearchEntry {
        self.imp().find_entry.get()
    }
    pub fn find_result_label(&self) -> gtk::Label {
        self.imp().find_result_label.get()
    }
    pub fn hamburger_menu(&self) -> gio::Menu {
        self.imp().hamburger_menu.get()
    }
    pub fn toast_overlay(&self) -> adw::ToastOverlay {
        self.imp().toast_overlay.get()
    }
    pub fn menu_button(&self) -> gtk::MenuButton {
        self.imp().menu_button.get()
    }
}

unsafe fn window_from_ptr(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> glib::translate::Borrowed<LadybirdBrowserWindow> {
    unsafe {
        from_glib_borrow(
            window.cast::<glib::subclass::basic::InstanceStruct<imp::LadybirdBrowserWindow>>(),
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn create_browser_window_widget(
    app: *mut adw::ffi::AdwApplication,
) -> *mut adw::ffi::AdwApplicationWindow {
    // FIXME: We shouldn't need to init here
    if !gtk::is_initialized() {
        gtk::init().expect("Failed to initialize GTK");
    }
    if !adw::is_initialized() {
        adw::init().expect("Failed to initialize Adw");
    }
    let app = unsafe { from_glib_borrow::<_, adw::Application>(app) };
    LadybirdBrowserWindow::new(&app)
        .upcast::<adw::ApplicationWindow>()
        .into_glib_ptr()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_header_bar(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut adw::ffi::AdwHeaderBar {
    let window = unsafe { window_from_ptr(window) };
    window.header_bar().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_tab_view(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut adw::ffi::AdwTabView {
    let window = unsafe { window_from_ptr(window) };
    window.tab_view().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_restore_button(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut gtk::ffi::GtkButton {
    let window = unsafe { window_from_ptr(window) };
    window.restore_button().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_zoom_label(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut gtk::ffi::GtkLabel {
    let window = unsafe { window_from_ptr(window) };
    window.zoom_label().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_devtools_banner(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut adw::ffi::AdwBanner {
    let window = unsafe { window_from_ptr(window) };
    window.devtools_banner().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_find_bar_revealer(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut gtk::ffi::GtkRevealer {
    let window = unsafe { window_from_ptr(window) };
    window.find_bar_revealer().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_find_entry(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut gtk::ffi::GtkSearchEntry {
    let window = unsafe { window_from_ptr(window) };
    window.find_entry().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_find_result_label(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut gtk::ffi::GtkLabel {
    let window = unsafe { window_from_ptr(window) };
    window.find_result_label().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_hamburger_menu(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut gio::ffi::GMenu {
    let window = unsafe { window_from_ptr(window) };
    window.hamburger_menu().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_toast_overlay(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut adw::ffi::AdwToastOverlay {
    let window = unsafe { window_from_ptr(window) };
    window.toast_overlay().to_glib_none().0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn browser_window_menu_button(
    window: *mut adw::ffi::AdwApplicationWindow,
) -> *mut gtk::ffi::GtkMenuButton {
    let window = unsafe { window_from_ptr(window) };
    window.menu_button().to_glib_none().0
}
