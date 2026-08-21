/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>

#include <QPointer>
#include <QWidget>

class QDialogButtonBox;
class QFrame;
class QLabel;
class QLineEdit;
class QScrollArea;

namespace Ladybird {

class JavaScriptDialog final : public QWidget {
public:
    enum class Type {
        Alert,
        Confirm,
        Prompt,
    };

    explicit JavaScriptDialog(QWidget* parent);

    bool is_open() const { return m_type.has_value(); }

    void show_alert(QString const& title, QString const& message);
    void show_confirm(QString const& title, QString const& message);
    void show_prompt(QString const& title, QString const& message, QString const& default_text);
    void set_prompt_text(QString const& text);
    void accept();
    void dismiss();
    void reset();

    Function<void(Type, bool accepted, QString const& prompt_text)> on_complete;

protected:
    virtual bool event(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual bool focusNextPrevChild(bool next) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void showEvent(QShowEvent*) override;

private:
    bool is_dialog_widget(QWidget const&) const;
    bool is_blocked_content_widget(QWidget const&) const;
    void open(Type, QString const& title, QString const& message, QString const& default_text = {});
    void complete(bool accepted);
    void focus_dialog();
    void restore_focus_after_close();
    void restore_parent_focus_policy();
    void update_chrome_style();
    void update_geometry_constraints();

    Optional<Type> m_type;
    QFrame* m_panel { nullptr };
    QLabel* m_title_label { nullptr };
    QLabel* m_message_label { nullptr };
    QScrollArea* m_message_scroll_area { nullptr };
    QLineEdit* m_prompt_text { nullptr };
    QDialogButtonBox* m_button_box { nullptr };
    QPointer<QWidget> m_previous_focus_widget;
    Optional<Qt::FocusPolicy> m_parent_focus_policy;
    bool m_is_updating_chrome_style { false };
};

}
