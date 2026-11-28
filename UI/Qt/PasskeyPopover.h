/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWebView/PasskeyPrompts.h>
#include <LibWebView/ViewImplementation.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Popover.h>

class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace Ladybird {

// What the browser shows and asks the user while a site creates a passkey or signs in with one. The steps of one
// ceremony follow each other in the same popover: which passkey or authenticator to use, the touch of a security key,
// its PIN. Leaving the popover any other way than by its buttons (Escape, clicking outside it) answers the step it
// shows with nothing.
class PasskeyPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit PasskeyPopover(QWidget* parent);

    void update_chrome_style(QPalette const&);

    // No options means that the browser has no passkey for the site, which the user is told.
    void show_choice(WebView::PasskeyChoiceKind, QString const& site, Vector<Utf16String> const& options, WebView::PasskeyChoiceCallback);
    // NB: The answer comes only when the user cancels.
    void show_security_key_prompt(WebView::SecurityKeyPromptKind, QString const& site, Function<void()> on_cancel);
    void show_pin_prompt(WebView::SecurityKeyPinPromptReason, QString const& site, Function<void(Optional<String>)> on_pin);
    void show_message(WebView::PasskeyMessage, QString const& site);

    // The ceremony went on or ended without the user. A message stays for the user to read.
    void dismiss();

    // Hides the prompt while its tab isn't shown in the active window, without answering it.
    void suspend();
    void resume();

    // The tab went away with the prompt open.
    void abandon();

    // Runs each time the prompt comes up or changes its size, so whoever shows it can place it by its anchor.
    Function<void()> on_layout_change;

private:
    enum class Step : u8 {
        None,
        Choice,
        SecurityKey,
        Pin,
        Message,
    };

    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    QVBoxLayout& begin_step(Step, ChromeIcon, QString const& title, QString const& body);
    QPushButton& add_button(QVBoxLayout&, char const* object_name, QString const& text);
    enum class TakeFocus : u8 {
        No,
        Yes,
    };
    void present(QWidget* widget_to_focus, TakeFocus = TakeFocus::Yes);
    // A popup takes the keyboard and the pointer as it shows. The prompt that leaves the keyboard to the page is a
    // window that does not, and that looks for the press outside it and the Escape that leave it.
    void set_leaves_keyboard_to_page(bool);
    // The user answered the step. Its controls rest until the ceremony goes on or ends.
    void wait_for_next_step();
    // The user leaves the step without an answer, or the step is over.
    void leave();
    void give_no_answer();
    void update_icons();
    void submit_pin();

    Step m_step { Step::None };
    u64 m_step_serial { 0 };
    // The answer that the step still owes, if any.
    WebView::PasskeyChoiceCallback m_on_choice;
    Function<void()> m_on_cancel;
    Function<void(Optional<String>)> m_on_pin;
    ChromeIcon m_icon { ChromeIcon::Passkey };
    QWidget* m_content { nullptr };
    QLabel* m_icon_tile { nullptr };
    QLineEdit* m_pin_field { nullptr };
    QPushButton* m_pin_submit_button { nullptr };
    Vector<Function<void()>> m_icon_updaters;
    bool m_is_hiding_itself { false };
    bool m_suspended { false };
    bool m_leaves_keyboard_to_page { false };
};

}
