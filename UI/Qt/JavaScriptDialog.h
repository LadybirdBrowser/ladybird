/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/kmalloc.h>

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QDialogButtonBox;
class QFrame;
class QGraphicsDropShadowEffect;
class QIcon;
class QLabel;
class QLineEdit;
class QScrollArea;
class QVariantAnimation;

namespace Ladybird {

class JavaScriptDialog final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    enum class Type {
        Alert,
        BeforeUnload,
        Confirm,
        Prompt,
    };

    // From a page's second dialog on, the dialog offers to stop the page from showing more.
    enum class OfferToBlockFurtherDialogs : bool {
        No,
        Yes,
    };

    explicit JavaScriptDialog(QWidget* parent);

    bool is_open() const { return m_type.has_value(); }
    Optional<Type> type() const { return m_type; }

    void show_alert(QString const& title, QString const& message, OfferToBlockFurtherDialogs = OfferToBlockFurtherDialogs::No);
    void show_before_unload(QString const& title);
    void show_confirm(QString const& title, QString const& message, OfferToBlockFurtherDialogs = OfferToBlockFurtherDialogs::No);
    void show_prompt(QString const& title, QString const& message, QString const& default_text, OfferToBlockFurtherDialogs = OfferToBlockFurtherDialogs::No);
    void set_prompt_text(QString const& text);
    // The icon of the site the dialogs come from, shown before the title.
    void set_title_icon(QIcon const&);
    // Whether the user asked, in the dialog that is open or that just closed, to block the page's further dialogs.
    bool blocks_further_dialogs() const;
    void accept();
    void dismiss();
    void reset();

    Function<void(Type, bool accepted, QString const& prompt_text)> on_complete;
    // Dims the page under the dialog, from 0 for not at all to 1 for fully.
    Function<void(qreal)> on_page_dim_change;

protected:
    virtual bool event(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;
    virtual bool focusNextPrevChild(bool next) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;

private:
    bool is_dialog_widget(QWidget const&) const;
    bool is_blocked_content_widget(QWidget const&) const;
    void open(Type, QString const& title, QString const& message, QString const& default_text = {}, OfferToBlockFurtherDialogs = OfferToBlockFurtherDialogs::No);
    void complete(bool accepted);
    void focus_dialog();
    void restore_focus_after_close();
    void restore_parent_focus_policy();
    void update_chrome_style();
    void update_geometry_constraints();

    Optional<Type> m_type;
    QFrame* m_panel { nullptr };
#if defined(AK_OS_MACOS)
    QWidget* m_panel_floating_shadow { nullptr };
#else
    QGraphicsDropShadowEffect* m_panel_shadow { nullptr };
#endif
    QLabel* m_title_icon { nullptr };
    QLabel* m_title_label { nullptr };
    QLabel* m_message_label { nullptr };
    QScrollArea* m_message_scroll_area { nullptr };
    QLineEdit* m_prompt_text { nullptr };
    QCheckBox* m_block_further_dialogs { nullptr };
    QDialogButtonBox* m_button_box { nullptr };
    QPointer<QWidget> m_previous_focus_widget;
    Optional<Qt::FocusPolicy> m_parent_focus_policy;
    bool m_is_updating_chrome_style { false };
    QVariantAnimation* m_opening_animation { nullptr };
    void set_opening_progress(qreal);
};

}
