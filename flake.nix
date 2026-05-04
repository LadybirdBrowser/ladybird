{
  description = "Ladybird browser, packaged from this checkout";

  inputs = {
    nixpkgs.url = "nixpkgs";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs systems;

      pkgsFor =
        system:
        import nixpkgs {
          inherit system;
          config = {
            # nixpkgs still marks Ladybird broken on Darwin. This flake builds
            # the local checkout on macOS anyway.
            problems.handlers.ladybird.broken = "ignore";
          };
        };
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          lib = pkgs.lib;

          source = lib.cleanSourceWith {
            src = ./.;
            filter =
              path: type:
              let
                relativePath = lib.removePrefix "${toString ./.}/" (toString path);
              in
              !(lib.hasPrefix "Build/" relativePath)
              && relativePath != "flake.nix"
              && relativePath != "flake.lock"
              && relativePath != "result";
          };

          ladybird = pkgs.ladybird.overrideAttrs (final: previous: {
            version = "local";
            src = source;
            cargoDeps = pkgs.rustPlatform.importCargoLock {
              lockFile = ./Cargo.lock;
            };

            postPatch = (previous.postPatch or "") + ''
              substituteInPlace AK/Format.cpp \
                --replace-fail '#include <stdio.h>' '#include <stdio.h>
#include <stdlib.h>' \
                --replace-fail 'static bool is_debug_enabled = true;' 'static bool is_debug_enabled = [] {
    auto const* value = getenv("LADYBIRD_NIX_DEBUG");
    return value != nullptr && value[0] != 0 && !(value[0] == 48 && value[1] == 0);
}();'
            '' + lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
              substituteInPlace Meta/CMake/check_for_dependencies.cmake \
                --replace-fail 'else()
    pkg_check_modules(angle REQUIRED IMPORTED_TARGET angle)
    set(ANGLE_TARGETS PkgConfig::angle)
endif()' 'elseif(APPLE)
    add_library(ladybird_angle INTERFACE)
    target_include_directories(ladybird_angle INTERFACE ${pkgs.angle}/include)
    target_link_libraries(ladybird_angle INTERFACE ${pkgs.angle}/lib/libEGL.dylib ${pkgs.angle}/lib/libGLESv2.dylib)
    set(ANGLE_TARGETS ladybird_angle)
else()
    pkg_check_modules(angle REQUIRED IMPORTED_TARGET angle)
    set(ANGLE_TARGETS PkgConfig::angle)
endif()'

              substituteInPlace Libraries/LibCrypto/CMakeLists.txt \
                --replace-fail '    OpenSSL.cpp' '    OpenSSL.cpp
    TommathDouble.cpp'

              substituteInPlace Libraries/LibWeb/WebGL/OpenGLContext.cpp \
                --replace-fail '    eglWaitUntilWorkScheduledANGLE(m_impl->display);' '    using EGLWaitUntilWorkScheduledANGLE = void (*)(EGLDisplay);
    static auto wait_until_work_scheduled = reinterpret_cast<EGLWaitUntilWorkScheduledANGLE>(eglGetProcAddress("eglWaitUntilWorkScheduledANGLE"));
    if (wait_until_work_scheduled)
        wait_until_work_scheduled(m_impl->display);
    else
        glFlush();'

              perl -0pi -e 's/\[\[NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionBottomRight\s+inDirections:NSCursorFrameResizeDirectionsAll\] set\]/[[NSCursor arrowCursor] set]/g; s/\[\[NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionBottomLeft\s+inDirections:NSCursorFrameResizeDirectionsAll\] set\]/[[NSCursor arrowCursor] set]/g' UI/AppKit/Interface/LadybirdWebView.mm

              {
                echo '#include <math.h>'
                echo '#include <stdio.h>'
                echo '#include <tommath.h>'
                echo
                echo "extern \"C\" mp_err mp_set_double(mp_int* value, double number)"
                echo '{'
                echo '    if (!isfinite(number) || trunc(number) != number)'
                echo '        return MP_VAL;'
                echo
                echo '    char buffer[400];'
                echo '    int length = snprintf(buffer, sizeof(buffer), "%.0f", number);'
                echo '    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer))'
                echo '        return MP_VAL;'
                echo
                echo '    return mp_read_radix(value, buffer, 10);'
                echo '}'
              } > Libraries/LibCrypto/TommathDouble.cpp
            '';

            cmakeFlags = (previous.cmakeFlags or [ ]) ++ [
              (lib.cmakeBool "BUILD_TESTING" false)
              (lib.cmakeBool "ENABLE_LAGOM_CCACHE" false)
            ];

            cmakeBuildType = "Release";

            nativeBuildInputs = (previous.nativeBuildInputs or [ ]) ++ [
              pkgs.perl
            ];

            buildInputs = (previous.buildInputs or [ ]) ++ [
              pkgs.mimalloc
            ];

            env = (previous.env or { }) // {
              NIX_LDFLAGS = lib.optionalString pkgs.stdenv.hostPlatform.isLinux "-lGL -lfontconfig";
            };

            postInstall = (previous.postInstall or "") + lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
              while IFS= read -r macho; do
                otool -L "$macho" 2>/dev/null \
                  | awk '/^[[:space:]]+\.\/lib.*\.dylib / { print $1 }' \
                  | while IFS= read -r dep; do
                    dep_name="''${dep#./}"
                    if [ -e "${pkgs.angle}/lib/$dep_name" ]; then
                      install_name_tool -change "$dep" "${pkgs.angle}/lib/$dep_name" "$macho"
                    fi
                  done

                otool -L "$macho" 2>/dev/null \
                  | awk '/^[[:space:]]+@rpath\/lib.*\.dylib / { print $1 }' \
                  | while IFS= read -r dep; do
                    dep_name="''${dep#@rpath/}"
                    if [ -e "${pkgs.angle}/lib/$dep_name" ]; then
                      install_name_tool -change "$dep" "${pkgs.angle}/lib/$dep_name" "$macho"
                    fi
                  done
              done < <(find "$out/Applications/Ladybird.app/Contents" -type f)
            '';

            dontWrapQtApps = pkgs.stdenv.hostPlatform.isDarwin;

            meta = (previous.meta or { }) // {
              broken = false;
              mainProgram = "Ladybird";
            };
          });
        in
        {
          default = ladybird;
          inherit ladybird;
        }
      );

      apps = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          package = self.packages.${system}.ladybird;
          program =
            if nixpkgs.lib.hasSuffix "-darwin" system then
              "${package}/Applications/Ladybird.app/Contents/MacOS/Ladybird"
            else
              "${package}/bin/Ladybird";
          verboseProgram = pkgs.writeShellScript "ladybird-debug-output" ''
            export LADYBIRD_NIX_DEBUG=1
            exec "${program}" "$@"
          '';
        in
        {
          default = {
            type = "app";
            inherit program;
            meta.description = "Run Ladybird from this checkout";
          };

          ladybird = self.apps.${system}.default;

          verbose = {
            type = "app";
            program = "${verboseProgram}";
            meta.description = "Run Ladybird from this checkout with Ladybird debug output enabled";
          };
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
          package = self.packages.${system}.ladybird;
        in
        {
          default = pkgs.mkShell {
            name = "ladybird";
            inputsFrom = [ package ];
            packages = [
              pkgs.ccache
              pkgs.clang-tools
            ];

            shellHook = ''
              ladybird-nix-configure() {
                cmake -S . -B Build/nix -G Ninja \
                  -DCMAKE_BUILD_TYPE=Release \
                  -DBUILD_TESTING=OFF \
                  -DENABLE_LAGOM_CCACHE=ON \
                  -DENABLE_LTO_FOR_RELEASE=OFF \
                  -DENABLE_NETWORK_DOWNLOADS=OFF \
                  -DLADYBIRD_CACHE_DIR=Caches \
                  -DICU_ROOT=${pkgs.icu78.dev}
              }

              ladybird-nix-fixup-angle() {
                local app_dir="''${1:-$PWD/Build/nix/bin/Ladybird.app/Contents}"

                [ -d "$app_dir" ] || return 0

                while IFS= read -r macho; do
                  otool -L "$macho" 2>/dev/null \
                    | awk '/^[[:space:]]+\.\/lib.*\.dylib / { print $1 }' \
                    | while IFS= read -r dep; do
                      dep_name="''${dep#./}"
                      if [ -e "${pkgs.angle}/lib/$dep_name" ]; then
                        install_name_tool -change "$dep" "${pkgs.angle}/lib/$dep_name" "$macho"
                      fi
                    done

                  otool -L "$macho" 2>/dev/null \
                    | awk '/^[[:space:]]+@rpath\/lib.*\.dylib / { print $1 }' \
                    | while IFS= read -r dep; do
                      dep_name="''${dep#@rpath/}"
                      if [ -e "${pkgs.angle}/lib/$dep_name" ]; then
                        install_name_tool -change "$dep" "${pkgs.angle}/lib/$dep_name" "$macho"
                      fi
                    done
                done < <(find "$app_dir" -type f)
              }

              ladybird-nix-build() {
                cmake --build Build/nix --target Ladybird
                ladybird-nix-fixup-angle
              }

              ladybird-nix-run() {
                ladybird-nix-configure
                ladybird-nix-build
                if [ -n "''${LADYBIRD_NIX_DEBUG:-}" ]; then
                  "$PWD/Build/nix/bin/Ladybird.app/Contents/MacOS/Ladybird" "$@"
                else
                  "$PWD/Build/nix/bin/Ladybird.app/Contents/MacOS/Ladybird" "$@" 2>/dev/null
                fi
              }
            '';
          };
        }
      );
    };
}
