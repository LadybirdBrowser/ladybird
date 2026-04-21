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

// Bridges the WebContentView widget into Qt's accessibility hierarchy. Returns the document root AccessibilityInterface
// as its child.
class WebContentViewAccessible : public QAccessibleWidget {
public:
    explicit WebContentViewAccessible(WebContentView* view);

    virtual int childCount() const override;
    virtual QAccessibleInterface* child(int index) const override;
    virtual QAccessibleInterface* focusChild() const override;
    virtual int indexOfChild(QAccessibleInterface const*) const override;
    virtual QAccessible::Role role() const override;
};

class AccessibilityInterface
    : public QAccessibleInterface
    , public QAccessibleActionInterface
    , public QAccessibleTextInterface
    , public QAccessibleTableCellInterface
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    , public QAccessibleAttributesInterface
#endif
{
public:
    AccessibilityInterface(i64 node_id, WebView::AccessibilityTreeManager const* manager, WebContentView* view);
    virtual ~AccessibilityInterface() override;

    i64 node_id() const { return m_node_id; }
    WebView::AccessibilityTreeManager const* accessibility_manager() const { return m_manager; }
    WebView::AccessibilityNodeData const* node_data() const;

    // QAccessibleInterface
    virtual bool isValid() const override;
    virtual QObject* object() const override;
    virtual QAccessibleInterface* childAt(int x, int y) const override;
    virtual void* interface_cast(QAccessible::InterfaceType type) override;

    virtual QAccessibleInterface* parent() const override;
    virtual QAccessibleInterface* child(int index) const override;
    virtual QAccessibleInterface* focusChild() const override;
    virtual int childCount() const override;
    virtual int indexOfChild(QAccessibleInterface const*) const override;

    virtual QString text(QAccessible::Text t) const override;
    virtual void setText(QAccessible::Text t, QString const& text) override;
    virtual QRect rect() const override;
    virtual QAccessible::Role role() const override;
    virtual QAccessible::State state() const override;

    // QAccessibleActionInterface
    virtual QStringList actionNames() const override;
    virtual void doAction(QString const& actionName) override;
    virtual QStringList keyBindingsForAction(QString const& actionName) const override;

    // QAccessibleTextInterface
    virtual void addSelection(int startOffset, int endOffset) override;
    virtual QString attributes(int offset, int* startOffset, int* endOffset) const override;
    virtual int cursorPosition() const override;
    virtual QRect characterRect(int offset) const override;
    virtual int selectionCount() const override;
    virtual int offsetAtPoint(QPoint const& point) const override;
    virtual void selection(int selectionIndex, int* startOffset, int* endOffset) const override;
    virtual QString text(int startOffset, int endOffset) const override;
    virtual void removeSelection(int selectionIndex) override;
    virtual void setCursorPosition(int position) override;
    virtual void setSelection(int selectionIndex, int startOffset, int endOffset) override;
    virtual int characterCount() const override;
    virtual void scrollToSubstring(int startIndex, int endIndex) override;
    virtual QString textAtOffset(int offset, QAccessible::TextBoundaryType boundaryType,
        int* startOffset, int* endOffset) const override;
    virtual QString textBeforeOffset(int offset, QAccessible::TextBoundaryType boundaryType,
        int* startOffset, int* endOffset) const override;
    virtual QString textAfterOffset(int offset, QAccessible::TextBoundaryType boundaryType,
        int* startOffset, int* endOffset) const override;

    // QAccessibleTableCellInterface
    virtual int columnExtent() const override;
    virtual QList<QAccessibleInterface*> columnHeaderCells() const override;
    virtual int columnIndex() const override;
    virtual int rowExtent() const override;
    virtual QList<QAccessibleInterface*> rowHeaderCells() const override;
    virtual int rowIndex() const override;
    virtual bool isSelected() const override;
    virtual QAccessibleInterface* table() const override;

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // QAccessibleAttributesInterface
    virtual QList<QAccessible::Attribute> attributeKeys() const override;
    virtual QVariant attributeValue(QAccessible::Attribute key) const override;
#endif

private:
    QAccessible::Role map_role() const;
    QString build_hypertext() const;
    QString flatten_descendant_text(i64 node_id) const;
    void collect_unignored_children(i64 node_id, QList<i64>& out) const;
    void collect_exposed_children(i64 node_id, QList<i64>& out) const;
    i64 find_unignored_parent(i64 node_id) const;
    bool is_ignored(WebView::AccessibilityNodeData const* data) const;

    i64 m_node_id;
    WebView::AccessibilityTreeManager const* m_manager;
    WebContentView* m_view;
    QAccessible::Id m_id = 0;
    QObject* m_object = nullptr;
};

}
