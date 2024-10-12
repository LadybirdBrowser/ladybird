# SerenityOS self-hosted runner setup instructions

## Requirements

Since these self hosted-runners are supposed to be a more performant alternative to the GitHub-provided runners, the bare minimum requirements are GitHub's  own Linux runner [hardware specification](https://docs.github.com/en/actions/using-github-hosted-runners/about-github-hosted-runners#supported-runners-and-hardware-resources) as well as guaranteed uptime.

As for recommended requirements, listed below are the specifications of the current SerenityOS runners, roughly matching these would eventually make running performance-regression related tests on these easier. (But this is not a hard requirement, as GitHub offers the ability to selectively choose which self-hosted runners run which workflow)

#### IdanHo runner:
 - Ryzen 5 3600 - 12 cores w/ KVM support
 - 64GB of RAM
 - 512GB of SSD space
###### This runner can be split into 2 runners with half the cores/RAM/space if needed. 

## Setup

These instructions assume the OS installed is Ubuntu 22.04 (Jammy), so they might not be compatible with other Linux flavours.

### Install base dependencies
```shell
sudo add-apt-repository ppa:canonical-server/server-backports
wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
sudo add-apt-repository 'deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-16 main'
apt update
apt install git build-essential make cmake clang-format-18 gcc-13 g++-13 libstdc++-13-dev libgmp-dev ccache libmpfr-dev libmpc-dev ninja-build e2fsprogs qemu-utils qemu-system-i386 wabt
```
### Force usage of GCC 13
```shell
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 --slave /usr/bin/g++ g++ /usr/bin/g++-13
```
### Create a new user account named 'runner'
```shell
adduser runner
```
### Give it password-less sudo capabilities by adding the following line to /etc/sudoers:
```shell
runner ALL=(ALL) NOPASSWD:ALL
```
### Add it to the kvm access group (if available):
```shell
adduser runner kvm
```
### Switch to the new user and then create a workspace folder in its home directory:
```shell
mkdir actions-runner && cd actions-runner
```
### Download the latest version of actions-runner-linux-x64 from https://github.com/rust-lang/gha-runner/releases
```shell
curl -o actions-runner-linux-x64-X.X.X.tar.gz -L https://github.com/rust-lang/gha-runner/releases/download/vX.X.X-rust1/actions-runner-linux-x64-X.X.X-rust1.tar.gz
```
### Extract the tar archive
```shell
tar xzf ./actions-runner-linux-x64-X.X.X.tar.gz
```
### Link the runner to the repository
```shell
./config.sh --url https://github.com/LadybirdBrowser/ladybird --token INSERT_SECRET_TOKEN_HERE
```
### Configure the runner to protect against malicious PRs by adding the following line to .env:
```shell
RUST_WHITELISTED_EVENT_NAME=push
```
### Configure the maximum runner threads by adding the following line to .env:
```shell
MAX_RUNNER_THREADS=XXX
```
If you are setting up multiple runners on the same machine, this setting can be used to divvy up the cores, if you're only setting up one runner, this can just be set to the server's core count
### Install the runner as a service
```shell
sudo ./svc.sh install
```
### Start the runner
```shell
sudo ./svc.sh start
```
