# VitaSDK setup

This port requires the softfp VitaSDK fork because the Android ARMv7 game
binary uses the soft-float calling convention. The official package manager
repository for that toolchain is:

```text
https://github.com/vitasdk-softfp/vdpm
```

The configured development machine uses:

```text
VITASDK: /Volumes/USB/persanal/psvita/.vitasdk-softfp
archive: vitasdk-x86_64-apple-darwin-2024-04-25_22-01-08.tar.bz2
SHA-256: 8265a24b03350ef20c2f031570d4e039ddc9c3af9c5cb65362f7b6aa00bd21e2
host:    Apple Silicon with Rosetta 2
```

The archived compiler is an x86_64 executable. On this Apple Silicon machine,
an x86_64 zstd 1.5.7 shared library was built locally and installed at:

```text
/Volumes/USB/persanal/psvita/.vitasdk-host-x86_64/lib/libzstd.1.dylib
```

Install the port dependencies after the SDK is present:

```sh
./scripts/install_vitasdk_deps.sh
```

Then configure and build:

```sh
./scripts/configure.sh Debug
./scripts/build.sh
```

The compiler wrappers add the local host library path only when that directory
exists, so Linux and native x86_64 macOS setups can use their normal loader
configuration.
