#!/usr/bin/env just --justfile
# Ladybird browser build automation
# 60-column limit enforced throughout

# Default recipe when just is called without arguments
default:
    @just --list

# Set common variables
build_preset := env_var_or_default("BUILD_PRESET", "default")
makejobs := env_var_or_default("MAKEJOBS", "0")

# Cross-platform helpers
uname := `uname -s`

# Determine OS based on uname output
os := if uname =~ "Darwin" { "macos" } else { "linux" }

# Detect if we're on macOS
is_macos := if os == "macos" { "true" } else { "false" }

# Ensures the toolchain is ready
toolchain:
    @echo "Ensuring toolchain is ready..."
    cd Toolchain && python3 ./BuildVcpkg.py

# Install dependencies based on OS
setup-deps:
    @echo "Installing dependencies for {{os}}..."
    just setup-deps-{{os}}

# Install dependencies for macOS
[unix]
setup-deps-macos:
    @echo "Installing macOS dependencies..."
    brew install autoconf autoconf-archive automake ccache \
        cmake nasm ninja pkg-config
    brew install llvm@19
    brew install qt

# Install dependencies for Debian/Ubuntu
[unix]
setup-deps-linux-debian:
    @echo "Installing Debian/Ubuntu dependencies..."
    sudo apt update
    sudo apt install -y autoconf autoconf-archive automake \
        build-essential ccache cmake curl fonts-liberation2 \
        git libgl1-mesa-dev nasm ninja-build pkg-config \
        qt6-base-dev qt6-tools-dev-tools qt6-wayland

# Install dependencies for Arch Linux
[unix]
setup-deps-linux-arch:
    @echo "Installing Arch Linux dependencies..."
    sudo pacman -S --needed autoconf-archive automake \
        base-devel ccache cmake curl libgl nasm ninja \
        qt6-base qt6-multimedia qt6-tools qt6-wayland \
        ttf-liberation

# Install dependencies for Fedora
[unix]
setup-deps-linux-fedora:
    @echo "Installing Fedora dependencies..."
    sudo dnf install -y autoconf-archive automake ccache \
        cmake curl liberation-sans-fonts libglvnd-devel \
        nasm ninja-build perl-FindBin perl-IPC-Cmd perl-lib \
        qt6-qtbase-devel qt6-qtmultimedia-devel \
        qt6-qttools-devel qt6-qtwayland-devel

# Configure the build with the specified preset
configure preset=build_preset:
    @echo "Configuring with preset: {{preset}}..."
    cmake --preset {{preset}} -S . -B Build/{{preset}}
    @echo "Configuration complete!"

# Build the project
build preset=build_preset target="":
    #!/usr/bin/env bash
    echo "Building with preset: {{preset}}..."
    
    # Get number of CPU cores for parallel build if MAKEJOBS=0
    if [ "{{makejobs}}" = "0" ]; then
        if command -v nproc > /dev/null; then
            # Linux
            JOBS=$(nproc)
        else
            # macOS
            JOBS=$(sysctl -n hw.ncpu)
        fi
    else
        JOBS={{makejobs}}
    fi
    
    BUILD_DIR="Build/{{preset}}"
    
    if [ -z "{{target}}" ]; then
        cmake --build $BUILD_DIR --parallel $JOBS
    else
        ninja -j $JOBS -C $BUILD_DIR -- {{target}}
    fi
    
    echo "Build complete!"

# Run Ladybird browser
run preset=build_preset *args:
    #!/usr/bin/env bash
    echo "Running Ladybird..."
    BUILD_DIR="Build/{{preset}}"
    
    # Ensure it's built first
    just build {{preset}} Ladybird
    
    if [ "$(uname -s)" = "Darwin" ]; then
        open -W --stdout $(tty) --stderr $(tty) \
            $BUILD_DIR/bin/Ladybird.app --args {{args}}
    else
        $BUILD_DIR/bin/Ladybird {{args}}
    fi

