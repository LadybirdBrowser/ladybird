# Ladybird Android

This directory contains the Android application shell for the Ladybird browser engine.
The app is built as a standard Android Gradle project that wraps a native shared
library (`libladybird.so`) compiled from the main Ladybird source tree via the Android
NDK and CMake.

---

## Prerequisites

### Host (Linux or macOS)

| Tool | Minimum version | Notes |
|------|-----------------|-------|
| **JDK** | 17 | Temurin or any OpenJDK 17 distribution |
| **Android SDK** | — | Installed by Android Studio or via [command-line tools](https://developer.android.com/studio#command-line-tools-only) |
| **Android NDK** | 29.0.13599879 (r29 β2) | Install with `sdkmanager "ndk;29.0.13599879"` |
| **CMake** | 3.30 | Must be the system default or on `$PATH` |
| **Ninja** | any | `ninja-build` on apt / `ninja` on brew |
| **Clang / Clang++** | LLVM 18 + | The host tools build uses the system compiler |
| **nasm** | any | Required by some vcpkg ports |
| **Rust toolchain** | stable | Install via [rustup](https://rustup.rs/) |
| **wasm-tools** | 1.243.0 | See [bytecodealliance/wasm-tools releases](https://github.com/bytecodealliance/wasm-tools/releases) |
| **Python** | 3.12 | With `pyyaml`, `requests`, `six` (`pip install pyyaml requests six`) |

> **macOS extras:** Xcode Command Line Tools, Homebrew, and the packages listed in
> [`Documentation/EditorConfiguration/AndroidStudioConfiguration.md`](../../Documentation/EditorConfiguration/AndroidStudioConfiguration.md).

> **Android Studio users:** Open the repository root (not this directory) in Android
> Studio. See [`Documentation/EditorConfiguration/AndroidStudioConfiguration.md`](../../Documentation/EditorConfiguration/AndroidStudioConfiguration.md)
> for the full IDE setup guide.

---

## First-time setup

1. **Install vcpkg** — the Ladybird build system bootstraps vcpkg automatically the
   first time it runs, but you can do it explicitly:

   ```bash
   ./Meta/Utils/build_vcpkg.py
   ```

2. **Install Android SDK components** (skip if Android Studio already installed them):

   ```bash
   # Accept licenses once
   yes | "${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin/sdkmanager" --licenses

   # Install the required components
   "${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin/sdkmanager" \
       "platform-tools" \
       "build-tools;35.0.0" \
       "platforms;android-35" \
       "ndk;29.0.13599879"
   ```

---

## Building a Debug APK

Run the following from **this directory** (`UI/Android`):

```bash
./gradlew assembleDebug
```

The first build also compiles the host-side Lagom tools (`Meta/ladybird.py install
--preset Host_Tools`) and downloads all vcpkg dependencies, so it may take 30–60
minutes. Subsequent builds are much faster thanks to ccache and Gradle's build cache.

The resulting APK is placed at:

```
UI/Android/build/outputs/apk/debug/app-debug.apk
```

### Useful Gradle tasks

| Task | Description |
|------|-------------|
| `./gradlew assembleDebug` | Build a debug APK |
| `./gradlew assembleRelease` | Build a release APK (requires signing config) |
| `./gradlew installDebug` | Build and install on connected device / emulator |
| `./gradlew connectedAndroidTest` | Run instrumented tests on device / emulator |
| `./gradlew lint` | Run Android Lint |
| `./gradlew clean` | Clean build outputs |

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LADYBIRD_CACHE_DIR` | `UI/Android/build/caches` | Root for ccache and vcpkg binary cache |
| `LADYBIRD_SOURCE_DIR` | auto-detected via `git` | Absolute path to the repository root |

---

## CI — GitHub Actions

The workflow at [`.github/workflows/android-debug.yml`](../../.github/workflows/android-debug.yml)
builds a Debug APK automatically on every push and pull request using an
`ubuntu-24.04` runner.

**What the workflow does:**

1. Checks out the repository.
2. Installs all host-build prerequisites (Clang, CMake, ninja, Rust, wasm-tools, vcpkg).
3. Sets up JDK 17 and configures the Gradle build cache.
4. Installs the required Android SDK components and NDK via `sdkmanager`.
5. Runs `./gradlew assembleDebug`.
6. Uploads `app-debug.apk` as a downloadable workflow artifact named **`app-debug`**.

ccache and the vcpkg binary cache are persisted between runs to speed up incremental
CI builds.

---

## Architecture

The Ladybird Android port consists of:

- **Kotlin activity layer** (`src/main/java/org/serenityos/ladybird/`) — the Android
  `Activity`, custom `WebView`, service wrappers for WebContent / RequestServer /
  ImageDecoder, and JNI glue.
- **Native shared library** (`src/main/cpp/` + `../../CMakeLists.txt`) — Ladybird's
  engine compiled as `libladybird.so` via the Android NDK.  The CMake build is driven
  by the root `CMakeLists.txt` with `VCPKG_TARGET_ANDROID=ON`.
- **vcpkg triplets** — set automatically by `UI/Android/vcpkg_android.cmake` based on
  `ANDROID_ABI` (`arm64-v8a` is enabled by default in `build.gradle.kts`).

---

## Known limitations / FIXMEs

- NDK version `29.0.13599879` is an r29 beta release.  Once r29 is stable it should
  be updated in `build.gradle.kts`.
- Resources are packaged as `ladybird-assets.zip` and extracted to app storage at
  runtime.  A proper `AssetManager`-backed implementation is planned.
- CA certificates for curl are manually merged from the system cert store as a
  workaround for curl's handling of the Android certificate storage.
