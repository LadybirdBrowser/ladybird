# Windows Build Environment Setup Guide for Ladybird

**System**: Windows 10/11  
**Target**: Complete Ladybird browser build environment  
**Duration**: 2-3 hours total  
**Disk Space Required**: ~30 GB

---

## Current System Status

**Detected**:
- ✅ Windows 10/11 (Build 26200)
- ✅ Git Bash / MinGW64 installed
- ⚠️ Visual Studio 2022 folder exists but **no C++ tools installed**
- ❌ CMake not found
- ❌ C++ compiler not found
- ❌ Qt6 not found
- ❌ vcpkg not found

**Required for Ladybird**:
- Visual Studio 2022 with C++ development tools
- CMake 3.25+
- Qt6 (via vcpkg)
- Ninja build system
- Various C++ libraries (handled by vcpkg)

---

## Step-by-Step Installation

### Step 1: Install Visual Studio 2022 (45 minutes)

**Download**:
1. Go to: https://visualstudio.microsoft.com/vs/community/
2. Click "Download Visual Studio 2022 Community" (FREE)
3. Run the installer: `vs_community.exe`

**Installation**:
1. When installer launches, select **"Desktop development with C++"** workload
2. In "Individual Components" tab, ensure these are checked:
   - ✅ MSVC v143 - VS 2022 C++ x64/x86 build tools (latest)
   - ✅ Windows 11 SDK (10.0.22621.0 or latest)
   - ✅ C++ CMake tools for Windows
   - ✅ C++ Clang Compiler for Windows (optional but recommended)
   - ✅ C++ ATL for latest v143 build tools
   - ✅ C++ MFC for latest v143 build tools
3. Click "Install" (downloads ~7-10 GB, installs ~15-20 GB)
4. Wait for installation to complete (30-45 minutes)
5. **Restart computer** after installation

**Verification**:
```powershell
# Open "Developer PowerShell for VS 2022" from Start Menu
cl.exe
# Should show: Microsoft (R) C/C++ Optimizing Compiler Version 19.xx

cmake --version
# Should show: cmake version 3.xx
```

---

### Step 2: Install Chocolatey Package Manager (5 minutes)

**Purpose**: Simplifies installation of remaining tools.

**Installation** (Run PowerShell as Administrator):
```powershell
# Check if already installed
choco --version

# If not installed, run:
Set-ExecutionPolicy Bypass -Scope Process -Force
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
```

**Verification**:
```powershell
choco --version
# Should show: 2.x.x
```

---

### Step 3: Install Additional Build Tools (10 minutes)

**Using Chocolatey** (PowerShell as Administrator):
```powershell
# Install CMake (if not included with VS)
choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System' -y

# Install Ninja build system
choco install ninja -y

# Install Git (if not already installed)
choco install git -y

# Install Python 3 (required for build scripts)
choco install python -y

# Refresh environment variables
refreshenv
```

**Verification**:
```powershell
cmake --version   # Should show 3.25+
ninja --version   # Should show 1.11+
git --version     # Should show 2.x+
python --version  # Should show 3.x+
```

---

### Step 4: Install vcpkg for Dependencies (15 minutes)

**vcpkg** is Microsoft's C++ package manager - used for Qt6 and other libraries.

**Installation**:
```powershell
# Navigate to a permanent location (NOT in Ladybird repo)
cd C:\Development

# Clone vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg

# Bootstrap vcpkg
.\bootstrap-vcpkg.bat

# Add to PATH permanently
[Environment]::SetEnvironmentVariable("Path", $env:Path + ";C:\Development\vcpkg", "Machine")
refreshenv
```

**Verification**:
```powershell
vcpkg version
# Should show: vcpkg package management program version...
```

---

### Step 5: Install Qt6 via vcpkg (60-90 minutes)

**Warning**: This is the longest step - Qt6 is large (~10 GB compiled).

**Installation**:
```powershell
cd C:\Development\vcpkg

# Install Qt6 base libraries (required for Ladybird UI)
.\vcpkg install qt6-base:x64-windows
.\vcpkg install qt6-tools:x64-windows
.\vcpkg install qt6-svg:x64-windows
.\vcpkg install qt6-multimedia:x64-windows

# Integrate with Visual Studio
.\vcpkg integrate install
```

