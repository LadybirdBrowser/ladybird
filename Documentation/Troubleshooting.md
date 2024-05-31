# Troubleshooting

In case of an error, you might find an answer of how to deal it here.

## Building Ladybird

### CMake fails to configure the build because it's outdated

Ensure your CMake version is >= 3.25 with `cmake --version`. If your system doesn't provide a suitable
version of CMake, you can download a binary release from the [CMake website](https://cmake.org/download).

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
