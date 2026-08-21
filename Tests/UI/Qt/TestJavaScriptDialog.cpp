/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <UI/Qt/JavaScriptDialog.h>

namespace {

struct Completion {
    Optional<Ladybird::JavaScriptDialog::Type> type;
    bool accepted { false };
    QString prompt_text;
};

class MouseRecordingWidget final : public QWidget {
public:
    int mouse_presses { 0 };

private:
    virtual void mousePressEvent(QMouseEvent*) override
    {
        ++mouse_presses;
    }
};

class KeyRecordingButton final : public QPushButton {
public:
    using QPushButton::QPushButton;

    int right_key_presses { 0 };

private:
    virtual void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Right)
            ++right_key_presses;
        QPushButton::keyPressEvent(event);
    }
};

void record_completions(Ladybird::JavaScriptDialog& dialog, Completion& completion)
{
    dialog.on_complete = [&completion](auto type, bool accepted, auto const& prompt_text) {
        completion.type = type;
        completion.accepted = accepted;
        completion.prompt_text = prompt_text;
    };
}

void trigger_shortcut(Ladybird::JavaScriptDialog& dialog, int key)
{
    for (auto* shortcut : dialog.findChildren<QShortcut*>()) {
        if (shortcut->key() != QKeySequence(key))
            continue;
        VERIFY(QMetaObject::invokeMethod(shortcut, "activated"));
        QApplication::processEvents();
        return;
    }
    VERIFY_NOT_REACHED();
}

void send_key(QWidget& widget, int key)
{
    QKeyEvent press { QEvent::KeyPress, key, Qt::NoModifier };
    QApplication::sendEvent(&widget, &press);
    QKeyEvent release { QEvent::KeyRelease, key, Qt::NoModifier };
    QApplication::sendEvent(&widget, &release);
    QApplication::processEvents();
}

}

TEST_CASE(alert_and_confirm_report_the_expected_results)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::JavaScriptDialog dialog(&web_view);
    Completion completion;
    record_completions(dialog, completion);

    dialog.show_alert("https://example.com", "Alert message");
    EXPECT(dialog.is_open());
    EXPECT(dialog.isVisible());
    EXPECT_EQ(dialog.geometry(), web_view.rect());
    auto* title_label = dialog.findChild<QLabel*>("LadybirdJavaScriptDialogTitle");
    auto* button_box = dialog.findChild<QDialogButtonBox*>("LadybirdJavaScriptDialogButtons");
    VERIFY(title_label);
    VERIFY(button_box);
    EXPECT(title_label->text() == "https://example.com");
    VERIFY(button_box->button(QDialogButtonBox::Ok));
    EXPECT(button_box->button(QDialogButtonBox::Ok)->isDefault());

    dialog.dismiss();
    EXPECT(!dialog.is_open());
    EXPECT(completion.type == Ladybird::JavaScriptDialog::Type::Alert);
    EXPECT(completion.accepted);

    completion = {};
    dialog.show_confirm("https://example.com", "Confirm message");
    auto* cancel_button = button_box->button(QDialogButtonBox::Cancel);
    VERIFY(cancel_button);
    cancel_button->setFocus();
    send_key(*cancel_button, Qt::Key_Return);
    EXPECT(completion.type == Ladybird::JavaScriptDialog::Type::Confirm);
    EXPECT(!completion.accepted);

    completion = {};
    dialog.show_confirm("https://example.com", "Confirm message");
    dialog.accept();
    EXPECT(completion.type == Ladybird::JavaScriptDialog::Type::Confirm);
    EXPECT(completion.accepted);

    completion = {};
    dialog.show_alert("https://example.com", "Original alert");
    dialog.show_confirm("https://ladybird.org", "Overlapping confirm");
    EXPECT(dialog.is_open());
    EXPECT(title_label->text() == "https://example.com");
    EXPECT(!button_box->button(QDialogButtonBox::Cancel));
    dialog.dismiss();
    EXPECT(completion.type == Ladybird::JavaScriptDialog::Type::Alert);
    EXPECT(completion.accepted);
}