**Expected Output**:
```
Computing installation plan...
The following packages will be built and installed:
    qt6-base[core]:x64-windows
    qt6-tools[core]:x64-windows
    ...
Building qt6-base[core]:x64-windows... (this may take 30-60 minutes)
```

**Verification**:
```powershell
.\vcpkg list
# Should show:
# qt6-base:x64-windows
# qt6-tools:x64-windows
# qt6-svg:x64-windows
# qt6-multimedia:x64-windows
```

---

### Step 6: Configure Ladybird for Windows Build (10 minutes)

**Navigate to Ladybird**:
```powershell
cd C:\Development\Projects\ladybird\ladybird
```

**Configure CMake with vcpkg**:
```powershell
# Create build directory
mkdir Build
cd Build

# Configure with vcpkg toolchain
cmake .. -G "Ninja" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:\Development\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DCMAKE_CXX_COMPILER=cl.exe `
  -DCMAKE_C_COMPILER=cl.exe
```

**Expected Output**:
```
-- The CXX compiler identification is MSVC 19.xx
-- Detecting CXX compiler ABI info - done
-- Found Qt6 (version 6.x.x)
-- Configuring done
-- Generating done
-- Build files have been written to: C:/Development/Projects/ladybird/ladybird/Build
```

**If Errors Occur**:
- **"Qt6 not found"**: Ensure vcpkg integrate install was run
- **"Compiler not found"**: Run from "Developer PowerShell for VS 2022"
- **"CMake version too old"**: Update CMake via choco upgrade cmake

---

### Step 7: Build Ladybird (30-60 minutes)

**Build Everything**:
```powershell
# From C:\Development\Projects\ladybird\ladybird\Build
ninja
```

**Or Build Specific Targets**:
```powershell
# Build just the IPC compiler
ninja IPCCompiler

# Build tests
ninja TestIPCCompiler
ninja TestValidation

# Build Ladybird browser
ninja Ladybird
```

**Expected Output**:
```
[1/2847] Building CXX object Libraries/LibCore/CMakeFiles/LibCore.dir/File.cpp.obj
[2/2847] Building CXX object Libraries/LibCore/CMakeFiles/LibCore.dir/Directory.cpp.obj
...
[2847/2847] Linking CXX executable bin\Ladybird.exe
Build succeeded!
```

**Build Times** (varies by CPU):
- IPC Compiler only: 5-10 minutes
- All Libraries: 20-30 minutes
- Full Ladybird: 45-90 minutes

---

### Step 8: Run Tests (2 minutes)

**Run IPC Compiler Tests**:
```powershell
cd C:\Development\Projects\ladybird\ladybird\Build

# Run unit tests
.\Tests\LibIPC\TestIPCCompiler.exe
.\Tests\LibIPC\TestValidation.exe

# Run all tests
ctest -C Release
```

**Expected Output**:
```
Running TestIPCCompiler:
✅ verify_max_length_attribute_syntax PASSED
✅ verify_max_size_attribute_syntax PASSED
...
Total: 25 tests, 25 passed, 0 failed

Running TestValidation:
✅ reject_oversized_string PASSED
...
Total: 30 tests, 30 passed, 0 failed
```

---

### Step 9: Test IPC Compiler with Sample File (2 minutes)

**Generate Code from Sample .ipc**:
```powershell
cd C:\Development\Projects\ladybird\ladybird

# Run IPC compiler on sample file
.\Build\bin\IPCCompiler.exe `
  claudedocs\security-hardening\Sample-Enhanced-RequestServer.ipc `
  > sample-generated.h

# Inspect generated validation code
Select-String -Path sample-generated.h -Pattern "exceeds maximum length"
Select-String -Path sample-generated.h -Pattern "disallowed URL scheme"
Select-String -Path sample-generated.h -Pattern "contains CRLF"
```

**Expected Output**:
```
sample-generated.h:234:        return Error::from_string_literal("Decoded method exceeds maximum length");
sample-generated.h:267:        return Error::from_string_literal("Decoded url has disallowed URL scheme");
sample-generated.h:289:        return Error::from_string_literal("Decoded method contains CRLF characters");
```

---

## Troubleshooting

### Issue: "Qt6 not found"

**Solution**:
```powershell
# Verify Qt6 installation
C:\Development\vcpkg\vcpkg list | Select-String qt6

