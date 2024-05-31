# SerenityOS Documentation

Serenity development moves quickly, so some of these might be out of date. Please let us know if something here is wrong, or submit a PR with any additions or corrections! If you have any questions that are not answered here or in the [FAQ](FAQ.md), you are welcome to ask on [Slack](../README.md#get-in-touch-and-participate).

A list of useful pages across the web can be found on [the link list](Links.md).

## Building and Running
* [Build Instructions](BuildInstructions.md)
* [Advanced Build Instructions](AdvancedBuildInstructions.md)
* [Troubleshooting](Troubleshooting.md)
* [Running in VirtualBox](VirtualBox.md)
* [Running in VMware](VMware.md)
* [Running Tests](RunningTests.md)
* [Setting Up Self-Hosted Runners](SelfHostedRunners.md)
* [Profiling the Build](BuildProfilingInstructions.md)
* [Spice Integration](SpiceIntegration.md)

### OS-specific
Make sure to read the basic [Build Instructions](BuildInstructions.md) first.
* [Building on Windows](BuildInstructionsWindows.md)
* [Building on macOS](BuildInstructionsMacOS.md)
* [Building on Linux](BuildInstructionsOther.md)

### Running on Hardware
* [Bare Metal Installation](BareMetalInstallation.md)
* [Running On Raspberry Pi](RunningOnRaspberryPi.md)
* [Known Hardware Compatibility](HardwareCompatibility.md)

## Configuring Editors
* [CLion](CLionConfiguration.md)
* [Emacs](EmacsConfiguration.md)
* [Helix](HelixConfiguration.md)
* [NVim](NvimConfiguration.md)
* [Qt Creator](QtCreatorConfiguration.md)
* [Vim](VimConfiguration.md)
* [VS Code](VSCodeConfiguration.md)

## Development
* [How to Contribute](../CONTRIBUTING.md)
* [Coding Style](CodingStyle.md)
* [Common Patterns](Patterns.md)
* [Guidelines for Text in UI](HumanInterfaceGuidelines/Text.md)
* [Guidelines for writing manual pages](WritingManPages.md)
* [EventLoop](EventLoop.md)
* [High DPI Support](HighDPI.md)
* [Smart Pointers](SmartPointers.md)
* [String Formatting](StringFormatting.md)
* [How to Transfer Files Out of Serenity](TransferringFiles.md)

### File and Data Formats
* [Application Files (.af)](../Base/usr/share/man/man5/af.md)
* [Bitmap Fonts (.font)](../Base/usr/share/man/man5/font.md)
* [Clipboard data](../Base/usr/share/man/man5/clipboard.md)
* [Drag-and-drop data](../Base/usr/share/man/man5/drag-and-drop.md)
* [GUI Markup Language (.gml)](../Base/usr/share/man/man5/GML.md)
* [HackStudio Post-Create Scripts (.postcreate)](../Base/usr/share/man/man5/postcreate.md)
* [Inter-Process Communication protocol (.ipc)](../Base/usr/share/man/man4/ipc.md)

## Browser/LibWeb
* [Ladybird Browser Build Instructions](BuildInstructionsLadybird.md)
* [General Architecture](Browser/ProcessArchitecture.md)
* [LibWeb: From Loading to Painting](Browser/LibWebFromLoadingToPainting.md)
* [How to Add an IDL File](Browser/AddNewIDLFile.md)
* [LibWeb Code Style & Patterns](Browser/Patterns.md)

## Kernel
* [AHCI Locking](Kernel/AHCILocking.md)
* [ProcFS Indexing](Kernel/ProcFSIndexing.md)
* [RAMFS](Kernel/RAMFS.md)
* [IOWindow](Kernel/IOWindow.md)
* [Graphics Subsystem](Kernel/GraphicsSubsystem.md)
* [Kernel Development Patterns & Guidelines](Kernel/DevelopmentGuidelines.md)

## Applications
Documentation for SerenityOS applications and utilities can be found in [the man pages](https://man.serenityos.org/).
