/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibURL/InternalURLs.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/EventLoopImplementationQt.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WebContentView.h>

#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif

#include <QAction>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileOpenEvent>
#include <QFormLayout>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStandardPaths>
#include <QTimer>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <LibCore/TimeZoneWatcher.h>
#    include <QAbstractNativeEventFilter>
#endif

namespace Ladybird {

#if defined(AK_OS_WINDOWS)
class NativeWindowsTimeChangeEventFilter : public QAbstractNativeEventFilter {
public:
    NativeWindowsTimeChangeEventFilter(Core::TimeZoneWatcher& time_zone_watcher)
        : m_time_zone_watcher(time_zone_watcher)
    {
    }

    bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr*) override
    {
        if (event_type == QByteArrayLiteral("windows_generic_MSG")) {
            auto msg = static_cast<MSG*>(message);
            if (msg->message == WM_TIMECHANGE) {
                m_time_zone_watcher.on_time_zone_changed();
            }
        }
        return false;
    }

private:
    Core::TimeZoneWatcher& m_time_zone_watcher;
};

#endif

class LadybirdQApplication : public QApplication {
public:
    explicit LadybirdQApplication(Main::Arguments& arguments)
        : QApplication(arguments.argc, arguments.argv)
    {
#if defined(AK_OS_MACOS)
        setQuitOnLastWindowClosed(false);
        install_appkit_event_capture();
#endif
        update_chrome_style();
    }

    virtual bool event(QEvent* event) override
    {
        auto& application = static_cast<Application&>(WebView::Application::the());
        auto const event_type = event->type();

#if defined(AK_OS_WINDOWS)
        static Optional<NativeWindowsTimeChangeEventFilter> time_change_event_filter {};
        if (auto time_zone_watcher = application.time_zone_watcher(); !time_change_event_filter.has_value() && time_zone_watcher.has_value()) {
            time_change_event_filter.emplace(time_zone_watcher.value());
            installNativeEventFilter(&time_change_event_filter.value());
        }
#endif

        switch (event_type) {
        case QEvent::FileOpen: {
            if (!application.on_open_file)
                break;

            auto const& open_event = *static_cast<QFileOpenEvent const*>(event);
            auto file = ak_string_from_qstring(open_event.file());

            if (auto file_url = WebView::sanitize_url(file); file_url.has_value())
                application.on_open_file(file_url.release_value());
            break;
        }

        default:
            break;
        }

        auto handled = QApplication::event(event);
        if (event_type == QEvent::ApplicationPaletteChange || event_type == QEvent::ThemeChange)
            update_chrome_style();

        return handled;
    }

#if defined(AK_OS_MACOS)
    void update_reopen_recently_closed_action()
    {
        if (!m_reopen_recently_closed_tab_action)
            return;

        auto recently_closed_entry = Application::history_store().most_recently_closed_entry();
        m_reopen_recently_closed_tab_action->setText("&Reopen Recently Closed Tab");
        m_reopen_recently_closed_tab_action->setEnabled(recently_closed_entry.has_value());
    }

#endif

#if defined(AK_OS_MACOS)
    void create_application_menu_bar()
    {
        if (m_application_menu_bar)
            return;

        m_application_menu_bar = new QMenuBar;
        auto& application = Application::the();

        auto* file_menu = m_application_menu_bar->addMenu("&File");

        auto* new_tab_action = add_application_menu_action(*file_menu, "New &Tab", QKeySequence::keyBindings(QKeySequence::StandardKey::AddTab));
        QObject::connect(new_tab_action, &QAction::triggered, this, [] {
            Application::the().open_new_tab();
        });

        auto* new_window_action = add_application_menu_action(*file_menu, "New &Window", QKeySequence::keyBindings(QKeySequence::StandardKey::New));
        QObject::connect(new_window_action, &QAction::triggered, this, [] {
            Application::the().open_new_window();
        });

        m_reopen_recently_closed_tab_action = add_application_menu_action(*file_menu, {}, { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T) });
        QObject::connect(m_reopen_recently_closed_tab_action, &QAction::triggered, this, [] {
            Application::the().reopen_recently_closed_tab();
        });

