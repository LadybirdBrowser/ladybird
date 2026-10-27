/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <UI/Qt/PermissionPromptPopover.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>

using Ladybird::PermissionPromptPopover;
using Ladybird::PermissionPromptQueue;
using Web::Permissions::PermissionName;
using Web::Permissions::PromptOutcome;

static WebView::PermissionPromptRequest request_for(StringView url, PermissionName name = PermissionName::Geolocation, bool show_os_hint = false)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return { name, parsed_url->origin(), parsed_url->scheme() == "file"sv, show_os_hint };
}

template<typename T>
static T& child(QWidget& parent, char const* name)
{
    auto* widget = parent.findChild<T*>(QString::fromLatin1(name));
    VERIFY(widget);
    return *widget;
}

// A popover fades out before it hides, so what follows from hiding one arrives a moment later.
static void wait_until(Function<bool()> condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 2000)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
}

static void let_time_pass()
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 250)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
}

struct Recorder {
    Vector<PromptOutcome> outcomes;

    void attach(PermissionPromptPopover& popover)
    {
        popover.on_outcome = [this](PromptOutcome outcome) { outcomes.append(outcome); };
    }
};

TEST_CASE(buttons_map_to_outcomes)
{
    struct Case {
        char const* button;
        PromptOutcome outcome;
    };
    for (auto [button, outcome] : { Case { "LadybirdPermissionPromptAllowAlways", PromptOutcome::AllowAlways }, Case { "LadybirdPermissionPromptAllowOnce", PromptOutcome::AllowOnce }, Case { "LadybirdPermissionPromptBlock", PromptOutcome::Block } }) {
        PermissionPromptPopover popover(nullptr, request_for("https://maps.example.com/"sv));
        Recorder recorder;
        recorder.attach(popover);
        popover.show();
        child<QPushButton>(popover, button).click();
        VERIFY(recorder.outcomes.size() == 1);
        EXPECT(recorder.outcomes[0] == outcome);
        wait_until([&] { return !popover.isVisible(); });
        EXPECT(!popover.isVisible());
    }
}

TEST_CASE(escape_and_clicking_away_dismiss)
{
    {
        PermissionPromptPopover popover(nullptr, request_for("https://maps.example.com/"sv));
        Recorder recorder;
        recorder.attach(popover);
        popover.show();
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&popover, &escape);
        VERIFY(recorder.outcomes.size() == 1);
        EXPECT(recorder.outcomes[0] == PromptOutcome::Dismissed);
    }
    {
        // A popover closes when the user clicks outside it.
        PermissionPromptPopover popover(nullptr, request_for("https://maps.example.com/"sv));
        Recorder recorder;
        recorder.attach(popover);
        popover.show();
        popover.hide();
        wait_until([&] { return !recorder.outcomes.is_empty(); });
        VERIFY(recorder.outcomes.size() == 1);
        EXPECT(recorder.outcomes[0] == PromptOutcome::Dismissed);
    }
}

TEST_CASE(an_answer_is_reported_once)
{
    PermissionPromptPopover popover(nullptr, request_for("https://maps.example.com/"sv));
    Recorder recorder;
    recorder.attach(popover);
    popover.show();
    child<QPushButton>(popover, "LadybirdPermissionPromptBlock").click();
    popover.show();
    popover.hide();
    let_time_pass();
    EXPECT_EQ(recorder.outcomes.size(), 1u);
}