TEST_CASE(prompt_supports_defaults_updates_and_keyboard_completion)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.show();

    Ladybird::JavaScriptDialog dialog(&web_view);
    Completion completion;
    record_completions(dialog, completion);

    dialog.show_prompt("https://example.com", "Prompt message", "default text");
    auto* prompt = dialog.findChild<QLineEdit*>("LadybirdJavaScriptDialogPrompt");
    auto* button_box = dialog.findChild<QDialogButtonBox*>("LadybirdJavaScriptDialogButtons");
    VERIFY(prompt);
    VERIFY(button_box);
    auto* ok_button = button_box->button(QDialogButtonBox::Ok);
    VERIFY(ok_button);
    EXPECT(prompt->isVisible());
    EXPECT(prompt->text() == "default text");
    EXPECT(prompt->hasSelectedText());
    QApplication::processEvents();
    EXPECT(!ok_button->isDefault());

    dialog.set_prompt_text("updated text");
    EXPECT(prompt->text() == "updated text");
    send_key(*prompt, Qt::Key_Return);
    EXPECT(completion.type == Ladybird::JavaScriptDialog::Type::Prompt);
    EXPECT(completion.accepted);
    EXPECT(completion.prompt_text == "updated text");

    completion = {};
    dialog.show_prompt("https://example.com", "Prompt message", "ignored");
    trigger_shortcut(dialog, Qt::Key_Escape);
    EXPECT(completion.type == Ladybird::JavaScriptDialog::Type::Prompt);
    EXPECT(!completion.accepted);
}

TEST_CASE(dialog_resizes_with_and_blocks_only_its_parent_view)
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* browser_button = new KeyRecordingButton("Browser control", &window);
    auto* web_view = new MouseRecordingWidget;
    web_view->setParent(&window);
    web_view->setFocusPolicy(Qt::StrongFocus);
    web_view->setMinimumSize(400, 300);
    auto* page_button = new QPushButton("Page control", web_view);
    layout->addWidget(browser_button);
    layout->addWidget(web_view);
    window.resize(800, 600);
    window.show();

    Ladybird::JavaScriptDialog dialog(web_view);
    dialog.show_prompt("https://example.com", "Prompt message", "Prompt text");
    QApplication::processEvents();

    EXPECT_EQ(dialog.geometry(), web_view->rect());
    EXPECT(web_view->childAt(1, 1) == &dialog);
    EXPECT_EQ(dialog.focusPolicy(), Qt::NoFocus);
    EXPECT_EQ(web_view->focusPolicy(), Qt::NoFocus);

    auto* prompt = dialog.findChild<QLineEdit*>("LadybirdJavaScriptDialogPrompt");
    VERIFY(prompt);
    EXPECT(prompt->hasFocus());

    auto global_mouse_position = dialog.mapToGlobal(QPoint { 1, 1 });
    auto* mouse_target = QApplication::widgetAt(global_mouse_position);
    VERIFY(mouse_target);
    QMouseEvent mouse_press {
        QEvent::MouseButtonPress,
        mouse_target->mapFromGlobal(global_mouse_position),
        global_mouse_position,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier,
    };
    QApplication::sendEvent(mouse_target, &mouse_press);
    EXPECT_EQ(web_view->mouse_presses, 0);
    EXPECT(prompt->hasFocus());

    bool browser_button_clicked = false;
    QObject::connect(browser_button, &QPushButton::clicked, [&] {
        browser_button_clicked = true;
    });
    browser_button->click();
    EXPECT(browser_button_clicked);

    auto* button_box = dialog.findChild<QDialogButtonBox*>("LadybirdJavaScriptDialogButtons");
    VERIFY(button_box);
    auto* cancel_button = button_box->button(QDialogButtonBox::Cancel);
    auto* ok_button = button_box->button(QDialogButtonBox::Ok);
    VERIFY(cancel_button);
    VERIFY(ok_button);

    QWidget::setTabOrder(prompt, browser_button);
    QWidget::setTabOrder(browser_button, ok_button);
    QWidget::setTabOrder(ok_button, cancel_button);
    dialog.dismiss();
    dialog.show_prompt("https://example.com", "Prompt message", "Prompt text");

    prompt = dialog.findChild<QLineEdit*>("LadybirdJavaScriptDialogPrompt");
    cancel_button = button_box->button(QDialogButtonBox::Cancel);
    ok_button = button_box->button(QDialogButtonBox::Ok);
    VERIFY(prompt);
    VERIFY(cancel_button);
    VERIFY(ok_button);

    prompt->setFocus();
    send_key(*prompt, Qt::Key_Tab);
    EXPECT(ok_button->hasFocus());

    send_key(*ok_button, Qt::Key_Tab);
    EXPECT(cancel_button->hasFocus());

    send_key(*cancel_button, Qt::Key_Tab);
    EXPECT(browser_button->hasFocus());

    cancel_button->setFocus();
    send_key(*cancel_button, Qt::Key_Right);
    EXPECT(browser_button->hasFocus());

    send_key(*browser_button, Qt::Key_Right);
    EXPECT_EQ(browser_button->right_key_presses, 1);

    QWidget::setTabOrder(browser_button, page_button);
    QWidget::setTabOrder(page_button, prompt);
    browser_button->setFocus();
    send_key(*browser_button, Qt::Key_Tab);
    EXPECT(!page_button->hasFocus());
    EXPECT(prompt->hasFocus());

    browser_button->setFocus();
    dialog.dismiss();
    EXPECT(browser_button->hasFocus());
    EXPECT_EQ(web_view->focusPolicy(), Qt::StrongFocus);

    web_view->resize(640, 480);
    QApplication::processEvents();
    EXPECT_EQ(dialog.geometry(), web_view->rect());
}