# Reinstall if missing
C:\Development\vcpkg\vcpkg install qt6-base:x64-windows --recurse

# Re-integrate
C:\Development\vcpkg\vcpkg integrate install
```

### Issue: "Compiler not recognized"

**Solution**:
- **MUST** use "Developer PowerShell for VS 2022" (not regular PowerShell)
- Or manually initialize VS environment:
```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
```

### Issue: "Ninja not found"

**Solution**:
```powershell
choco install ninja -y
refreshenv
```

### Issue: Build fails with "Out of memory"

**Solution**:
- Reduce parallel jobs: `ninja -j 4` (instead of default)
- Close other applications
- Ensure at least 16 GB RAM available

### Issue: vcpkg packages fail to install

**Solution**:
```powershell
# Update vcpkg
cd C:\Development\vcpkg
git pull
.\bootstrap-vcpkg.bat

# Clear cache and retry
.\vcpkg remove --outdated --recurse
.\vcpkg install qt6-base:x64-windows --recurse
```

---

## Quick Reference Commands

### Open Development Environment
```powershell
# Start Menu → "Developer PowerShell for VS 2022"
```

### Clean Build
```powershell
cd C:\Development\Projects\ladybird\ladybird
Remove-Item -Recurse -Force Build
mkdir Build
cd Build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:\Development\vcpkg\scripts\buildsystems\vcpkg.cmake
ninja
```

### Rebuild IPC Compiler Only
```powershell
cd C:\Development\Projects\ladybird\ladybird\Build
ninja IPCCompiler
```

### Run All Tests
```powershell
cd C:\Development\Projects\ladybird\ladybird\Build
ctest -C Release --output-on-failure
```

---

## Estimated Disk Usage

| Component | Disk Space |
|-----------|------------|
| Visual Studio 2022 | ~15-20 GB |
| vcpkg | ~5 GB |
| Qt6 (via vcpkg) | ~10 GB |
| Ladybird build artifacts | ~5-8 GB |
| **Total** | **~35-43 GB** |

---

## Build Time Summary

| Task | Duration |
|------|----------|
| Install Visual Studio | 45 min |
| Install Chocolatey + tools | 15 min |
| Install vcpkg | 15 min |
| Install Qt6 | 60-90 min |
| Configure Ladybird | 5-10 min |
| Build Ladybird (first time) | 45-90 min |
| Run tests | 2 min |
| **Total** | **~3-4 hours** |

---

## Next Steps After Setup

Once build completes successfully:

1. **Run Tests**:
   ```powershell
   .\Build\Tests\LibIPC\TestIPCCompiler.exe
   .\Build\Tests\LibIPC\TestValidation.exe
   ```

2. **Test IPC Compiler**:
   ```powershell
   .\Build\bin\IPCCompiler.exe claudedocs\security-hardening\Sample-Enhanced-RequestServer.ipc
   ```

3. **Complete Week 4 Phase 2**:
   - Verify all 25 parser tests pass
   - Verify all 30 integration tests pass
   - Document actual results

4. **Proceed to Phase 3** (Migration):
   - Apply validation attributes to real .ipc files
   - Remove manual validation code
   - Verify security equivalence

---

## Alternative: WSL2 (Faster Setup)

If Windows build proves too complex, consider WSL2:

**Advantages**:
- Faster setup (~30 minutes vs. 3 hours)
- Native Linux environment
- Official Ladybird target platform

**Setup**:
```powershell
# Install WSL2
wsl --install -d Ubuntu-24.04

# Inside WSL:
sudo apt update
sudo apt install build-essential cmake ninja-build qt6-base-dev git python3

# Clone and build
git clone https://github.com/LadybirdBrowser/ladybird.git
cd ladybird
cmake -B Build -G Ninja
cmake --build Build
```

---

**Document Version**: 1.0  
**Last Updated**: 2025-10-23  
**Tested On**: Windows 11 Build 26200  
**Author**: Security Hardening Team
