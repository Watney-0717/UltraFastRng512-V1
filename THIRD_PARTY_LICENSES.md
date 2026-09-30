# Third-Party Licenses

The V1 release tree does not intentionally bundle third-party source code as part of the UltraFastRng512 core implementation.

## OpenSSL / libcrypto

The V1 build links against the system-provided OpenSSL `libcrypto` library for cryptographic primitives used by the implementation. OpenSSL is an external build dependency and its own license and notices remain applicable. The exact OpenSSL version is determined by the build environment.

Official project: https://www.openssl.org/

## Compiler / C runtime / pthreads / math library

GCC/Clang, the C runtime, pthreads, and the math library are expected to be provided by the target system/toolchain. They are not relicensed by this repository.

## External benchmark references

Any externally published benchmark values or comparison references discussed in documentation are contextual references; the originating project's own license and citation requirements remain applicable.
