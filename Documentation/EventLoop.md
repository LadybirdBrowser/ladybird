# LibCore's `EventLoop` system

This is not about web event loops, which are a separate LibWeb concept.

For handling application tasks concurrently on one thread, LibCore provides the `EventLoop` system. The event loop is found in many systems, and it can be briefly summarized as an in-process loop, running all the time, which processes incoming events from signals, notifiers, file watchers, timers etc. by running associated callbacks. The event loop relies on the fact that the callbacks will eventually return control to it, so that other events can be processed. In that way, it's like a cooperative multitasking scheduler in userspace.

LibCore's event loop system is mainly used with graphical applications, and LibGUI and IPC are deeply integrated into it. If you are working on a command-line application, chances are you don't have to deal with the event loop, though that's not a universal rule.

## How it works

When an event loop runs, it usually sits in `exec()` repeatedly `pump()`ing the loop. `exec()` waits for an event to happen and then runs all the callbacks associated with that event. After that, it goes back to sleep and waits for more events to happen.

On a lower level, this is what happens:

-   On `exec()`, the event loop enters the event loop stack. Then, the event loop is pumped repeatedly.
-   Each `pump()` first puts the event loop to sleep with `wait_for_event()`, then handles all events.
-   `wait_for_event()` uses the `select(2)` function to wait until one of the prepared file descriptors becomes writeable or readable, or until a timeout expires. This means that while an event loop has nothing to do, the kernel keeps the thread at sleep. That's also the reason you'll see most GUI applications being listed with a "Selecting" state in SystemMonitor. After `select(2)` returns, it immediately dispatches signals to all signal handlers. Other new events (expired timers, file notifications) are put into the event queue.
    -   The specific file descriptors are the file notifiers the event loop has registered (this is the original purpose of `select(2)`) as well as the wake pipe. This pipe is responsible for two things: Once a POSIX signal is received which the event loop has a handler for (this might happen at any time, on any thread), `handle_signal()` is entered, which writes the handler number to the pipe. But second, the `wake()` function just writes 0 (not a valid signal number) to the wake pipe, which is used to trigger an event loop wake from other threads.
    -   The timeout of the `select(2)` call might be infinite if there's no time-based wake condition. If there are timers, however, the `select(2)` timeout is the minimum of all timer timeouts. If there are available events before `select(2)` is even called, the timeout is zero, leading to a `select(2)` which immediately returns.
-   Event handling from the event queue happens by invoking associated callbacks. Remember that the event queue has two primary sources of events: `deferred_invoke()` adds an event immediately and signals a wake, while after returning from `select(2)` new events are created based on timers and notifications.
-   If the event loop's exit was requested (checked while handling events), it returns the pending events to the queue so that the next-lower event loop can handle them. The assumption is that that event loop will resume running shortly. The return value of `exec()` has comparable purpose to a process return code, which is why the pattern `return app.exec()` is so common in GUI applications (`GUI::Application::exec()` just runs an event loop under the hood). In any case, even if the event loop's exit doesn't mean program exit, it is removed from the event loop stack.

All of this applies per thread. The event loops share some global state (notifiers, timers, event loop stack etc.) which are thread-local variables. The convenience getter `EventLoop::current()` relies on this to give you the topmost event loop on the thread you're calling from.

The **event loop stack** is mainly used for nesting GUI windows. Each window adds another event loop onto the stack, and remaining events are added to the lower event loop's queue when it exits. The purpose behind this system is that `GUI::Window`s and other systems which provide their own event loop can just create and run this class depending on global variables without interfering with other event loops.

## What it can do

An event loop handles several kinds of events:

-   POSIX signals can be registered with `EventLoop::register_signal()`. This means that the event loop of the calling thread registers the specified POSIX signal and callback with the kernel, and you can be sure that the signal handler will run as a normal event without the weirdness that comes with POSIX signal handlers (such as unspecified thread).
-   EventLoop::post_event() allows calling code to fire an event targeting a specific Core::EventReceiver the next time the event loop is pumped.
-   Similarly, an arbitrary callback can be called on the next event loop iteration with `EventLoop::deferred_invoke()`.
-   Timer events, i.e. events that fire after a certain timeout, possibly repeatedly, can be created with `EventLoop::register_timer` and `Object::start_timer()`. A more user-friendly version is the `Core::Timer` utility class which does the same thing and allows you to attach any callback to the timer.
-   For when a "file" becomes readable or writeable, the utility class `Core::Notifier` interfaces with the event loop system to handle exactly that.

Note that all events are registered, and therefore happen, on the event loop of the thread that created the event.

## Dos and Don'ts with event loops

-   DO NOT store an event loop in a global variable. Because the event loop itself relies on global variable initialization, UBSAN will catch an initialization order fiasco. DO create your main event loop in `main` and pass it to classes which need it.
-   DO NOT access the current event loop if you don't know on which thread you are running. If there is no event loop on your thread, the program will crash. DO receive the specific event loop you need to talk to as an initializer variable.
-   DO NOT `pump()` and/or `exec()` the event loop of another thread. While handling events is fine, going to sleep and waking up relies on thread-local variables. DO signal events to event loops on other threads.
