# Android WebWorker Status

Last updated: 2026-05-24

## Goal

Fix Android worker startup end-to-end so `new Worker(...)` works without crashing helper services, including the Google search and CAPTCHA path.

## Current status

- `assembleRelease` completed successfully on 2026-05-24.
- Release APK was installed to device `59041JEBF13693` with `adb install --no-incremental -r`.
- Main app launch after install succeeded.
- Main app, `:WebContent`, `:ImageDecoder`, and `:WebWorker` were all observed running.
- `:RequestServer` still crashed during the worker path with `g_primary_connection == nullptr`.

## What is already done

- Added Android native `WebWorkerService` entry path and verified the worker process starts.
- Fixed the Android worker native include to use `LibCore/Socket.h`.
- Fixed worker connection construction to use `WebWorker::ConnectionFromClient::construct(...)`.
- Added ProGuard keep rules for JNI-bound worker/service methods so release builds do not strip them.
- Confirmed the original `fd >= 0` crash in `IPC::encode<IPC::File>` is no longer the active blocker.

## Root cause of the current blocker

The Android worker path was binding `RequestServerService` and `ImageDecoderService` again for each worker. On Android that does not create a fresh isolated helper process the same way the desktop process model does. Instead, the already-running service receives another bind and tries to create another primary IPC connection, which trips:

- `Services/RequestServer/ConnectionFromClient.cpp`
- `VERIFY(g_primary_connection == nullptr)`

The same architectural problem is expected for `ImageDecoder` as well.

## Direction of the fix

- Do not spawn new RequestServer/ImageDecoder primary service connections for workers.
- Reuse the existing RequestServer and ImageDecoder connections already owned by `WebContent`.
- For worker startup on Android, only create and bind a fresh WebWorker transport.
- Obtain RequestServer/ImageDecoder worker-side transports via `connect_new_client()` style IPC on the existing clients.

## Files touched in this workstream

- `UI/Android/src/main/cpp/WebWorkerService.cpp`
- `UI/Android/proguard-rules.pro`
- `Libraries/LibWebView/WebContentClient.cpp`
- `Services/WebContent/PageClient.cpp`
- `UI/Android/src/main/cpp/WebContentService.cpp`
- `UI/Android/src/main/cpp/WebContentService.h`
- `UI/Android/src/main/cpp/WebContentServiceJNI.cpp`
- `UI/Android/src/main/java/org/serenityos/ladybird/WebContentService.kt`
- `Libraries/LibWebView/Plugins/ImageCodecPlugin.h`

## Next steps

- Run a fresh focused retest of `https://www.google.com/search?q=test` on the newly installed APK.
- Verify that `:RequestServer` no longer crashes and that the worker gets valid RequestServer/ImageDecoder transports.
- Validate the full Google/CAPTCHA path on device.

## Useful commands

Build:

```sh
cd UI/Android
JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 \
ANDROID_HOME=/media/junkers/DataSSD/android-dev/sdk \
./gradlew assembleRelease
```

Install:

```sh
/media/junkers/DataSSD/android-dev/sdk/platform-tools/adb install --no-incremental -r \
  /media/junkers/DataSSD/ladybird-android/UI/Android/build/outputs/apk/release/Ladybird-release.apk
```

Smoke test:

```sh
ADB=/media/junkers/DataSSD/android-dev/sdk/platform-tools/adb
$ADB logcat -c
$ADB shell am start -a android.intent.action.VIEW -d 'https://www.google.com/search?q=test' \
  -n org.serenityos.ladybird/.LadybirdActivity
$ADB shell ps -A | grep ladybird
$ADB logcat -d -t 400 | grep -E 'Ladybird|ladybird|WebWorker|FATAL|verification_failed|SIGTRAP|tombstone'
```