/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Gtk/Events.h>
#include <UI/Gtk/WebContentView.h>

#include <adwaita.h>
#include <gdk/gdk.h>

#define LADYBIRD_WEB_VIEW(obj) (reinterpret_cast<LadybirdWebView*>(obj))
#define LADYBIRD_TYPE_WEB_VIEW (ladybird_web_view_get_type())

struct LadybirdWebView {
    GtkWidget parent_instance;
    Ladybird::WebContentView* impl { nullptr };
};

struct LadybirdWebViewClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_FINAL_TYPE(LadybirdWebView, ladybird_web_view, GTK_TYPE_WIDGET)

// GObject vfunc implementations

static void ladybird_web_view_finalize(GObject* object)
{
    auto* self = LADYBIRD_WEB_VIEW(object);
    // Don't delete impl - it's owned by Tab's OwnPtr<WebContentView>.
    // Just tell the C++ side the widget is gone.
    if (self->impl)
        self->impl->set_widget(nullptr);
    self->impl = nullptr;
    G_OBJECT_CLASS(ladybird_web_view_parent_class)->finalize(object);
}

static void ladybird_web_view_snapshot(GtkWidget* widget, GtkSnapshot* snapshot)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (self->impl)
        self->impl->paint(snapshot);
}

static void ladybird_web_view_measure(GtkWidget*, GtkOrientation orientation, int, int* minimum, int* natural, int* minimum_baseline, int* natural_baseline)
{
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = 100;
        *natural = 800;
    } else {
        *minimum = 100;
        *natural = 600;
    }
    *minimum_baseline = -1;
    *natural_baseline = -1;
}

static void ladybird_web_view_size_allocate(GtkWidget* widget, int width, int height, int)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (self->impl)
        self->impl->update_viewport_size(width, height);
}

static void ladybird_web_view_class_init(LadybirdWebViewClass* klass)
{
    auto* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = ladybird_web_view_snapshot;
    widget_class->measure = ladybird_web_view_measure;
    widget_class->size_allocate = ladybird_web_view_size_allocate;

    auto* object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = ladybird_web_view_finalize;

    gtk_widget_class_set_css_name(widget_class, "ladybird-web-view");
}

// Input event callbacks

static gboolean on_key_pressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return GDK_EVENT_PROPAGATE;

    self->impl->enqueue_native_event(Web::KeyEvent::Type::KeyDown, keyval, state);
    return GDK_EVENT_STOP;
}

static void on_key_released(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    self->impl->enqueue_native_event(Web::KeyEvent::Type::KeyUp, keyval, state);
}

static void on_mouse_pressed(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    gtk_widget_grab_focus(GTK_WIDGET(self));

    auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseDown, x, y, button, state, n_press);
}

static void on_mouse_released(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseUp, x, y, button, state, n_press);
}

static void on_mouse_motion(GtkEventControllerMotion* controller, gdouble x, gdouble y, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseMove, x, y, 0, state, 0);
}

static void on_mouse_leave(GtkEventControllerMotion*, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return;

    self->impl->enqueue_native_event(Web::MouseEvent::Type::MouseLeave, 0, 0, 0, static_cast<GdkModifierType>(0), 0);
}

static gboolean on_scroll(GtkEventControllerScroll* controller, gdouble dx, gdouble dy, gpointer user_data)
{
    auto* self = LADYBIRD_WEB_VIEW(user_data);
    if (!self->impl)
        return GDK_EVENT_PROPAGATE;

    auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));

    // Ctrl+scroll = zoom
    if (state & GDK_CONTROL_MASK) {
        if (dy < 0)
            self->impl->zoom_in();
        else if (dy > 0)
            self->impl->zoom_out();
        return GDK_EVENT_STOP;
    }

    auto device_pixel_ratio = self->impl->device_pixel_ratio();

    int wheel_delta_x = 0;
    int wheel_delta_y = 0;

    auto* gdk_event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    auto unit = gdk_scroll_event_get_unit(gdk_event);

    if (unit == GDK_SCROLL_UNIT_SURFACE) {
        wheel_delta_x = static_cast<int>(dx * device_pixel_ratio);
        wheel_delta_y = static_cast<int>(dy * device_pixel_ratio);
    } else {
        static constexpr double scroll_lines = 3.0;
        static constexpr double scroll_step_size = 24.0;
        wheel_delta_x = static_cast<int>(dx * scroll_lines * scroll_step_size * device_pixel_ratio);
        wheel_delta_y = static_cast<int>(dy * scroll_lines * scroll_step_size * device_pixel_ratio);
    }

    Web::MouseEvent event {
        .type = Web::MouseEvent::Type::MouseWheel,
        .position = {},
        .screen_position = {},
        .button = Web::UIEvents::MouseButton::None,
        .buttons = Web::UIEvents::MouseButton::None,
        .modifiers = Ladybird::gdk_modifier_to_web(state),
        .wheel_delta_x = wheel_delta_x,
        .wheel_delta_y = wheel_delta_y,
        .click_count = 0,
        .browser_data = {},
    };
    self->impl->enqueue_input_event(move(event));
    return GDK_EVENT_STOP;
}

static void on_map(GtkWidget* widget, gpointer)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (!self->impl)
        return;
    self->impl->set_system_visibility_state(Web::HTML::VisibilityState::Visible);
    self->impl->update_viewport_size();
    gtk_widget_queue_draw(widget);
}

static void on_unmap(GtkWidget* widget, gpointer)
{
    auto* self = LADYBIRD_WEB_VIEW(widget);
    if (!self->impl)
        return;
    self->impl->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
}

static void ladybird_web_view_init(LadybirdWebView* self)
{
    self->impl = nullptr;

    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);

    g_signal_connect(GTK_WIDGET(self), "map", G_CALLBACK(on_map), nullptr);
    g_signal_connect(GTK_WIDGET(self), "unmap", G_CALLBACK(on_unmap), nullptr);

    auto* focus_controller = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus_controller, "enter", G_CALLBACK(+[](LadybirdWebView* self, GtkEventControllerFocus*) {
        if (self->impl)
            self->impl->set_has_focus(true);
    }),
        self);
    g_signal_connect_swapped(focus_controller, "leave", G_CALLBACK(+[](LadybirdWebView* self, GtkEventControllerFocus*) {
        if (self->impl)
            self->impl->set_has_focus(false);
    }),
        self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(focus_controller));

    auto* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(key_controller, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_controller);

    auto* click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), 0);
    g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_mouse_pressed), self);
    g_signal_connect(click_gesture, "released", G_CALLBACK(on_mouse_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click_gesture));

    auto* motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(motion_controller, "motion", G_CALLBACK(on_mouse_motion), self);
    g_signal_connect(motion_controller, "leave", G_CALLBACK(on_mouse_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion_controller);

    auto* scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll_controller);
}

// Public API

LadybirdWebView* ladybird_web_view_new()
{
    return LADYBIRD_WEB_VIEW(g_object_new(LADYBIRD_TYPE_WEB_VIEW, nullptr));
}

Ladybird::WebContentView* ladybird_web_view_get_impl(LadybirdWebView* self)
{
    return self->impl;
}

void ladybird_web_view_set_impl(LadybirdWebView* self, Ladybird::WebContentView* impl)
{
    self->impl = impl;
}
