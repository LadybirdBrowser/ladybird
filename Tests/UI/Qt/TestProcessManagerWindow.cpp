/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/ProcessManager.h>
#include <UI/Qt/ProcessManagerWindow.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>

#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <signal.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

TEST_CASE(palette_changes_do_not_reenter_style_updates)
{
    Core::EventLoop event_loop;
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
    WebView::ProcessManager manager;
    Ladybird::ProcessManagerWindow window(manager);
    window.ensurePolished();

    for (auto color : { Qt::black, Qt::white, Qt::black }) {
        auto palette = window.palette();
        palette.setColor(QPalette::Window, color);
        window.setPalette(palette);
        EXPECT(window.styleSheet().isEmpty());
        EXPECT_EQ(window.palette().color(QPalette::Window), QColor(color));
    }

    auto* processes = window.findChild<QTreeWidget*>();
    EXPECT(processes);
    EXPECT_EQ(processes->columnCount(), 4);
    EXPECT(processes->headerItem()->textAlignment(0) & Qt::AlignLeft);
    for (int column = 1; column < 4; ++column)
        EXPECT(processes->headerItem()->textAlignment(column) & Qt::AlignRight);
}

TEST_CASE(search_keeps_matching_child_processes_visible)
{
    Core::EventLoop event_loop;
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
    WebView::ProcessManager manager;
    Ladybird::ProcessManagerWindow window(manager);
    auto* processes = window.findChild<QTreeWidget*>();
    auto* search = window.findChild<QLineEdit*>();

    auto* browser = new QTreeWidgetItem(processes, { "Browser", {}, {}, "123" });
    auto* page = new QTreeWidgetItem(processes, { "WebContent - Example", {}, {}, "456" });
    auto* frame = new QTreeWidgetItem(page, { "WebContent - Child frame", {}, {}, "789" });

    search->setText("CHILD");
    EXPECT(browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(!frame->isHidden());
    EXPECT(page->isExpanded());

    search->setText("123");
    EXPECT(!browser->isHidden());
    EXPECT(page->isHidden());

    search->setText("  example  ");
    EXPECT(browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(frame->isHidden());

    search->setText("no matching process");
    EXPECT(browser->isHidden());
    EXPECT(page->isHidden());
    EXPECT(frame->isHidden());

    search->clear();
    EXPECT(!browser->isHidden());
    EXPECT(!page->isHidden());
    EXPECT(!frame->isHidden());
}

TEST_CASE(browser_is_not_terminable_and_can_be_copied)
{
    Core::EventLoop event_loop;
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
    WebView::ProcessManager manager;
    Ladybird::ProcessManagerWindow window(manager);
    window.show();
    auto* processes = window.findChild<QTreeWidget*>();
    auto* button = window.findChild<QPushButton*>();
    EXPECT(!button->isEnabled());
    EXPECT_EQ(processes->topLevelItemCount(), 1);
    auto* browser = processes->topLevelItem(0);
    EXPECT_EQ(browser->text(0), QString("Browser"));
    processes->setCurrentItem(browser);
    EXPECT(!button->isEnabled());
    bool inspected_menu = false;
    QTimer::singleShot(0, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        EXPECT(menu);
        if (!menu)
            return;
        EXPECT_EQ(menu->actions().size(), 1);
        EXPECT_EQ(menu->actions().first()->text(), QString("Copy PID"));
        inspected_menu = true;
        menu->close();
    });
    QContextMenuEvent menu_event(QContextMenuEvent::Keyboard, QPoint(-1, -1), QPoint(-1, -1));
    QApplication::sendEvent(processes->viewport(), &menu_event);
    EXPECT(inspected_menu);
    for (auto* action : processes->actions()) {
        if (action->text() == "Copy PID") {
            EXPECT(action->isEnabled());
            action->trigger();
            EXPECT_EQ(QApplication::clipboard()->text(), browser->text(3));
        }
    }
}

#if !defined(AK_OS_WINDOWS)
TEST_CASE(end_process_terminates_a_child_and_handles_disappearing_selection)
{
    Core::EventLoop event_loop;
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
    WebView::ProcessManager manager;
    auto pipe = MUST(Core::System::pipe2(O_CLOEXEC));
    auto child = MUST(Core::Process::spawn({
        .executable = "/bin/cat",
        .file_actions = { Core::FileAction::DupFd { pipe[0], STDIN_FILENO } },
    }));
    auto pid = child.pid();
    bool reaped = false;
    ScopeGuard cleanup = [&] {
        if (!reaped) {
            (void)Core::Process::terminate_process(pid, Core::Process::TerminationMode::Forceful);
            (void)Core::System::waitpid(pid);
        }
        (void)Core::System::close(pipe[0]);
        (void)Core::System::close(pipe[1]);
    };
    auto identity = manager.add_process(WebView::Process(WebView::ProcessType::WebContent, nullptr, move(child)));
    Ladybird::ProcessManagerWindow window(manager);
    window.show();
    auto* processes = window.findChild<QTreeWidget*>();
    auto* button = window.findChild<QPushButton*>();
    auto* timer = window.findChild<QTimer*>();
    auto find_row = [&](u64 child_id) -> QTreeWidgetItem* {
        for (int i = 0; i < processes->topLevelItemCount(); ++i) {
            if (processes->topLevelItem(i)->data(3, Qt::UserRole + 1).toULongLong() == child_id)
                return processes->topLevelItem(i);
        }
        return nullptr;
    };

    // A page process that shows no site is listed with the browser's own processes.
    EXPECT_EQ(processes->topLevelItemCount(), 2);
    auto* child_item = find_row(identity.child_id);
    EXPECT(child_item);
    EXPECT_EQ(child_item->data(3, Qt::UserRole).toLongLong(), pid);

    // A spare one is listed apart, under a heading of its own.
    manager.find_process(pid)->set_title("(spare)"_utf16);
    EXPECT(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(processes->topLevelItemCount(), 3);
    EXPECT_EQ(processes->topLevelItem(1)->text(0), QString("Spare"));
    EXPECT(find_row(identity.child_id));
    manager.find_process(pid)->set_title({});
    EXPECT(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(processes->topLevelItemCount(), 2);

    child_item = find_row(identity.child_id);
    processes->setCurrentItem(child_item);
    EXPECT(button->isEnabled());
    EXPECT_EQ(button->text(), QString("End Process"));
    bool inspected_menu = false;
    QTimer::singleShot(0, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        EXPECT(menu);
        if (!menu)
            return;
        EXPECT_EQ(menu->actions().size(), 2);
        EXPECT_EQ(menu->actions().first()->text(), QString("End Process"));
        inspected_menu = true;
        menu->close();
    });
    QContextMenuEvent menu_event(QContextMenuEvent::Keyboard, QPoint(-1, -1), QPoint(-1, -1));
    QApplication::sendEvent(processes->viewport(), &menu_event);
    EXPECT(inspected_menu);
    button->click();
    auto status = MUST(Core::System::waitpid(pid));
    reaped = true;
    EXPECT(WIFSIGNALED(status.status));
    EXPECT_EQ(WTERMSIG(status.status), SIGKILL);
    EXPECT(!button->isEnabled());

    processes->setCurrentItem(find_row(identity.child_id));
    EXPECT(button->isEnabled());
    (void)manager.remove_process(identity.child_id);
    button->click();
    EXPECT(!button->isEnabled());
    EXPECT(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT(!find_row(identity.child_id));
    EXPECT(!button->isEnabled());
    EXPECT(processes->selectedItems().isEmpty());
}

TEST_CASE(helper_processes_are_listed_with_the_site_they_work_for)
{
    Core::EventLoop event_loop;
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
    WebView::ProcessManager manager;
    auto pipe = MUST(Core::System::pipe2(O_CLOEXEC));
    Vector<pid_t> pids;
    ScopeGuard cleanup = [&] {
        for (auto pid : pids) {
            (void)Core::Process::terminate_process(pid, Core::Process::TerminationMode::Forceful);
            (void)Core::System::waitpid(pid);
        }
        (void)Core::System::close(pipe[0]);
        (void)Core::System::close(pipe[1]);
    };
    auto add_process = [&](WebView::ProcessType type, Optional<u64> owner_child_id, Optional<StringView> site) {
        auto child = MUST(Core::Process::spawn({
            .executable = "/bin/cat",
            .file_actions = { Core::FileAction::DupFd { pipe[0], STDIN_FILENO } },
        }));
        pids.append(child.pid());
        auto identity = manager.add_process(WebView::Process(type, nullptr, move(child)));
        auto process = manager.find_child(identity.child_id);
        if (owner_child_id.has_value())
            process->set_owner_child_id(*owner_child_id);
        if (site.has_value())
            process->set_site_url(URL::Parser::basic_parse(*site));
        return identity.child_id;
    };

    // A worker of an opaque origin has no site of its own, and its helpers are listed with the page that started it.
    auto page = add_process(WebView::ProcessType::WebContent, {}, "https://example.com/"sv);
    auto worker = add_process(WebView::ProcessType::WebWorker, page, {});
    add_process(WebView::ProcessType::ImageDecoder, worker, {});
    add_process(WebView::ProcessType::AudioServer, worker, {});
    add_process(WebView::ProcessType::MediaServer, page, {});
    // A shared worker outlives the page that started it, and is then listed with the site of its own origin.
    auto gone_page = add_process(WebView::ProcessType::WebContent, {}, "https://example.org/"sv);
    auto surviving_worker = add_process(WebView::ProcessType::WebWorker, gone_page, "https://example.org"sv);
    add_process(WebView::ProcessType::ImageDecoder, surviving_worker, {});
    (void)manager.remove_process(gone_page);

    // With top-level site isolation, a cross-site frame shares its parent's process. Its worker has its own site.
    auto frame_worker = add_process(WebView::ProcessType::WebWorker, page, "https://example.net"sv);
    add_process(WebView::ProcessType::ImageDecoder, frame_worker, {});

    Ladybird::ProcessManagerWindow window(manager);
    window.show();
    auto* processes = window.findChild<QTreeWidget*>();
    auto child_count_of_site = [&](QString const& name) -> Optional<int> {
        for (int i = 0; i < processes->topLevelItemCount(); ++i) {
            if (processes->topLevelItem(i)->text(0) == name)
                return processes->topLevelItem(i)->childCount();
        }
        return {};
    };

    // The browser, the sites heading, and the three sites.
    EXPECT_EQ(processes->topLevelItemCount(), 5);
    EXPECT_EQ(child_count_of_site("example.com"), 5);
    EXPECT_EQ(child_count_of_site("example.org"), 2);
    EXPECT_EQ(child_count_of_site("example.net"), 2);
}
#endif

#if defined(AK_OS_MACOS)
TEST_CASE(pressing_the_title_bar_moves_the_window)
{
    Core::EventLoop event_loop;
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_utf8(TEST_RESOURCE_ROOT ""sv))));
    WebView::ProcessManager manager;
    Ladybird::ProcessManagerWindow window(manager);
    size_t drag_count = 0;
    window.on_title_bar_drag = [&] {
        ++drag_count;
        return true;
    };
    window.show();
    auto* title_bar = window.findChild<QWidget*>("LadybirdTaskManagerTitleBar");
    auto* title = window.findChild<QWidget*>("LadybirdTaskManagerTitle");
    auto* search = window.findChild<QLineEdit*>();
    auto press = [](QWidget& widget, Qt::MouseButton button) {
        QPointF position(2, 2);
        QMouseEvent event(QEvent::MouseButtonPress, position, widget.mapToGlobal(position), button, button, Qt::NoModifier);
        QApplication::sendEvent(&widget, &event);
    };

    press(*title_bar, Qt::LeftButton);
    EXPECT_EQ(drag_count, 1u);
    press(*title, Qt::LeftButton);
    EXPECT_EQ(drag_count, 2u);
    press(*title_bar, Qt::RightButton);
    press(*search, Qt::LeftButton);
    EXPECT_EQ(drag_count, 2u);
}
#endif

TEST_CASE(site_labels_handle_urls_without_hosts)
{
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("https://google.com/search?q=test"sv).value()), QString("google.com"));
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("https://www.reddit.com/r/ladybird"sv).value()), QString("reddit.com"));
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("https://reddit.com"sv).value()), QString("reddit.com"));
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("http://localhost:8000/"sv).value()), QString("localhost"));
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("about:blank"sv).value()), QString {});
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("file:///tmp/report.html"sv).value()), QString("report.html"));
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("data:text/plain,hello"sv).value()), QString("data:"));
}
