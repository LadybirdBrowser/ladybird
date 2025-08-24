# Visual Studio Code Dev Container

Visual Studio Code has support for containerized development environments, the Ladybird code base has such a Dev Container included to provided a consistent and easily reproducible development environment.

Using the Dev Container requires the installation of the [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extension.

After installation the extension will detect the Ladybird Dev Container and provide the option to build it. Start the container build and wait for it to complete, the building process takes quite some time, view the log to monitor progress.

The Dev Container already includes the recommended extensions and settings, as laid out in the [Visual Studio Code Project Configuration](VSCodeConfiguration.md), but it is still a good read for further details.

## Usage
Run `./Meta/ladybird.py run ladybird` at least once to kick off downloading and building vcpkg dependencies.

On execution, the Ladybird processes run in the container, and a browser window opens if a Wayland socket is found. If this is not the case then there is the fallback to connect to the container using VNC. This can be done from a browser by accessing http://localhost:6080, or from a VNC application by accessing `localhost:5901`. Apps to use are [noVNC](https://novnc.com) in the former case, and [TigerVNC](https://tigervnc.org) in the latter case, or any alternative already available on the system.

## Debugging with LLDB
The Dev Container includes the [CodeLLDB](https://marketplace.visualstudio.com/items?itemName=vadimcn.vscode-lldb) extension for debugging. Refer to the generic [VSCode Debugging](VSCodeConfiguration.md#debugging) section for setting up the LLDB lauch configuration.
