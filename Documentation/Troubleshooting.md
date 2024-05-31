# Troubleshooting

In case of an error, you might find an answer of how to deal it here.

## Building SerenityOS

### CMake fails to configure the build because it's outdated

Ensure your CMake version is >= 3.16 with `cmake --version`. If your system doesn't provide a suitable
version of CMake, you can download a binary release from the [CMake website](https://cmake.org/download).

### The toolchain is outdated

We strive to use the latest compilers and build tools to ensure the best developer experience; so every
few months, the toolchain needs to be updated. When such an update is due, an error like the following
will be printed during the build:

```
CMake Error at CMakeLists.txt:28 (message):
  GNU version (13.1.0) does not match expected compiler version (13.2.0).

  Please rebuild the GNU Toolchain
```

Or like this one:

```
Your toolchain has an old version of binutils installed.
    installed version: "GNU ld (GNU Binutils) 2.40"
    expected version:  "GNU ld (GNU Binutils) 2.41"
Please run Meta/serenity.sh rebuild-toolchain x86_64 to update it.
```

Run `Meta/serenity.sh rebuild-toolchain x86_64` to perform the update.

CMake might cache the compiler version in some cases and print an error even after the toolchain has been rebuilt.
If this happens, run `Meta/serenity.sh rebuild x86_64` to start over from a fresh build directory.

### GCC is missing or is outdated

Ensure your gcc version is >= 13 with `gcc --version`. Otherwise, install it. If your gcc binary is not
called `gcc` you have to specify the names of your C and C++ compiler when you run cmake, e.g.
`cmake ../.. -GNinja -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13`.

### Legacy renegotiation is disabled

Ensure your `/etc/ssl/openssl.cnf` file has the following options:

```console
[openssl_init]
ssl_conf = ssl_sect

[ssl_sect]
system_default = system_default_sect

[system_default_sect]
MinProtocol = TLSv1.2
CipherString = DEFAULT@SECLEVEL=1
Options = UnsafeLegacyRenegotiation
```
