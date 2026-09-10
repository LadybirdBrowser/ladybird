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
    WebView::ProcessManager manager;
    Ladybird::ProcessManagerWindow window(manager);
    auto* processes = window.findChild<QTreeWidget*>();
    auto* search = window.findChild<QLineEdit*>();

    auto* browser = new QTreeWidgetItem(processes, { "Browser", "123" });
    auto* page = new QTreeWidgetItem(processes, { "WebContent - Example", "456" });
    auto* frame = new QTreeWidgetItem(page, { "WebContent - Child frame", "789" });

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
            EXPECT_EQ(QApplication::clipboard()->text(), browser->text(1));
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
    manager.add_process(WebView::Process(WebView::ProcessType::WebContent, nullptr, move(child)));
    Ladybird::ProcessManagerWindow window(manager);
    window.show();
    auto* processes = window.findChild<QTreeWidget*>();
    auto* button = window.findChild<QPushButton*>();
    auto* timer = window.findChild<QTimer*>();
    EXPECT_EQ(processes->topLevelItemCount(), 1);
    auto* browser = processes->topLevelItem(0);
    EXPECT_EQ(browser->childCount(), 1);
    auto* child_item = browser->child(0);
    EXPECT_EQ(child_item->data(1, Qt::UserRole).toLongLong(), pid);
    auto blank_icon = child_item->icon(0).pixmap(16, 16).toImage();
    manager.find_process(pid)->set_title("(spare)"_utf16);
    EXPECT(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT(child_item->text(0).contains("(spare)"));
    EXPECT(child_item->icon(0).pixmap(16, 16).toImage() != blank_icon);
    manager.find_process(pid)->set_title({});
    EXPECT(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(child_item->icon(0).pixmap(16, 16).toImage(), blank_icon);
    processes->setCurrentItem(child_item);
    EXPECT(button->isEnabled());
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

    processes->setCurrentItem(browser->child(0));
    EXPECT(button->isEnabled());
    (void)manager.remove_process(pid);
    button->click();
    EXPECT(!button->isEnabled());
    EXPECT(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_EQ(browser->childCount(), 0);
    EXPECT(!button->isEnabled());
    EXPECT(processes->selectedItems().isEmpty());
}
#endif

TEST_CASE(site_labels_handle_urls_without_hosts)
{
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("https://google.com/search?q=test"sv).value()), QString("google.com"));
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("about:blank"sv).value()), QString {});
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("file:///tmp/report.html"sv).value()), QString("report.html"));
    EXPECT_EQ(Ladybird::task_manager_site_label(URL::Parser::basic_parse("data:text/plain,hello"sv).value()), QString("data:"));
}