        auto* open_file_action = add_application_menu_action(*file_menu, "&Open File...", QKeySequence::keyBindings(QKeySequence::StandardKey::Open));
        QObject::connect(open_file_action, &QAction::triggered, this, [] {
            Application::the().open_file();
        });

        file_menu->addAction(create_application_action(*file_menu, application.open_downloads_page_action(), IncludeActionIcon::No));

        auto* open_location_action = add_application_menu_action(*file_menu, "Open &Location", { QKeySequence("Ctrl+L"), QKeySequence("Alt+D") });
        QObject::connect(open_location_action, &QAction::triggered, this, [] {
            Application::the().focus_location_editor();
        });

        file_menu->addSeparator();

        auto* quit_action = add_application_menu_action(*file_menu, "&Quit", QKeySequence::keyBindings(QKeySequence::StandardKey::Quit));
        QObject::connect(quit_action, &QAction::triggered, this, [] {
            Application::the().quit();
        });

        auto* edit_menu = m_application_menu_bar->addMenu("&Edit");
        edit_menu->addAction(create_application_action(*edit_menu, application.cut_selection_action(), IncludeActionIcon::No));
        edit_menu->addAction(create_application_action(*edit_menu, application.copy_selection_action(), IncludeActionIcon::No));
        edit_menu->addAction(create_application_action(*edit_menu, application.paste_action(), IncludeActionIcon::No));
        edit_menu->addAction(create_application_action(*edit_menu, application.select_all_action(), IncludeActionIcon::No));
        edit_menu->addSeparator();
        edit_menu->addAction(create_application_action(*edit_menu, application.open_settings_page_action(), IncludeActionIcon::No));

        auto* view_menu = m_application_menu_bar->addMenu("&View");
        view_menu->addMenu(create_application_menu(*view_menu, application.color_scheme_menu()));
        view_menu->addMenu(create_application_menu(*view_menu, application.contrast_menu()));
        view_menu->addMenu(create_application_menu(*view_menu, application.motion_menu()));

        m_application_menu_bar->addMenu(bookmarks_menu());
        m_application_menu_bar->addMenu(create_application_menu(*m_application_menu_bar, application.history_menu()));

        auto* help_menu = m_application_menu_bar->addMenu("&Help");
        help_menu->addAction(create_application_action(*help_menu, application.open_about_page_action(), IncludeActionIcon::No));

        update_reopen_recently_closed_action();
    }
#endif

#if defined(AK_OS_MACOS)
    QMenu* bookmarks_menu()
    {
        if (!m_bookmarks_menu)
            m_bookmarks_menu = create_application_menu(*m_application_menu_bar, Application::the().bookmarks_menu());
        return m_bookmarks_menu;
    }

    void rebuild_bookmarks_menu()
    {
        if (m_bookmarks_menu)
            repopulate_application_menu(*m_bookmarks_menu, *m_application_menu_bar, Application::the().bookmarks_menu());
    }
#endif

private:
#if defined(AK_OS_MACOS)
    QAction* add_application_menu_action(QMenu& menu, QString const& text, QList<QKeySequence> shortcuts)
    {
        auto* action = new QAction(text, m_application_menu_bar);
        action->setShortcuts(shortcuts);
        menu.addAction(action);
        return action;
    }
#endif

    void update_chrome_style()
    {
        setStyleSheet(ChromeStyle::application_style_sheet(palette()));
    }

#if defined(AK_OS_MACOS)
    QMenuBar* m_application_menu_bar { nullptr };
    QMenu* m_bookmarks_menu { nullptr };
    QAction* m_reopen_recently_closed_tab_action { nullptr };
#endif
};

Application::Application() = default;
Application::~Application() = default;

void Application::create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options)
{
    web_content_options.config_path = Settings::the()->directory();
}

