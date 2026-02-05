# shellcheck shell=bash
# shellcheck disable=SC2034
# SC2034: "Variable appears unused. Verify it or export it."
#         Those are intentional here, as the file is meant to be included elsewhere.

# NOTE: If using another privilege escalation binary make sure it is configured or has the appropriate flag
#       to keep the current environment variables in the launched process (in sudo's case this is achieved
#       through the -E flag described in sudo(8).
die() {
    echo "die: $*"
    exit 1
}

exit_if_running_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
       die "$*"
    fi
}

get_number_of_processing_units() {
  number_of_processing_units="nproc"
  SYSTEM_NAME="$(uname -s)"

  if [ "$SYSTEM_NAME" = "OpenBSD" ]; then
      number_of_processing_units="sysctl -n hw.ncpuonline"
  elif [ "$SYSTEM_NAME" = "FreeBSD" ]; then
      number_of_processing_units="sysctl -n hw.ncpu"
  elif [ "$SYSTEM_NAME" = "Darwin" ]; then
      number_of_processing_units="sysctl -n hw.ncpu"
  fi

  ($number_of_processing_units)
}

get_top_dir() {
    git rev-parse --show-toplevel
}

ensure_ladybird_source_dir() {
    if [ -z "$LADYBIRD_SOURCE_DIR" ] || [ ! -d "$LADYBIRD_SOURCE_DIR" ]; then
        LADYBIRD_SOURCE_DIR="$(get_top_dir)"
        export LADYBIRD_SOURCE_DIR
    fi
}

get_build_dir() {
    ensure_ladybird_source_dir

    # Note: Keep in sync with buildDir defaults in CMakePresets.json
    case "$1" in
        "Release")
            BUILD_DIR="${LADYBIRD_SOURCE_DIR}/Build/release"
            ;;
        "Debug")
            BUILD_DIR="${LADYBIRD_SOURCE_DIR}/Build/debug"
            ;;
        "Sanitizer")
            BUILD_DIR="${LADYBIRD_SOURCE_DIR}/Build/sanitizers"
            ;;
        "Distribution")
            BUILD_DIR="${LADYBIRD_SOURCE_DIR}/Build/distribution"
            ;;
        *)
            echo "Unknown BUILD_PRESET: '$1'" >&2
            exit 1
            ;;
    esac

    echo "${BUILD_DIR}"
}

absolutize_path() {
    directory="$(eval echo "$(dirname "$1")")"
    if [ -d "$directory" ]; then
        resolved_directory="$(cd "$directory" && pwd)"
        echo "${resolved_directory%/}/$(basename "$1")"
    else
        echo "No such directory: '$directory'" >&2
        return 1
    fi
}
