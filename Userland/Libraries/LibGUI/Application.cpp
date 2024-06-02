/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibConfig/Client.h>
#include <LibCore/EventLoop.h>
#include <LibGUI/Action.h>
#include <LibGUI/Application.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/ConnectionToWindowServer.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menubar.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Palette.h>

namespace GUI {

class Application::TooltipWindow final : public Window {
    C_OBJECT(TooltipWindow);

public:
    void set_tooltip(String tooltip)
    {
        m_label->set_text(move(tooltip));
        int tooltip_width = m_label->effective_min_size().width().as_int() + 10;
        int line_count = m_label->text().bytes_as_string_view().count_lines();
        int font_size = m_label->font().pixel_size_rounded_up();
        int tooltip_height = font_size * line_count + ((font_size + 1) / 2) * (line_count - 1) + 8;

        Gfx::IntRect desktop_rect = Desktop::the().rect();
        if (tooltip_width > desktop_rect.width())
            tooltip_width = desktop_rect.width();

        set_rect(rect().x(), rect().y(), tooltip_width, tooltip_height);
    }

private:
    TooltipWindow()
    {
        set_window_type(WindowType::Tooltip);
        set_obey_widget_min_size(false);
        m_label = set_main_widget<Label>();
        m_label->set_background_role(Gfx::ColorRole::Tooltip);
        m_label->set_foreground_role(Gfx::ColorRole::TooltipText);
        m_label->set_fill_with_background_color(true);
        m_label->set_frame_style(Gfx::FrameStyle::Plain);
        m_label->set_autosize(true);
    }

    RefPtr<Label> m_label;
};

static NeverDestroyed<WeakPtr<Application>> s_the;

Application* Application::the()
{
    // NOTE: If we don't explicitly call revoke_weak_ptrs() in the
    // ~Application destructor, we would have to change this to
    // return s_the->strong_ref().ptr();
    // This is because this is using the unsafe operator*/operator->
    // that do not have the ability to check the ref count!
    return *s_the;
}

ErrorOr<NonnullRefPtr<Application>> Application::create(Main::Arguments const& arguments)
{
    if (*s_the)
        return Error::from_string_literal("An Application has already been created for this process!");

    auto application = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) Application {}));
    *s_the = *application;

    application->m_event_loop = TRY(try_make<Core::EventLoop>());

    ConnectionToWindowServer::the();
    TRY(Clipboard::initialize({}));

    if (arguments.argc > 0)
        application->m_invoked_as = arguments.argv[0];

    if (getenv("GUI_FOCUS_DEBUG"))
        application->m_focus_debugging_enabled = true;

    if (getenv("GUI_HOVER_DEBUG"))
        application->m_hover_debugging_enabled = true;

    if (getenv("GUI_DND_DEBUG"))
        application->m_dnd_debugging_enabled = true;

    if (!arguments.strings.is_empty()) {
        for (auto arg : arguments.strings.slice(1))
            TRY(application->m_args.try_append(arg));
    }

    application->m_tooltip_show_timer = Core::Timer::create_single_shot(700, [weak_application = application->make_weak_ptr<Application>()] {
        weak_application->request_tooltip_show();
    });

    application->m_tooltip_hide_timer = Core::Timer::create_single_shot(50, [weak_application = application->make_weak_ptr<Application>()] {
        weak_application->tooltip_hide_timer_did_fire();
    });

    return application;
}

static bool s_in_teardown;

bool Application::in_teardown()
{
    return s_in_teardown;
}

Application::~Application()
{
    s_in_teardown = true;
    revoke_weak_ptrs();
}

int Application::exec()
{
    return m_event_loop->exec();
}

void Application::quit(int exit_code)
{
    m_event_loop->quit(exit_code);
}

void Application::register_global_shortcut_action(Badge<Action>, Action& action)
{
    m_global_shortcut_actions.set(action.shortcut(), &action);
    m_global_shortcut_actions.set(action.alternate_shortcut(), &action);
}

void Application::unregister_global_shortcut_action(Badge<Action>, Action& action)
{
    m_global_shortcut_actions.remove(action.shortcut());
    m_global_shortcut_actions.remove(action.alternate_shortcut());
}