Core::EventLoop& Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        Core::EventLoopManager::install(*new EventLoopManagerQt);
        m_application = make<LadybirdQApplication>(arguments());
    }

    auto& event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value())
        static_cast<EventLoopImplementationQt&>(event_loop.impl()).set_main_loop();

    return event_loop;
}

BrowserWindow& Application::new_window(Vector<URL::URL> const& initial_urls, WindowConfiguration const& configuration, BrowserWindow::IsPopupWindow is_popup_window, Tab* parent_tab, Optional<u64> page_index)
{
    auto* window = new BrowserWindow(initial_urls, is_popup_window, parent_tab, move(page_index));
    set_active_window(*window);
    QObject::connect(window, &QObject::destroyed, m_application.ptr(), [this, window] {
        if (m_active_window == window)
            m_active_window = nullptr;
    });

    auto should_focus_location_editor = initial_urls.size() == 1 && initial_urls.first() == WebView::Application::settings().new_tab_page_url();
    if (should_focus_location_editor) {
        if (auto* tab = window->current_tab())
            tab->set_url_is_hidden(true);
    }

    window->set_window_rect(configuration.x, configuration.y, configuration.width, configuration.height);
    if (configuration.maximized == true)
        window->showMaximized();
    else
        window->show();

    window->activateWindow();
    window->raise();
    if (should_focus_location_editor) {
        QTimer::singleShot(0, window, [window] {
            if (auto* tab = window->current_tab())
                tab->focus_location_editor();
        });
    }
    return *window;
}

void Application::open_new_tab()
{
    if (!m_active_window) {
        new_window({ WebView::Application::settings().new_tab_page_url() });
        return;
    }

    auto& tab = m_active_window->new_tab_from_url(WebView::Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes);
    tab.set_url_is_hidden(true);
    tab.focus_location_editor();
}

void Application::open_new_window()
{
    WindowConfiguration configuration {};
    if (auto* previous_active_window = active_window_if_any()) {
        configuration.width = previous_active_window->width();
        configuration.height = previous_active_window->height();
        configuration.maximized = previous_active_window->isMaximized();
    }
    new_window({ WebView::Application::settings().new_tab_page_url() }, configuration);
}

void Application::focus_location_editor()
{
    if (!m_active_window) {
        new_window({ WebView::Application::settings().new_tab_page_url() });
        return;
    }

    if (auto* tab = m_active_window->current_tab())
        tab->focus_location_editor();
}

void Application::reopen_recently_closed_tab()
{
    auto recently_closed_entry = Application::history_store().pop_most_recently_closed_entry();
    if (recently_closed_entry.has_value()) {
        if (recently_closed_entry->was_window) {
            auto& window = new_window(recently_closed_entry->urls);
            window.activate_tab(static_cast<int>(recently_closed_entry->active_tab_index));
        } else if (!recently_closed_entry->urls.is_empty()) {
            if (!m_active_window)
                new_window({ recently_closed_entry->urls[0] });
            else
                m_active_window->new_tab_from_url(recently_closed_entry->urls[0], Web::HTML::ActivateTab::Yes);
        }
    }
    update_reopen_recently_closed_actions();
}

void Application::open_file()
{
    if (!m_active_window) {
        auto filename = QFileDialog::getOpenFileUrl(nullptr, "Open file", QDir::homePath(), "All Files (*.*)");
        if (filename.isValid())
            new_window({ ak_url_from_qurl(filename) });
        return;
    }

    m_active_window->open_file();
}

void Application::quit()
{
    if (!confirm_cancel_active_downloads(active_window_if_any()))
        return;

    QApplication::closeAllWindows();

    for (auto* widget : QApplication::topLevelWidgets()) {
        if (as_if<BrowserWindow>(widget) && widget->isVisible())
            return;
    }

    QApplication::quit();
}

