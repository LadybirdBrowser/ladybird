import os
import subprocess
import sys
import shutil


def get_script_dir():
    return os.path.dirname(os.path.realpath(__file__))


def run_command(command):
    subprocess.run(command, shell=True, check=True)


def main():
    script_dir = get_script_dir()

    git_repo = "https://github.com/microsoft/vcpkg.git"
    git_rev = "2960d7d80e8d09c84ae8abf15c12196c2ca7d39a"  # 2024.09.30
    prefix_dir = os.path.join(script_dir, "Local", "vcpkg")

    os.makedirs(os.path.join(script_dir, "Tarballs"), exist_ok=True)
    os.chdir(os.path.join(script_dir, "Tarballs"))

    if not os.path.isdir("vcpkg"):
        run_command(f"git clone {git_repo}")
    else:
        bootstrapped_vcpkg_version = subprocess.check_output(
            "git -C vcpkg rev-parse HEAD", shell=True).strip().decode()

        if bootstrapped_vcpkg_version == git_rev:
            sys.exit(0)

    print("Building vcpkg")

    os.chdir("vcpkg")
    run_command("git fetch origin")
    run_command(f"git checkout {git_rev}")
    run_command("cd " + os.path.join(script_dir, "Tarballs", "vcpkg"))
    if os.name == 'nt':
        run_command("bootstrap-vcpkg.bat -disableMetrics")
    else:
        run_command("./bootstrap-vcpkg.sh -disableMetrics")

    os.makedirs(os.path.join(prefix_dir, "bin"), exist_ok=True)
    vcpkg_name = "vcpkg.exe" if os.name == 'nt' else "vcpkg"
    shutil.copy(vcpkg_name, os.path.join(prefix_dir, "bin", vcpkg_name))


if __name__ == "__main__":
    main()