TEST_CASE(dialog_visibility_and_state_follow_the_owning_tab)
{
    QStackedWidget tabs;
    auto* first_tab = new QWidget(&tabs);
    auto* second_tab = new QWidget(&tabs);
    tabs.addWidget(first_tab);
    tabs.addWidget(second_tab);
    tabs.resize(800, 600);
    tabs.show();

    Ladybird::JavaScriptDialog dialog(first_tab);
    dialog.show_prompt("https://example.com", "Prompt message", "preserved text");
    QApplication::processEvents();
    EXPECT(dialog.isVisible());

    tabs.setCurrentWidget(second_tab);
    QApplication::processEvents();
    EXPECT(dialog.is_open());
    EXPECT(!dialog.isVisible());

    tabs.setCurrentWidget(first_tab);
    QApplication::processEvents();
    EXPECT(dialog.isVisible());
    auto* prompt = dialog.findChild<QLineEdit*>("LadybirdJavaScriptDialogPrompt");
    VERIFY(prompt);
    EXPECT(prompt->text() == "preserved text");
}

TEST_CASE(reset_closes_without_reporting_completion)
{
    QWidget web_view;
    web_view.resize(800, 600);
    web_view.setFocusPolicy(Qt::StrongFocus);
    web_view.show();

    Ladybird::JavaScriptDialog dialog(&web_view);
    Completion completion;
    record_completions(dialog, completion);

    web_view.setFocus();
    QApplication::processEvents();
    VERIFY(web_view.hasFocus());
    dialog.show_alert("https://example.com", "Alert message");
    dialog.reset();

    EXPECT(!dialog.is_open());
    EXPECT(!dialog.isVisible());
    EXPECT(!completion.type.has_value());
    EXPECT(web_view.hasFocus());
}

TEST_CASE(long_messages_remain_plain_text_and_bounded)
{
    QWidget web_view;
    web_view.resize(480, 320);
    web_view.show();

    Ladybird::JavaScriptDialog dialog(&web_view);
    QString message = "<b>not markup</b> ";
    message = message.repeated(100);
    dialog.show_alert("https://example.com", message);
    QApplication::processEvents();

    auto* message_label = dialog.findChild<QLabel*>("LadybirdJavaScriptDialogMessage");
    auto* message_area = dialog.findChild<QScrollArea*>("LadybirdJavaScriptDialogMessageArea");
    VERIFY(message_label);
    VERIFY(message_area);
    EXPECT_EQ(message_label->textFormat(), Qt::PlainText);
    EXPECT(message_label->text() == message);
    EXPECT(message_area->height() <= 240);
    EXPECT(message_area->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded);
}

TEST_CASE(dialog_keeps_controls_visible_with_a_large_font)
{
    QWidget web_view;
    web_view.resize(480, 320);
    web_view.show();

    Ladybird::JavaScriptDialog dialog(&web_view);
    auto font = dialog.font();
    font.setPointSize(24);
    dialog.setFont(font);
    dialog.show_prompt("https://example.com", QString("Long message ").repeated(100), "Prompt text");
    QApplication::processEvents();

    auto* button_box = dialog.findChild<QDialogButtonBox*>("LadybirdJavaScriptDialogButtons");
    auto* prompt = dialog.findChild<QLineEdit*>("LadybirdJavaScriptDialogPrompt");
    VERIFY(button_box);
    VERIFY(prompt);
    EXPECT(dialog.rect().contains(button_box->geometry().bottomRight() + button_box->parentWidget()->pos()));
    EXPECT(dialog.rect().contains(prompt->geometry().bottomRight() + prompt->parentWidget()->pos()));
}