bool Application::confirm_cancel_active_downloads(QWidget* parent)
{
    auto& downloader = file_downloader();
    if (!downloader.has_active_downloads())
        return true;

    QMessageBox dialog(parent ? parent : active_window_if_any());
    dialog.setWindowTitle("Ladybird");
    dialog.setIcon(QMessageBox::Warning);
    dialog.setText("Downloads are still in progress.");
    dialog.setInformativeText("Quitting will cancel active downloads.");
    auto* quit_button = dialog.addButton("Quit and Cancel Downloads", QMessageBox::DestructiveRole);
    dialog.addButton(QMessageBox::Cancel);
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.exec();

    if (dialog.clickedButton() != quit_button)
        return false;

    downloader.cancel_active_downloads();
    return true;
}

void Application::initialize_macos_application_menu()
{
#if defined(AK_OS_MACOS)
    if (m_application)
        static_cast<LadybirdQApplication*>(m_application.ptr())->create_application_menu_bar();
#endif
}

QMenu* Application::qt_bookmarks_menu() const
{
#if defined(AK_OS_MACOS)
    if (m_application)
        return static_cast<LadybirdQApplication*>(m_application.ptr())->bookmarks_menu();
#endif
    return nullptr;
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (auto* active_tab = this->active_tab())
        return active_tab->view();
    return {};
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    if (!m_active_window) {
        auto& window = const_cast<Application&>(*this).new_window({ WebView::Application::settings().new_tab_page_url() });
        if (auto* tab = window.current_tab())
            return tab->view();
        return {};
    }

    auto& tab = active_window().create_new_tab(activate_tab);
    return tab.view();
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const
{
    if (!m_active_window) {
        const_cast<Application&>(*this).new_window({ url });
        return;
    }

    active_window().new_tab_from_url(url, activate_tab);
}

bool Application::activate_tab_with_url(URL::URL const& url) const
{
    if (!m_active_window)
        return false;
    return m_active_window->activate_tab_with_url(url);
}

void Application::open_url_in_new_window(URL::URL const& url)
{
    this->new_window({ url });
}

Optional<ByteString> Application::ask_user_for_download_path(ByteString const& file) const
{
    auto default_path = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

    if (default_path.isNull() || default_path.isEmpty())
        default_path = qstring_from_ak_string(file.view());
    else
        default_path = QDir { default_path }.filePath(qstring_from_ak_string(file.view()));

    auto path = QFileDialog::getSaveFileName(nullptr, "Select save location", default_path);
    if (path.isNull())
        return {};

    return ak_byte_string_from_qstring(path);
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    auto message = MUST(String::formatted("{} saved to: {}", download_name, path));

    QMessageBox dialog(active_tab());
    dialog.setWindowTitle("Ladybird");
    dialog.setIcon(QMessageBox::Information);
    dialog.setText(qstring_from_ak_string(message));
    dialog.addButton(QMessageBox::Ok);
    dialog.addButton(QMessageBox::Open)->setText("Open folder");

    if (dialog.exec() == QMessageBox::Open) {
        auto path_url = QUrl::fromLocalFile(qstring_from_ak_string(path.dirname()));
        QDesktopServices::openUrl(path_url);
    }
}

void Application::display_error_dialog(StringView error_message) const
{
    QMessageBox::warning(active_tab(), "Ladybird", qstring_from_ak_string(error_message));
}

void Application::open_download(WebView::FileDownloader::Download const& download) const
{
    auto path = download_file_path_for_frontend_action(download);
    if (path.is_error()) {
        display_error_dialog("Unable to open downloaded file: path cannot be represented by this frontend"sv);
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(qstring_from_ak_string(path.release_value())));
}

void Application::show_download_in_folder(WebView::FileDownloader::Download const& download) const
{
    auto path = download_directory_path_for_frontend_action(download);
    if (path.is_error()) {
        display_error_dialog("Unable to show downloaded file: path cannot be represented by this frontend"sv);
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(qstring_from_ak_string(path.release_value())));
}

static QClipboard::Mode clipboard_mode(QClipboard const& clipboard, Application::ClipboardType type)
{
    switch (type) {
    case WebView::Application::ClipboardType::Text:
        return QClipboard::Clipboard;
    case WebView::Application::ClipboardType::Selection:
        return clipboard.supportsSelection() ? QClipboard::Selection : QClipboard::Clipboard;
    }
    VERIFY_NOT_REACHED();
}

bool Application::supports_clipboard_type(ClipboardType type) const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::supports_clipboard_type(type);

    switch (type) {
    case WebView::Application::ClipboardType::Text:
        return true;
    case WebView::Application::ClipboardType::Selection:
        return QGuiApplication::clipboard()->supportsSelection();
    }
    VERIFY_NOT_REACHED();
}

