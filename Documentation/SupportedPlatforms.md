# Supported platforms

> [!IMPORTANT]
> This is proposed platform support for the alpha release and is open for discussion. We're still testing some of the
> setups listed here.

Last reviewed: 23 November 2026.

The alpha targets macOS and Linux. See [Platform compatibility](PlatformCompatibility.md) for detailed requirements.

## Support levels

- **Supported:** We publish downloads, test every release and fix problems.
- **Expected to work:** Our downloads should work, but we don't test these setups for releases. Bug reports are welcome.
- **Community:** No official download. Use community packages or [build from source](BuildInstructionsLadybird.md).

Keep your system up to date.

## macOS

| Mac | macOS | Support level |
| --- | --- | --- |
| Apple Silicon | 15, 26 and 27 | Supported |
| Apple Silicon | 14 (14.4 or later) | Expected to work |
| Intel | 14.4 to 26 | Community |

## Linux

These distributions are **Supported** on x86-64:

| Distribution | Releases |
| --- | --- |
| Ubuntu | 24.04 and 26.04 LTS |
| Fedora | 44 and 45 |
| Debian | 13 |
| Arch Linux | An up-to-date rolling installation |
| Linux Mint | 22.x |

Download the .deb on Ubuntu, the .rpm on Fedora, or the Flatpak on any distribution. Packages from the AUR and other
community sources are **Community**.

ARM64 is **Supported** on Ubuntu 26.04 and **Expected to work** on the other distributions that provide it.

Our downloads are **Expected to work** on other distributions, such as Bazzite, CachyOS and openSUSE.

LTS releases may be dropped before their end of life. Release notes announce each change.

### CPU

- **x86-64:** x86-64-v2 (SSE4.2 and POPCNT). For example, Intel Core from Nehalem (2008) and AMD from Bulldozer (2011)
  onward.
- **ARM64:** ARMv8.2-A. For example, Raspberry Pi 5. Raspberry Pi 4 can't run Ladybird.

See the [CPU details](PlatformCompatibility.md#cpu-requirements).

## Windows

In WSL2, the [Linux downloads](#linux) are **Expected to work** and the Linux requirements apply. To build from source,
see the [WSL2 build instructions](BuildInstructionsLadybird.md#wsl2).

The native Windows port is experimental and is not part of the alpha.

## Other platforms

Other systems, such as FreeBSD, OpenBSD and Haiku, are **Community**. See the
[build instructions](BuildInstructionsLadybird.md) and the [porting guide](Porting.md).

There is currently no Android port.

## Graphics

Ladybird works on Wayland and X11, including XWayland. Without a usable GPU, Ladybird falls back to CPU rendering, which
is slower.

## Reporting issues

See the [issue reporting guidelines](../ISSUES.md) and the
[platform details to include](PlatformCompatibility.md#reporting-platform-issues).