Action* Application::action_for_shortcut(Shortcut const& shortcut) const
{
    auto it = m_global_shortcut_actions.find(shortcut);
    if (it == m_global_shortcut_actions.end())
        return nullptr;
    return (*it).value;
}

void Application::show_tooltip(String tooltip, Widget const* tooltip_source_widget)
{
    if (!Desktop::the().system_effects().tooltips())
        return;
    m_tooltip_source_widget = tooltip_source_widget;
    if (!m_tooltip_window) {
        m_tooltip_window = TooltipWindow::construct();
        m_tooltip_window->set_double_buffering_enabled(false);
    }
    m_tooltip_window->set_tooltip(move(tooltip));

    if (m_tooltip_window->is_visible()) {
        request_tooltip_show();
        m_tooltip_show_timer->stop();
        m_tooltip_hide_timer->stop();
    } else {
        m_tooltip_show_timer->restart();
        m_tooltip_hide_timer->stop();
    }
}

void Application::show_tooltip_immediately(String tooltip, Widget const* tooltip_source_widget)
{
    if (!Desktop::the().system_effects().tooltips())
        return;
    m_tooltip_source_widget = tooltip_source_widget;
    if (!m_tooltip_window) {
        m_tooltip_window = TooltipWindow::construct();
        m_tooltip_window->set_double_buffering_enabled(false);
    }
    m_tooltip_window->set_tooltip(move(tooltip));

    request_tooltip_show();
    m_tooltip_show_timer->stop();
    m_tooltip_hide_timer->stop();
}

void Application::hide_tooltip()
{
    m_tooltip_show_timer->stop();
    m_tooltip_hide_timer->start();
}

void Application::did_create_window(Badge<Window>)
{
    if (m_event_loop->was_exit_requested())
        m_event_loop->unquit();
}

void Application::did_delete_last_window(Badge<Window>)
{
    if (m_quit_when_last_window_deleted)
        m_event_loop->quit(0);
}

void Application::set_system_palette(Core::AnonymousBuffer& buffer)
{
    if (!m_system_palette)
        m_system_palette = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
    else
        m_system_palette->replace_internal_buffer(buffer);

    if (!m_palette)
        m_palette = m_system_palette;
}

void Application::set_palette(Palette& palette)
{
    m_palette = palette.impl();
}

Gfx::Palette Application::palette() const
{
    return Palette(*m_palette);
}

void Application::request_tooltip_show()
{
    VERIFY(m_tooltip_window);
    Gfx::IntRect desktop_rect = Desktop::the().rect();

    int const margin = 30;
    Gfx::IntPoint adjusted_pos = ConnectionToWindowServer::the().get_global_cursor_position();

    adjusted_pos.translate_by(0, 14);

    if (adjusted_pos.x() + m_tooltip_window->width() >= desktop_rect.width() - margin) {
        adjusted_pos = adjusted_pos.translated(-m_tooltip_window->width(), 0);
    }
    if (adjusted_pos.y() + m_tooltip_window->height() >= desktop_rect.height() - margin) {
        adjusted_pos = adjusted_pos.translated(0, -(m_tooltip_window->height() * 2));
    }
    if (adjusted_pos.x() < 0)
        adjusted_pos.set_x(0);

    m_tooltip_window->move_to(adjusted_pos);
    m_tooltip_window->show();
}

void Application::tooltip_hide_timer_did_fire()
{
    m_tooltip_source_widget = nullptr;
    if (m_tooltip_window)
        m_tooltip_window->hide();
}

void Application::window_did_become_active(Badge<Window>, Window& window)
{
    m_active_window = window.make_weak_ptr<Window>();
    window.update();
}

void Application::window_did_become_inactive(Badge<Window>, Window& window)
{
    if (m_active_window.ptr() != &window)
        return;
    window.update();
    m_active_window = nullptr;
}

void Application::set_pending_drop_widget(Widget* widget)
{
    if (m_pending_drop_widget == widget)
        return;
    if (m_pending_drop_widget)
        m_pending_drop_widget->update();
    m_pending_drop_widget = widget;
    if (m_pending_drop_widget)
        m_pending_drop_widget->update();
}