# Run with debugger
debug preset=build_preset *args:
    #!/usr/bin/env bash
    echo "Debugging Ladybird..."
    BUILD_DIR="Build/{{preset}}"
    
    # Ensure it's built first
    just build {{preset}} Ladybird
    
    # Determine which debugger to use
    if command -v gdb > /dev/null 2>&1; then
        DEBUGGER=gdb
    elif command -v lldb > /dev/null 2>&1; then
        DEBUGGER=lldb
    else
        echo "Please install gdb or lldb!"
        exit 1
    fi
    
    # Run with debugger
    if [ "$(uname -s)" = "Darwin" ]; then
        $DEBUGGER $BUILD_DIR/bin/Ladybird.app/Contents/MacOS/Ladybird {{args}}
    else
        $DEBUGGER $BUILD_DIR/bin/Ladybird {{args}}
    fi

# Clean the build directory
clean preset=build_preset:
    @echo "Cleaning build directory for preset: {{preset}}..."
    rm -rf Build/{{preset}}
    @echo "Clean complete!"

# Rebuild from scratch
rebuild preset=build_preset:
    @just clean {{preset}}
    @just configure {{preset}}
    @just build {{preset}}
    @echo "Rebuild complete!"

# Run the tests
test preset=build_preset test_name="":
    #!/usr/bin/env bash
    echo "Running tests..."
    BUILD_DIR="Build/{{preset}}"
    
    # Ensure it's built first
    just build {{preset}}
    
    CTEST_ARGS=("--preset" "{{preset}}" "--output-on-failure" "--test-dir" "$BUILD_DIR")
    if [ -n "{{test_name}}" ]; then
        if [ "{{test_name}}" = "WPT" ]; then
            CTEST_ARGS+=("-C" "Integration")
        fi
        CTEST_ARGS+=("-R" "{{test_name}}")
    fi
    ctest "${CTEST_ARGS[@]}"

# Install the built application
install preset=build_preset:
    @echo "Installing Ladybird..."
    cmake --build Build/{{preset}} --target install
    @echo "Installation complete!"

# Build with debug preset
build-debug *args:
    @BUILD_PRESET=Debug just build Debug {{args}}

# Run with debug preset
run-debug *args:
    @BUILD_PRESET=Debug just run Debug {{args}}

# Enable Qt chrome
enable-qt:
    @echo "Enabling Qt chrome..."
    cmake --preset {{build_preset}} -DENABLE_QT=ON
    @echo "Qt chrome enabled. Please rebuild."

# Disable Qt chrome
disable-qt:
    @echo "Disabling Qt chrome..."
    cmake --preset {{build_preset}} -DENABLE_QT=OFF
    @echo "Qt chrome disabled. Please rebuild."

# Show build status and information
status preset=build_preset:
    @echo "Build status for preset: {{preset}}"
    @echo "Build directory: Build/{{preset}}"
    @if [ -d "Build/{{preset}}" ]; then \
        echo "Build exists: Yes"; \
        if [ -f "Build/{{preset}}/build.ninja" ]; then \
            echo "  Build configured: Yes"; \
        else \
            echo "  Build configured: No"; \
        fi; \
        if [ -f "Build/{{preset}}/bin/Ladybird" ] || \
           [ -d "Build/{{preset}}/bin/Ladybird.app" ]; then \
            echo "  Browser built: Yes"; \
        else \
            echo "  Browser built: No"; \
        fi; \
    else \
        echo "Build exists: No"; \
    fi

# Helper for macOS to build with homebrew clang
[unix]
build-with-homebrew-clang preset=build_preset:
    #!/usr/bin/env bash
    if [ "$(uname -s)" != "Darwin" ]; then
        echo "This command is only for macOS!"
        exit 1
    fi
    
    BREW_PREFIX=$(brew --prefix llvm)
    CC="$BREW_PREFIX/bin/clang" \
    CXX="$BREW_PREFIX/bin/clang++" \
    just build {{preset}}

# Use the original ladybird.sh script (fallback)
script *args:
    @./Meta/ladybird.sh {{args}} 