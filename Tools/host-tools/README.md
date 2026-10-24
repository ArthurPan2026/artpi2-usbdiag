# Windows host tools

This folder contains the pinned Windows x64 host-tool metadata used by ART-Pi2 Studio.
The GUI verifies each build-tool archive against `manifest.json` and expands it into
the project-local `.artpi2-studio/tools` cache by default. STM32CubeProgrammer
2.23.0 is pre-expanded under `../CubeProgrammer`, so first-run probe detection does
not need to decompress it. The 7-Zip command-line extractor is included; users do
not need to install an archive utility. Keep the license files and notices with
their redistributed tools.

| Tool | Version | Package form |
|---|---:|---|
| CMake | 4.1.1 | `cmake-4.1.1-windows-x86_64.7z` |
| Ninja | 1.13.2 | `ninja-1.13.2-win.7z` |
| Arm GNU Toolchain | 13.3.Rel1 | `arm-gnu-toolchain-13.3.rel1-mingw-w64-i686-arm-none-eabi.7z` |
| STM32CubeProgrammer CLI | 2.23.0 | Pre-expanded in `../CubeProgrammer` |

The archive extractor is bundled under `7zip/`; users do not need a separate
7-Zip installation. Tool assets use LZMA2, compression level 7, a 32 MiB
dictionary, and two compression threads to keep the exporter responsive.

CMake, Ninja, and Arm GNU Toolchain are distributed as individually verified
archives. The CubeProgrammer runtime is individually verified from
`../CubeProgrammer/runtime-manifest.json`; its optional J-Link library and
bundled ExternalLoader directory are omitted because ART-Pi2 uses ST-Link and
supplies its own external Loader. Arm's official 13.3.rel1 corresponding-source snapshot adds about
311.3 MB and is retained as a `.tar.xz` file under `sources/`; it is not
expanded by Studio. The source manifest, Arm release EULA, published SHA-256
sidecar, and component license notices are under `licenses/` and `sources/`.
The complete submission directory is measured after export and the exporter
rejects a payload at or above 1 GiB.

Preparing all three build tools expands roughly 1.3 GB into the
project-local cache by default. Retain at least 1.5 GB free so extraction can
complete safely. Tool Settings can move the cache to another drive.

License documents are in `licenses/`; the manifest records archive-internal
license entries separately from package-sidecar documents. The GUI validates
each path in its correct root before publishing a cached tool. Ninja's Windows
archive contains only `ninja.exe`, so Studio writes its embedded Apache notice
as `COPYING` during installation. The Arm source snapshot is pinned to the
official 13.3.rel1 SHA-256. The pre-expanded CubeProgrammer CLI runtime is
version 2.23.0 and includes SLA0048 Rev6 and its 2.23.0 additional license terms
under `../CubeProgrammer/doc/`.

The signed STSW-LINK009 2.02 driver bundle is included with its SLA0048 notice.
Studio offers installation only when Windows reports a problem on a matching
ST-Link device; installation requires explicit Windows UAC confirmation. It
does not install drivers at startup or force-replace an existing driver.