Utf16String Application::clipboard_text(ClipboardType type) const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_text(type);

    auto const* clipboard = QGuiApplication::clipboard();
    auto mode = clipboard_mode(*clipboard, type);

    return utf16_string_from_qstring(clipboard->text(mode));
}

void Application::set_clipboard_text(String text, ClipboardType type)
{
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::set_clipboard_text(text, type);
        return;
    }

    auto* clipboard = QGuiApplication::clipboard();
    auto mode = clipboard_mode(*clipboard, type);

    clipboard->setText(qstring_from_ak_string(text), mode);
}

Vector<Web::Clipboard::SystemClipboardRepresentation> Application::clipboard_entries() const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_entries();

    Vector<Web::Clipboard::SystemClipboardRepresentation> representations;
    auto const* clipboard = QGuiApplication::clipboard();

    auto const* mime_data = clipboard->mimeData();
    if (!mime_data)
        return {};

    for (auto const& format : mime_data->formats()) {
        auto data = ak_byte_string_from_qbytearray(mime_data->data(format));
        auto mime_type = ak_string_from_qstring(format);

        representations.empend(move(data), move(mime_type));
    }

    return representations;
}

void Application::insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation entry)
{
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::insert_clipboard_entry(move(entry));
        return;
    }

    auto* mime_data = new QMimeData();
    mime_data->setData(qstring_from_ak_string(entry.mime_type), qbytearray_from_ak_string(entry.data));

    auto* clipboard = QGuiApplication::clipboard();
    clipboard->setMimeData(mime_data);
}

void Application::update_tabs_display() const
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->update_tabs_display();
    }
}

void Application::rebuild_bookmarks_menu() const
{
#if defined(AK_OS_MACOS)
    if (m_application)
        static_cast<LadybirdQApplication*>(m_application.ptr())->rebuild_bookmarks_menu();
#endif

    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->rebuild_bookmarks_menu();
    }
}

void Application::update_reopen_recently_closed_actions() const
{
#if defined(AK_OS_MACOS)
    if (m_application)
        static_cast<LadybirdQApplication*>(m_application.ptr())->update_reopen_recently_closed_action();
#endif

    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = as_if<BrowserWindow>(widget))
            window->update_reopen_recently_closed_action();
    }
}

void Application::show_bookmark_context_menu(Gfx::IntPoint content_position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id)
{
    if (auto* active_tab = this->active_tab()) {
        auto position = active_tab->view().mapToGlobal(QPoint { content_position.x(), content_position.y() });
        active_tab->bookmarks_bar().show_context_menu(position, item, target_folder_id);
    }
}

Optional<Application::BookmarkID> Application::bookmark_item_id_for_context_menu() const
{
    if (auto* active_tab = this->active_tab()) {
        auto const& bookmarks_bar = active_tab->bookmarks_bar();

        return Application::BookmarkID {
            .id = bookmarks_bar.selected_bookmark_menu_item_id(),
            .target_folder_id = bookmarks_bar.selected_bookmark_menu_target_folder_id(),
        };
    }

    return {};
}