TEST_CASE(question_and_answers)
{
    PermissionPromptPopover site(nullptr, request_for("https://maps.example.com/"sv));
    EXPECT_EQ(child<QLabel>(site, "LadybirdPermissionPromptTitle").text(), QStringLiteral("maps.example.com wants to know your location"));

    PermissionPromptPopover port(nullptr, request_for("https://maps.example.com:8443/"sv, PermissionName::Microphone));
    EXPECT_EQ(child<QLabel>(port, "LadybirdPermissionPromptTitle").text(), QStringLiteral("maps.example.com:8443 wants to use your microphone"));

    PermissionPromptPopover file(nullptr, request_for("file:///Users/someone/map.html"sv));
    EXPECT_EQ(child<QLabel>(file, "LadybirdPermissionPromptTitle").text(), QStringLiteral("This file wants to know your location"));

    EXPECT_EQ(child<QPushButton>(site, "LadybirdPermissionPromptAllowAlways").text(), QStringLiteral("Allow while visiting the site"));
    EXPECT_EQ(child<QPushButton>(site, "LadybirdPermissionPromptAllowOnce").text(), QStringLiteral("Allow this time"));
    EXPECT_EQ(child<QPushButton>(site, "LadybirdPermissionPromptBlock").text(), QStringLiteral("Never allow"));
}

TEST_CASE(hint_shows_only_when_requested)
{
    PermissionPromptPopover without_hint(nullptr, request_for("https://maps.example.com/"sv));
    EXPECT(child<QLabel>(without_hint, "LadybirdPermissionPromptHint").isHidden());

    PermissionPromptPopover with_hint(nullptr, request_for("https://maps.example.com/"sv, PermissionName::Geolocation, true));
    auto& hint = child<QLabel>(with_hint, "LadybirdPermissionPromptHint");
    EXPECT(!hint.isHidden());
    EXPECT_EQ(hint.text(), QStringLiteral("macOS will then ask you to let Ladybird use your location."));
}

TEST_CASE(suspending_hides_without_an_answer)
{
    PermissionPromptPopover popover(nullptr, request_for("https://maps.example.com/"sv));
    Recorder recorder;
    recorder.attach(popover);
    popover.show();
    popover.suspend();
    wait_until([&] { return !popover.isVisible(); });
    EXPECT(!popover.isVisible());
    EXPECT(recorder.outcomes.is_empty());
    popover.resume();
    EXPECT(popover.isVisible());
    popover.ignore();
    VERIFY(recorder.outcomes.size() == 1);
    EXPECT(recorder.outcomes[0] == PromptOutcome::Ignored);
}

TEST_CASE(queued_requests_show_one_at_a_time)
{
    PermissionPromptQueue queue;
    Vector<PermissionPromptPopover*> shown;
    queue.create_popover = [&](WebView::PermissionPromptRequest const& request) {
        auto* popover = new PermissionPromptPopover(nullptr, request);
        shown.append(popover);
        return popover;
    };

    Vector<PromptOutcome> first_outcomes;
    Vector<PromptOutcome> second_outcomes;
    queue.enqueue(request_for("https://maps.example.com/"sv), [&](PromptOutcome outcome) { first_outcomes.append(outcome); });
    queue.enqueue(request_for("https://talk.example.org/"sv, PermissionName::Microphone), [&](PromptOutcome outcome) { second_outcomes.append(outcome); });
    VERIFY(shown.size() == 1);

    child<QPushButton>(*shown[0], "LadybirdPermissionPromptAllowOnce").click();
    EXPECT(first_outcomes == Vector { PromptOutcome::AllowOnce });
    VERIFY(shown.size() == 2);
    EXPECT_EQ(child<QLabel>(*shown[1], "LadybirdPermissionPromptTitle").text(), QStringLiteral("talk.example.org wants to use your microphone"));

    queue.ignore_all();
    EXPECT(second_outcomes == Vector { PromptOutcome::Ignored });
}

TEST_CASE(cancelling_a_queued_request_drops_it_silently)
{
    PermissionPromptQueue queue;
    Vector<PermissionPromptPopover*> shown;
    queue.create_popover = [&](WebView::PermissionPromptRequest const& request) {
        auto* popover = new PermissionPromptPopover(nullptr, request);
        shown.append(popover);
        return popover;
    };

    Vector<PromptOutcome> outcomes;
    auto first = request_for("https://maps.example.com/"sv);
    queue.enqueue(first, [&](PromptOutcome outcome) { outcomes.append(outcome); });
    queue.cancel(first);
    wait_until([&] { return !shown[0]->isVisible(); });
    EXPECT(outcomes.is_empty());
    EXPECT(!shown[0]->isVisible());
}
