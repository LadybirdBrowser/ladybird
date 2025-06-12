> [!NOTE]
> The Android port is currently [broken](https://github.com/LadybirdBrowser/ladybird/issues/2774) and won't build.

## Android Studio Project Configuration

The Android port of Ladybird has straightforward integration with the Android Studio IDE.

## Prerequisites

Ensure that your system has the following tools available:

- Android Studio Jellyfish 2023.3.1 or later
- CMake 3.23 or higher as the default CMake executable
- 20G or more storage space for SDKs + Emulator images + Gradle dependencies + build artifacts

## Opening the project

After opening the ``ladybird`` directory in Android Studio (NOT the UI/Android directory!)
there should be a pop-up in the bottom left indicating that an Android Gradle project was detected
in ``UI/Android``.

In the top left of the screen in the Project view, navigate to ``UI/Android``. Or, click the
highlighted text in the notification for that path. Open the ``settings.gradle.kts`` file. At the
top of the file should be a banner that says ``Code Insight unavailable (related Gradle project not
linked).`` Click the ``Link Gradle project`` text on the right side of the banner. After the IDE
loads the Gradle view to the right of the code window, go back to the banner at the top of the
``settings.gradle.kts`` file and click ``Load Script Configurations`` to finish loading the Gradle
project.

Gradle will index the project, and download all the required plugins. If it complains about no NDK,
follow the instructions in Android Studio to install an appropriate NDK version. If it still
complains about the NDK version, open ``File->Invalidate Caches...`` and click  ``Invalidate and
Restart``.

## Getting the most out of the IDE

See the sections in the [CLionConfiguration](CLionConfiguration.md) for [Excluding Build Artifacts](CLionConfiguration.md#excluding-build-artifacts),
and [Code Generation Settings](CLionConfiguration.md#code-generation-settings).