template<typename PromiseType>
static NonnullRefPtr<PromiseType> display_add_or_edit_bookmark_dialog(
    QWidget* parent,
    QString const& dialog_title,
    Optional<URL::URL const&> current_url,
    Optional<String const&> current_title)
{
    auto promise = PromiseType::construct();

    auto* dialog = new QDialog(parent);
    dialog->resize(400, dialog->height());
    dialog->setWindowTitle(dialog_title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto* url_edit = new QLineEdit(dialog);
    auto* title_edit = new QLineEdit(dialog);

    if (current_url.has_value())
        url_edit->setText(qstring_from_ak_string(current_url->serialize()));
    if (current_title.has_value())
        title_edit->setText(qstring_from_ak_string(*current_title));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    auto* layout = new QFormLayout(dialog);
    layout->addRow("URL:", url_edit);
    layout->addRow("Title:", title_edit);
    layout->addRow(buttons);

    QObject::connect(dialog, &QDialog::finished, [promise, url_edit = QPointer { url_edit }, title_edit = QPointer { title_edit }](auto result) {
        if (result != QDialog::Accepted || !url_edit || !title_edit) {
            promise->reject(Error::from_errno(ECANCELED));
            return;
        }

        auto url = WebView::sanitize_url(ak_string_from_qstring(url_edit->text()));
        if (!url.has_value()) {
            promise->reject(Error::from_errno(EINVAL));
            return;
        }

        Optional<String> title;
        if (auto title_text = ak_string_from_qstring(title_edit->text()); !title_text.is_empty())
            title = move(title_text);

        promise->resolve(WebView::BookmarkItem::Bookmark {
            .url = url.release_value(),
            .title = move(title),
            .favicon_base64_png = {},
        });
    });

    dialog->open();
    return promise;
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_add_bookmark_dialog() const
{
    Optional<URL::URL> current_url;
    Optional<String> current_title;

    if (auto view = active_web_view(); view.has_value()) {
        current_url = view->url();
        current_title = view->title().to_utf8();
    }

    return display_add_or_edit_bookmark_dialog<BookmarkPromise>(active_tab(), "Add Bookmark", current_url, current_title);
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_dialog(WebView::BookmarkItem::Bookmark const& current_bookmark) const
{
    return display_add_or_edit_bookmark_dialog<BookmarkPromise>(active_tab(), "Edit Bookmark", current_bookmark.url, current_bookmark.title);
}

template<typename PromiseType>
static NonnullRefPtr<PromiseType> display_add_or_edit_bookmark_folder_dialog(
    QWidget* parent,
    QString const& dialog_title,
    Optional<String const&> current_title)
{
    auto promise = PromiseType::construct();

    auto* dialog = new QDialog(parent);
    dialog->resize(400, dialog->height());
    dialog->setWindowTitle(dialog_title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto* title_edit = new QLineEdit(dialog);
    if (current_title.has_value())
        title_edit->setText(qstring_from_ak_string(*current_title));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    auto* layout = new QFormLayout(dialog);
    layout->addRow("Title:", title_edit);
    layout->addRow(buttons);

    QObject::connect(dialog, &QDialog::finished, [promise, title_edit = QPointer { title_edit }](auto result) {
        if (result != QDialog::Accepted || !title_edit) {
            promise->reject(Error::from_errno(ECANCELED));
            return;
        }

        Optional<String> title;
        if (auto title_text = ak_string_from_qstring(title_edit->text()); !title_text.is_empty())
            title = move(title_text);

        promise->resolve(WebView::BookmarkItem::Folder {
            .title = move(title),
            .children = {},
        });
    });

    dialog->open();
    return promise;
}

NonnullRefPtr<Application::BookmarkFolderPromise> Application::display_add_bookmark_folder_dialog() const
{
    return display_add_or_edit_bookmark_folder_dialog<BookmarkFolderPromise>(active_tab(), "Add Folder", {});
}

NonnullRefPtr<Application::BookmarkFolderPromise> Application::display_edit_bookmark_folder_dialog(WebView::BookmarkItem::Folder const& current_folder) const
{
    return display_add_or_edit_bookmark_folder_dialog<BookmarkFolderPromise>(active_tab(), "Edit Folder", current_folder.title);
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();

    if (m_active_window)
        m_active_window->on_devtools_enabled();
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();

    if (m_active_window)
        m_active_window->on_devtools_disabled();
}

void Application::on_recently_closed_entries_changed() const
{
    update_reopen_recently_closed_actions();
}

}
