# Porting to new Operating System Environments

This document outlines the steps required to port Ladybird to a new OS environment, and expectations for such ports to
be accepted into the mainline repository.

## Types of Ports

There are two types of ports that can be made to Ladybird:

- UI port: We define the "browser frontend" as the UI layer. This includes the browser window, tabs, address bar, etc.
- Platform port: This includes the underlying platform-specific code that interacts with the OS. This includes things like
  file I/O, networking, and process management.

### UI Ports

There are currently three supported UI ports:

- Qt6: The generic UI port.
- AppKit/Cocoa: The macOS native port, which uses the AppKit framework.
- Headless: A headless port that does not have a UI, used for testing.

### Platform Ports

There are currently two supported platform ports:

- GNU/Linux: The Linux platform port that may work on other POSIX platforms
- macOS: The macOS platform port

Many other POSIX desktop platforms are known to work, or have worked in the past, but are not officially supported.
Among others, these include Alpine Linux, FreeBSD, OpenBSD, and Haiku. Support for these systems is community-driven,
meaning there is no regular CI to ensure that each master branch commit maintains their functionality.
Contributions to restore or improve compatibility are welcome!

There is currently one in progress platform port:

- Android: The Android platform port, using the Android SDK/NDK directly.
  The removal of `LibArchive` and the `DeprecatedPainter` from the repository has left gaps, as the port previously depended on these components.

## Porting Steps

### UI ports

UI ports mostly concern themselves with the UI layer. This means the main Ladybird process, using LibWebView.

To create a new Ladybird UI, you will need to implement a new `WebView::ViewImplementation` subclass.
ViewImplementation is the main interface between the UI process and WebContent processes. It is expected that each tab
of the browser will have its own WebContent process. This is all managed by the WebView layer.

Each UI port must also subclass `WebView::Application` to add any UI-specific command-line flags.

TODO: Explain any more details that are necessary

### Platform ports

Platform ports concern themselves with the underlying OS-specific code. In Ladybird, this code is largely contained in
the AK and LibCore libraries.

AK is the standard template library for Ladybird. The first step of a new platform port is a new platform define in
`AK/Platform.h`. This define will be used to conditionally compile platform-specific code.
In AK, the most likely class to need platform-specific code is `AK::StackInfo`.

LibCore is an abstraction over POSIX. It contains classes to wrap lower level OS functionality into APIs that are
comfortable for Ladybird developers to use. The most likely place to need adjustment is `Core::System`, followed by
`Core::Process` and `Core::Socket`.

Ladybird makes heavy use of IPC, and the IPC layer is in `LibIPC`. This layer is mostly platform-agnostic, but there are
some platform-specific details in LibCore that may need to be adjusted. The IPC system is based on Unix domain sockets,
so  any platform that supports Unix domain sockets should be able to use the IPC system out of the box.

## Special Consideration

### Windows

Over the years excitement about a native windows port has waxed and waned. The main issue is that Ladybird is built on
top of LibCore, which is a POSIX abstraction. Windows is not POSIX, and so a Windows port requires a significant amount
of effort to implement.

To limit the scope of the work, we have decided that a Windows port will need the following properties:

- Target x86_64-windows-msvc and arm64-windows-msvc only. We are not interested in accepting patches to make MinGW or MSYS2 work.
- The port must use `clang-cl.exe` as the compiler. We are not interested in accepting patches to make MSVC work.
- Platform `#ifdef`s must stay within AK and LibCore, with a few exceptions in LibWebView.
- Prefer separate cpp files with the same interface to `#ifdef`s within a single file.
- Avoid `#ifdef`s in headers, as much as possible.
