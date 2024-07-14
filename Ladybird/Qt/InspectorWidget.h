/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "WebContentView.h"
#include <LibGfx/Point.h>
#include <LibWebView/Forward.h>
#include <QWidget>

class QAction;
class QMenu;

namespace Ladybird {

class WebContentView;

class InspectorWidget final : public QWidget {
    Q_OBJECT

public:
    InspectorWidget(QWidget* tab, WebContentView& content_view);
    virtual ~InspectorWidget() override;

    void inspect();
    void reset();

    void select_hovered_node();
    void select_default_node();

public slots:
    void device_pixel_ratio_changed(qreal dpi);

private:
    virtual bool event(QEvent*) override;
    void closeEvent(QCloseEvent*) override;

    QScreen* m_current_screen;
    double m_device_pixel_ratio { 0 };

    WebContentView* m_inspector_view;
    OwnPtr<WebView::InspectorClient> m_inspector_client;

    QMenu* m_dom_node_text_context_menu { nullptr };
    QMenu* m_dom_node_tag_context_menu { nullptr };
    QMenu* m_dom_node_attribute_context_menu { nullptr };

    QAction* m_edit_node_action { nullptr };
    QAction* m_copy_node_action { nullptr };
    QAction* m_screenshot_node_action { nullptr };
    QAction* m_create_child_element_action { nullptr };
    QAction* m_create_child_text_node_action { nullptr };
    QAction* m_clone_node_action { nullptr };
    QAction* m_delete_node_action { nullptr };
    QAction* m_add_attribute_action { nullptr };
    QAction* m_remove_attribute_action { nullptr };
    QAction* m_copy_attribute_value_action { nullptr };
};

}
