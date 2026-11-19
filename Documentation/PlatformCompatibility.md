# Platform compatibility

Details for [Supported platforms](SupportedPlatforms.md), which defines **Supported**, **Expected to work** and
**Community**.

## CPU requirements

These requirements apply to source builds too. The JavaScript and WebAssembly engines need these CPU features whatever
CPU the build targets.

### x86-64

Ladybird requires x86-64-v2. SSE4.2 and POPCNT are required. AVX and AVX2 are not.

To check a Linux machine, run:

```
/lib64/ld-linux-x86-64.so.2 --help | grep x86-64-v2
```

The line should say `supported`.

### ARM64

Ladybird requires ARMv8.2-A. Raspberry Pi 5 (Cortex-A76) meets this. Raspberry Pi 4 (Cortex-A72, ARMv8.0) does not.

## Linux packages

- **Flatpak:** Any distribution. Ships its own runtime and libraries, including Qt.
- **.deb:** Ubuntu 24.04 and 26.04.
- **.rpm:** Fedora 44 and 45.

The .deb and .rpm recommend the system's libavcodec, which Ladybird needs to play H.264 and AAC. On Fedora,
libavcodec-free can't play H.264 by itself.

Keep the Flatpak runtime and graphics extensions up to date. On NVIDIA, the Flatpak's NVIDIA extension must match the
host driver version. See the [Flatpak extension documentation](https://docs.flatpak.org/en/latest/extension.html).

## Desktop environments and window systems

| Configuration | Support level |
| --- | --- |
| GNOME with Mutter on Wayland | Supported |
| KDE Plasma with KWin on Wayland | Supported |
| Cinnamon on X11 | Supported |
| X11 backend under XWayland | Expected to work |
| GNOME on X11 | Expected to work |
| Xfce on X11 | Expected to work |
| Other compositors, such as Sway and Hyprland | Expected to work |

Supported desktops are tested on the versions the supported distributions ship. Testing covers window management,
scaling, clipboard, drag and drop, input methods, keyboard navigation, accessibility and desktop integration.

## Graphics and media

Use the graphics drivers from your OS or distribution and keep them up to date.

| Driver stack | Support level |
| --- | --- |
| Apple Silicon with the macOS Metal driver | Supported |
| AMD with Mesa Vulkan and OpenGL drivers | Supported |
| Intel with Mesa Vulkan and OpenGL drivers | Supported |
| NVIDIA with a maintained NVIDIA driver, including hybrid-GPU systems | Supported |
| Virtual GPUs that meet the Linux requirements below | Expected to work |
| Other Linux GPU drivers that meet the requirements below, such as NVK | Expected to work |

These support levels are for GPU acceleration. Without it, browsing uses CPU rendering, which is slower. Without
hardware video decoding, supported codecs are decoded in software.

### Linux

- **GPU painting:** Vulkan 1.1 with `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
  `VK_EXT_image_drm_format_modifier` and `VK_KHR_image_format_list`. `VK_EXT_queue_family_foreign` is used if
  available. Without these, Ladybird uses CPU painting.
- **WebGL:** ANGLE with its OpenGL/EGL backend.
- **Hardware video decoding:** Vulkan Video, where the GPU and driver can decode the codec.

### macOS

- **GPU painting:** Metal.
- **WebGL:** ANGLE with its Metal backend.
- **Hardware video decoding:** Supported.

## Virtual machines

An x86-64 guest needs the x86-64-v2 CPU features. QEMU's default `qemu64` CPU model lacks them. Use `-cpu host` with
hardware virtualization, or a named CPU model that supports x86-64-v2 when emulating. See the
[QEMU CPU model guide](https://www.qemu.org/docs/master/system/i386/cpu.html).

GPU acceleration depends on the virtual GPU and guest drivers. Without it, browsing uses CPU rendering. WebGL and
hardware video decoding may not be available. See your VM software's documentation, for example the
[UTM Linux guest guide](https://docs.getutm.app/guest-support/linux/).

### WSL2

Our Linux downloads are **Expected to work** in WSL2. Graphics depend on the WSL graphics stack and Windows drivers.
To build from source, see the [WSL2 build instructions](BuildInstructionsLadybird.md#wsl2).

## Sandboxing

On macOS, Ladybird's sandbox uses Seatbelt.

On Linux, it uses seccomp, and Landlock where the kernel has it. Without Landlock, a broker process checks each file a
helper opens instead. Either way, the sandbox needs no setup.

With Landlock older than ABI 3 (before Linux 6.2), file truncation isn't restricted. See the
[Landlock documentation](https://docs.kernel.org/userspace-api/landlock.html).

Flatpak's sandbox is an extra layer. Ladybird's helpers keep their own sandbox inside it.

The native Windows port and other systems, such as FreeBSD, have no sandbox yet. See the
[process architecture](ProcessArchitecture.md#sandboxing) for what each process's sandbox allows.

## Reporting platform issues

See the [issue reporting guidelines](../ISSUES.md). For platform issues, include:

- Operating system and version
- CPU architecture
- Ladybird version (from About Ladybird) and package format
- Desktop environment, and Wayland or X11
- For rendering issues: GPU and driver version
- For VMs: virtualization software, backend and virtual display device
