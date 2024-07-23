/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "InspectorWidget.h"
#include <Ladybird/Qt/StringUtils.h>
#include <LibWebView/Attribute.h>
#include <LibWebView/InspectorClient.h>
#include <QAction>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QVBoxLayout>
#include <QWindow>

namespace Ladybird {

extern bool is_using_dark_system_theme(QWidget&);

InspectorWidget::InspectorWidget(QWidget* tab, WebContentView& content_view)
    : QWidget(tab, Qt::Window)
{
    m_inspector_view = new WebContentView(this, content_view.web_content_options(), {});

    if (is_using_dark_system_theme(*this))
        m_inspector_view->update_palette(WebContentView::PaletteMode::Dark);

    m_inspector_client = make<WebView::InspectorClient>(content_view, *m_inspector_view);

    m_edit_node_action = new QAction("&Edit node", this);
    connect(m_edit_node_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_edit_dom_node(); });

    m_copy_node_action = new QAction("&Copy HTML", this);
    connect(m_copy_node_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_copy_dom_node(); });

    m_screenshot_node_action = new QAction("Take node &screenshot", this);
    connect(m_screenshot_node_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_screenshot_dom_node(); });

    m_create_child_element_action = new QAction("Create child &element", this);
    connect(m_create_child_element_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_create_child_element(); });

    m_create_child_text_node_action = new QAction("Create child &text node", this);
    connect(m_create_child_text_node_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_create_child_text_node(); });

    m_clone_node_action = new QAction("C&lone node", this);
    connect(m_clone_node_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_clone_dom_node(); });

    m_delete_node_action = new QAction("&Delete node", this);
    connect(m_delete_node_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_remove_dom_node(); });

    m_add_attribute_action = new QAction("&Add attribute", this);
    connect(m_add_attribute_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_add_dom_node_attribute(); });

    m_remove_attribute_action = new QAction("&Remove attribute", this);
    connect(m_remove_attribute_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_remove_dom_node_attribute(); });

    m_copy_attribute_value_action = new QAction("Copy attribute &value", this);
    connect(m_copy_attribute_value_action, &QAction::triggered, [this]() { m_inspector_client->context_menu_copy_dom_node_attribute_value(); });

    m_dom_node_text_context_menu = new QMenu("DOM text context menu", this);
    m_dom_node_text_context_menu->addAction(m_edit_node_action);
    m_dom_node_text_context_menu->addAction(m_copy_node_action);
    m_dom_node_text_context_menu->addSeparator();
    m_dom_node_text_context_menu->addAction(m_delete_node_action);

    auto* create_child_menu = new QMenu("Create child", this);
    create_child_menu->addAction(m_create_child_element_action);
    create_child_menu->addAction(m_create_child_text_node_action);

    m_dom_node_tag_context_menu = new QMenu("DOM tag context menu", this);
    m_dom_node_tag_context_menu->addAction(m_edit_node_action);
    m_dom_node_tag_context_menu->addSeparator();
    m_dom_node_tag_context_menu->addAction(m_add_attribute_action);
    m_dom_node_tag_context_menu->addMenu(create_child_menu);
    m_dom_node_tag_context_menu->addAction(m_clone_node_action);
    m_dom_node_tag_context_menu->addAction(m_delete_node_action);
    m_dom_node_tag_context_menu->addSeparator();
    m_dom_node_tag_context_menu->addAction(m_copy_node_action);
    m_dom_node_tag_context_menu->addAction(m_screenshot_node_action);

    m_dom_node_attribute_context_menu = new QMenu("DOM attribute context menu", this);
    m_dom_node_attribute_context_menu->addAction(m_edit_node_action);
    m_dom_node_attribute_context_menu->addAction(m_copy_attribute_value_action);
    m_dom_node_attribute_context_menu->addAction(m_remove_attribute_action);
    m_dom_node_attribute_context_menu->addSeparator();
    m_dom_node_attribute_context_menu->addAction(m_add_attribute_action);
    m_dom_node_attribute_context_menu->addMenu(create_child_menu);
    m_dom_node_attribute_context_menu->addAction(m_clone_node_action);
    m_dom_node_attribute_context_menu->addAction(m_delete_node_action);
    m_dom_node_attribute_context_menu->addSeparator();
    m_dom_node_attribute_context_menu->addAction(m_copy_node_action);
    m_dom_node_attribute_context_menu->addAction(m_screenshot_node_action);

    m_inspector_client->on_requested_dom_node_text_context_menu = [this](auto position) {
        m_edit_node_action->setText("&Edit text");
        m_copy_node_action->setText("&Copy text");

        m_dom_node_text_context_menu->exec(m_inspector_view->map_point_to_global_position(position));
    };

    m_inspector_client->on_requested_dom_node_tag_context_menu = [this](auto position, auto const& tag) {
        m_edit_node_action->setText(qstring_from_ak_string(MUST(String::formatted("&Edit \"{}\"", tag))));
        m_copy_node_action->setText("&Copy HTML");

        m_dom_node_tag_context_menu->exec(m_inspector_view->map_point_to_global_position(position));
    };

    m_inspector_client->on_requested_dom_node_attribute_context_menu = [this](auto position, auto const&, WebView::Attribute const& attribute) {
        static constexpr size_t MAX_ATTRIBUTE_VALUE_LENGTH = 32;

        m_copy_node_action->setText("&Copy HTML");
        m_edit_node_action->setText(qstring_from_ak_string(MUST(String::formatted("&Edit attribute \"{}\"", attribute.name))));
        m_remove_attribute_action->setText(qstring_from_ak_string(MUST(String::formatted("&Remove attribute \"{}\"", attribute.name))));
        m_copy_attribute_value_action->setText(qstring_from_ak_string(MUST(String::formatted("Copy attribute &value \"{:.{}}{}\"",
            attribute.value, MAX_ATTRIBUTE_VALUE_LENGTH,
            attribute.value.bytes_as_string_view().length() > MAX_ATTRIBUTE_VALUE_LENGTH ? "..."sv : ""sv))));

        m_dom_node_attribute_context_menu->exec(m_inspector_view->map_point_to_global_position(position));
    };

    setLayout(new QVBoxLayout);
    layout()->addWidget(m_inspector_view);

    setWindowTitle("Inspector");
    resize(875, 825);

    // Listen for DPI changes
    m_device_pixel_ratio = devicePixelRatio();
    m_current_screen = screen();
    if (QT_VERSION < QT_VERSION_CHECK(6, 6, 0) || QGuiApplication::platformName() != "wayland") {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        QObject::connect(m_current_screen, &QScreen::logicalDotsPerInchChanged, this, &InspectorWidget::device_pixel_ratio_changed);
        QObject::connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen* screen) {
            if (m_device_pixel_ratio != screen->devicePixelRatio())
                device_pixel_ratio_changed(screen->devicePixelRatio());

            // Listen for logicalDotsPerInchChanged signals on new screen
            QObject::disconnect(m_current_screen, &QScreen::logicalDotsPerInchChanged, nullptr, nullptr);
            m_current_screen = screen;
            QObject::connect(m_current_screen, &QScreen::logicalDotsPerInchChanged, this, &InspectorWidget::device_pixel_ratio_changed);
        });
    }
}

InspectorWidget::~InspectorWidget() = default;

void InspectorWidget::inspect()
{
    m_inspector_client->inspect();
}

void InspectorWidget::reset()
{
    m_inspector_client->reset();
}

void InspectorWidget::select_hovered_node()
{
    m_inspector_client->select_hovered_node();
}

void InspectorWidget::select_default_node()
{
    m_inspector_client->select_default_node();
}

void InspectorWidget::device_pixel_ratio_changed(qreal dpi)
{
    m_device_pixel_ratio = dpi;
    m_inspector_view->set_device_pixel_ratio(m_device_pixel_ratio);
}

bool InspectorWidget::event(QEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange) {
        if (m_device_pixel_ratio != devicePixelRatio())
            device_pixel_ratio_changed(devicePixelRatio());
    }
#endif

    return QWidget::event(event);
}

void InspectorWidget::closeEvent(QCloseEvent* event)
{
    event->accept();
    m_inspector_client->clear_selection();
}

}
