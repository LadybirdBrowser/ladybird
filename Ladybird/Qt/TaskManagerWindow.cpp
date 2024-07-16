/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TaskManagerWindow.h"
#include <LibWebView/Application.h>
#include <QVBoxLayout>

namespace Ladybird {

TaskManagerWindow::TaskManagerWindow(QWidget* parent, WebContentOptions const& web_content_options)
    : QWidget(parent, Qt::WindowFlags(Qt::WindowType::Window))
    , m_web_view(new WebContentView(this, web_content_options, {}))
{
    setLayout(new QVBoxLayout);
    layout()->addWidget(m_web_view);

    setWindowTitle("Task Manager");
    resize(600, 400);

    m_update_timer.setInterval(1000);

    QObject::connect(&m_update_timer, &QTimer::timeout, [this] {
        this->update_statistics();
    });

    update_statistics();
}

void TaskManagerWindow::showEvent(QShowEvent*)
{
    m_update_timer.start();
}

void TaskManagerWindow::hideEvent(QHideEvent*)
{
    m_update_timer.stop();
}

void TaskManagerWindow::update_statistics()
{
    WebView::Application::the().update_process_statistics();
    m_web_view->load_html(WebView::Application::the().generate_process_statistics_html());
}

}