void Application::set_drag_hovered_widget_impl(Widget* widget, Gfx::IntPoint position, Vector<String> mime_types)
{
    if (widget == m_drag_hovered_widget)
        return;

    if (m_drag_hovered_widget) {
        Event leave_event(Event::DragLeave);
        m_drag_hovered_widget->dispatch_event(leave_event, m_drag_hovered_widget->window());
    }

    set_pending_drop_widget(nullptr);
    m_drag_hovered_widget = widget;

    if (m_drag_hovered_widget) {
        DragEvent enter_event(Event::DragEnter, position, move(mime_types));
        enter_event.ignore();
        m_drag_hovered_widget->dispatch_event(enter_event, m_drag_hovered_widget->window());
        if (enter_event.is_accepted())
            set_pending_drop_widget(m_drag_hovered_widget);
        ConnectionToWindowServer::the().async_set_accepts_drag(enter_event.is_accepted());
    }
}

void Application::notify_drag_cancelled(Badge<ConnectionToWindowServer>)
{
    set_drag_hovered_widget_impl(nullptr);
}

void Application::event(Core::Event& event)
{
    if (event.type() == GUI::Event::ActionEnter || event.type() == GUI::Event::ActionLeave) {
        auto& action_event = static_cast<ActionEvent&>(event);
        auto& action = action_event.action();
        if (action_event.type() == GUI::Event::ActionEnter) {
            if (on_action_enter)
                on_action_enter(action);
        } else {
            if (on_action_leave)
                on_action_leave(action);
        }
    }
    if (event.type() == GUI::Event::ThemeChange) {
        if (on_theme_change)
            on_theme_change();
    }
    EventReceiver::event(event);
}

void Application::set_config_domain(String config_domain)
{
    m_config_domain = move(config_domain);
}

void Application::register_recent_file_actions(Badge<GUI::Menu>, Vector<NonnullRefPtr<GUI::Action>> actions)
{
    m_recent_file_actions = move(actions);
    update_recent_file_actions();
}

void Application::update_recent_file_actions()
{
    VERIFY(!m_config_domain.is_empty());

    size_t number_of_recently_open_files = 0;
    auto update_action = [&](size_t index) {
        auto& action = m_recent_file_actions[index];
        char buffer = static_cast<char>('0' + index);
        auto key = StringView(&buffer, 1);
        auto path = Config::read_string(m_config_domain, "RecentFiles"sv, key);

        if (path.is_empty()) {
            action->set_visible(false);
            action->set_enabled(false);
        } else {
            action->set_visible(true);
            action->set_enabled(true);
            action->set_text(path);
            action->set_status_tip(String::formatted("Open {}", path).release_value_but_fixme_should_propagate_errors());
            ++number_of_recently_open_files;
        }
    };
    for (size_t i = 0; i < max_recently_open_files(); ++i)
        update_action(i);

    // Hide or show the "(No recently open files)" placeholder.
    m_recent_file_actions.last()->set_visible(number_of_recently_open_files == 0);
}

void Application::set_most_recently_open_file(ByteString new_path)
{
    VERIFY(!new_path.is_empty());
    Vector<ByteString> new_recent_files_list;

    for (size_t i = 0; i < max_recently_open_files(); ++i) {
        static_assert(max_recently_open_files() < 10);
        char buffer = static_cast<char>('0' + i);
        auto key = StringView(&buffer, 1);
        new_recent_files_list.append(Config::read_string(m_config_domain, "RecentFiles"sv, key));
    }

    new_recent_files_list.remove_all_matching([&](auto& existing_path) {
        return existing_path.view() == new_path;
    });

    new_recent_files_list.prepend(new_path);
    new_recent_files_list.resize(max_recently_open_files());

    for (size_t i = 0; i < max_recently_open_files(); ++i) {
        auto& path = new_recent_files_list[i];
        char buffer = static_cast<char>('0' + i);
        auto key = StringView(&buffer, 1);
        Config::write_string(
            m_config_domain,
            "RecentFiles"sv,
            key,
            path);
    }

    if (!m_recent_file_actions.is_empty())
        update_recent_file_actions();
}

}
