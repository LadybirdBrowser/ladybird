/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/AccessibilityNodeData.h>
#include <LibWebView/AccessibilityTreeManager.h>

#include <QAccessible>
#include <QAccessibleInterface>
#include <QAccessibleWidget>
#include <QRect>
#include <QString>
#include <QWidget>

namespace Ladybird {

class WebContentView;

QAccessibleInterface* accessibility_factory(QString const& class_name, QObject* object);

// Bridges the WebContentView widget into Qt's accessibility hierarchy.
// Returns the document root AccessibilityInterface as its child.
class WebContentViewAccessible : public QAccessibleWidget {
public:
    explicit WebContentViewAccessible(WebContentView* view);

    int childCount() const override;
    QAccessibleInterface* child(int index) const override;
    QAccessibleInterface* focusChild() const override;
    int indexOfChild(QAccessibleInterface const*) const override;
    QAccessible::Role role() const override;
};

class AccessibilityInterface
    : public QAccessibleInterface
    , public QAccessibleActionInterface
    , public QAccessibleTextInterface
    , public QAccessibleTableCellInterface {
public:
    AccessibilityInterface(i64 node_id, WebView::AccessibilityTreeManager const* manager, WebContentView* view);
    ~AccessibilityInterface() override;

    i64 node_id() const { return m_node_id; }
    WebView::AccessibilityNodeData const* node_data() const;

    // QAccessibleInterface
    bool isValid() const override;
    QObject* object() const override;
    QAccessibleInterface* childAt(int x, int y) const override;
    void* interface_cast(QAccessible::InterfaceType type) override;

    QAccessibleInterface* parent() const override;
    QAccessibleInterface* child(int index) const override;
    QAccessibleInterface* focusChild() const override;
    int childCount() const override;
    int indexOfChild(QAccessibleInterface const*) const override;

    QString text(QAccessible::Text t) const override;
    void setText(QAccessible::Text t, QString const& text) override;
    QRect rect() const override;
    QAccessible::Role role() const override;
    QAccessible::State state() const override;

    // QAccessibleActionInterface
    QStringList actionNames() const override;
    void doAction(QString const& actionName) override;
    QStringList keyBindingsForAction(QString const& actionName) const override;

    // QAccessibleTextInterface
    void addSelection(int startOffset, int endOffset) override;
    QString attributes(int offset, int* startOffset, int* endOffset) const override;
    int cursorPosition() const override;
    QRect characterRect(int offset) const override;
    int selectionCount() const override;
    int offsetAtPoint(QPoint const& point) const override;
    void selection(int selectionIndex, int* startOffset, int* endOffset) const override;
    QString text(int startOffset, int endOffset) const override;
    void removeSelection(int selectionIndex) override;
    void setCursorPosition(int position) override;
    void setSelection(int selectionIndex, int startOffset, int endOffset) override;
    int characterCount() const override;
    void scrollToSubstring(int startIndex, int endIndex) override;

    // QAccessibleTableCellInterface
    int columnExtent() const override;
    QList<QAccessibleInterface*> columnHeaderCells() const override;
    int columnIndex() const override;
    int rowExtent() const override;
    QList<QAccessibleInterface*> rowHeaderCells() const override;
    int rowIndex() const override;
    bool isSelected() const override;
    QAccessibleInterface* table() const override;

private:
    QAccessible::Role map_role() const;
    void collect_unignored_children(i64 node_id, QList<i64>& out) const;
    i64 find_unignored_parent(i64 node_id) const;
    bool is_ignored(WebView::AccessibilityNodeData const* data) const;

    i64 m_node_id;
    WebView::AccessibilityTreeManager const* m_manager;
    WebContentView* m_view;
    QAccessible::Id m_id = 0;
    QObject* m_object = nullptr;
};

}
