/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibCore/ThreadedPromise.h>
#include <LibTest/TestSuite.h>
#include <LibThreading/Thread.h>
#include <unistd.h>

TEST_CASE(promise_await_async_event)
{
    Core::EventLoop loop;

    auto promise = MUST(Core::Promise<int>::try_create());

    loop.deferred_invoke([=] {
        promise->resolve(42);
    });

    auto result = promise->await();
    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), 42);
}

TEST_CASE(promise_await_async_event_rejection)
{
    Core::EventLoop loop;

    auto promise = MUST(Core::Promise<int>::try_create());

    loop.deferred_invoke([=] {
        promise->reject(AK::Error::from_string_literal("lol no"));
    });

    auto result = promise->await();
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "lol no"sv);
}

TEST_CASE(promise_chain_handlers)
{
    Core::EventLoop loop;

    bool resolved = false;
    bool rejected = false;

    NonnullRefPtr<Core::Promise<int>> promise = MUST(Core::Promise<int>::try_create())
                                                    ->when_resolved([&](int&) -> ErrorOr<void> { resolved = true; return {}; })
                                                    .when_rejected([&](AK::Error const&) { rejected = true; });

    loop.deferred_invoke([=] {
        promise->resolve(42);
    });

    (void)promise->await();
    EXPECT(resolved);
    EXPECT(!rejected);
}

TEST_CASE(infallible_promise_chain_handlers)
{
    Core::EventLoop loop;

    bool resolved = false;
    bool rejected = false;

    NonnullRefPtr<Core::Promise<int>> promise = MUST(Core::Promise<int>::try_create())
                                                    ->when_resolved([&](int&) { resolved = true; })
                                                    .when_rejected([&](AK::Error const&) { rejected = true; });

    loop.deferred_invoke([=] {
        promise->resolve(42);
    });

    (void)promise->await();
    EXPECT(resolved);
    EXPECT(!rejected);
}

TEST_CASE(promise_map)
{
    Core::EventLoop loop;

    auto promise = MUST(Core::Promise<int>::try_create());
    auto mapped_promise = promise->map<int>([](int result) {
        return result * 2;
    });

    loop.deferred_invoke([=] {
        promise->resolve(21);
    });

    auto result = mapped_promise->await();
    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), 42);
}

TEST_CASE(promise_map_already_resolved)
{
    Core::EventLoop loop;

    auto promise = MUST(Core::Promise<int>::try_create());
    promise->resolve(21);

    auto mapped_promise = promise->map<int>([](int result) {
        return result * 2;
    });

    auto result = mapped_promise->await();
    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), 42);
}

TEST_CASE(threaded_promise_instantly_resolved)
{
    Core::EventLoop loop;

    bool resolved = false;
    bool rejected = true;
    Optional<pthread_t> thread_id;

    auto promise = Core::ThreadedPromise<int>::create();

    auto thread = Threading::Thread::construct([&, promise] {
        thread_id = pthread_self();
        promise->resolve(42);
        return 0;
    });
    thread->start();

    promise
        ->when_resolved([&](int result) {
            EXPECT(thread_id.has_value());
            EXPECT(pthread_equal(thread_id.value(), pthread_self()));
            resolved = true;
            rejected = false;
            EXPECT_EQ(result, 42);
        })
        .when_rejected([](Error&&) {
            VERIFY_NOT_REACHED();
        });

    promise->await();
    EXPECT(promise->has_completed());
    EXPECT(resolved);
    EXPECT(!rejected);
    MUST(thread->join());
}

TEST_CASE(threaded_promise_resolved_later)
{
    Core::EventLoop loop;

    IGNORE_USE_IN_ESCAPING_LAMBDA bool unblock_thread = false;
    bool resolved = false;
    bool rejected = true;
    Optional<pthread_t> thread_id;

    auto promise = Core::ThreadedPromise<int>::create();

    auto thread = Threading::Thread::construct([&, promise] {
        thread_id = pthread_self();
        while (!unblock_thread)
            usleep(500);
        promise->resolve(42);
        return 0;
    });
    thread->start();

    promise
        ->when_resolved([&]() {
            EXPECT(thread_id.has_value());
            EXPECT(pthread_equal(thread_id.value(), pthread_self()));
            EXPECT(unblock_thread);
            resolved = true;
            rejected = false;
        })
        .when_rejected([](Error&&) {
            VERIFY_NOT_REACHED();
        });

    Core::EventLoop::current().deferred_invoke([&]() { unblock_thread = true; });

    promise->await();
    EXPECT(promise->has_completed());
    EXPECT(unblock_thread);
    EXPECT(resolved);
    EXPECT(!rejected);
    MUST(thread->join());
}

TEST_CASE(threaded_promise_synchronously_resolved)
{
    Core::EventLoop loop;

    bool resolved = false;
    bool rejected = true;
    auto thread_id = pthread_self();

    auto promise = Core::ThreadedPromise<int>::create();
    promise->resolve(1337);

    promise
        ->when_resolved([&]() {
            EXPECT(pthread_equal(thread_id, pthread_self()));
            resolved = true;
            rejected = false;
        })
        .when_rejected([](Error&&) {
            VERIFY_NOT_REACHED();
        });

    promise->await();
    EXPECT(promise->has_completed());
    EXPECT(resolved);
    EXPECT(!rejected);
}
