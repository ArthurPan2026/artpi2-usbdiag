# Third-party notices

The root [LICENSE](LICENSE) applies to original project-authored code only.
Bundled and separately obtained components retain their own terms.

| Component | Version / identity | License notice | Delivery |
|---|---|---|---|
| RT-Thread | 5.3.0 | Apache-2.0 | Source and `Appli/ThirdParty/rt-thread/LICENSE` |
| CherryUSB | 1.5.2 | Apache-2.0 | Source and `Appli/ThirdParty/cherryusb/LICENSE` |
| LVGL | 9.6.0 | MIT | Source and `Appli/ThirdParty/lvgl/LICENCE.txt` |
| NemaGFX M7 headers/archive | API 1.4.17, implementation 2024-10; GCC hard-float Cortex-M7 archive | Think Silicon notices in the headers; LVGL identifies bundled dependencies as MIT-compatible | Header files and the archive from the LVGL 9.6.0 package; archive SHA-256 is in `dependencies/third_party.lock` |
| Noto Sans SC PDF font subset | Noto Sans CJK 2.004, Simplified Chinese, static weight 400 subset used by the board-side libHaru renderer | SIL Open Font License 1.1; `Appli/User/web/assets/pdf/OFL-NotoSansSC.txt` | Subset and source SHA-256 provenance are recorded in `Appli/User/web/assets/pdf/PROVENANCE.md` and `dependencies/third_party.lock` |
| libHaru | 2.4.6, upstream commit 3467749fd1c0ab6ca6ed424d053b1ea53c1bf67c; ART-Pi2 embedded allocation/CMap patch is documented in Appli/ThirdParty/libharu/PATCHES.md | Zlib/libpng license; source notice in Appli/ThirdParty/libharu/LICENSE | C source is included in the development firmware tree and locked in dependencies/third_party.lock. The embedded target build disables PDF encryption. |
| Mongoose | 7.23 | GPLv2, Evaluation, or Commercial; see Appli/ThirdParty/mongoose/LICENSE and the vendor licensing page | Source and license notices are included. Keep the applicable license terms with any redistributed copy. |
| Infineon Wi-Fi Host Driver (WHD) | 3.3.3.26314 | Cypress/Infineon EULA in Appli/ThirdParty/wifi-host-driver/LICENSE.txt | Exact source tree and matching radio resources are bundled; SHA-256 is recorded in dependencies/third_party.lock. |
| CYW43438 firmware and CLM resources | Bundled with WHD 3.3.3.26314 | Appli/ThirdParty/wifi-host-driver/LICENSE-permissive-binary-license-1.0.txt and the WHD EULA | Firmware/CLM source arrays and NVRAM resources are included in the locked WHD tree. |
| STM32H7RS HAL | ST package supplied with this project | `Drivers/STM32H7RSxx_HAL_Driver/LICENSE.txt` (BSD-3-Clause terms apply when no package-specific terms are supplied) | Source |
| CMSIS | ARM CMSIS files supplied with this project | `Drivers/CMSIS/LICENSE.txt` (Apache-2.0) | Source |
| STM32H7RS device files | ST device package supplied with this project | `Drivers/CMSIS/Device/ST/STM32H7RSxx/LICENSE.txt` (Apache-2.0) | Source |
| APS256xx PSRAM driver | Board vendor package | `Boot/ThirdParty/aps256xx/LICENSE.txt` | Source |
| ART-Pi2 Winbond XSPI loader | ART-Pi2_ST_winbond_64MB.stldr | Board-supplied binary; no standalone license is present in the project copy | Bundled in Tools/ and checked against ARTPI2_EXTERNAL_LOADER_SHA256 in dependencies/third_party.lock. |
| CMake | 4.1.1, Windows x64 | BSD-3-Clause; `Tools/host-tools/licenses/CMake-BSD-3-Clause.txt` and the original archive | Compressed upstream release archive; SHA-256 and byte size are recorded in `Tools/host-tools/manifest.json`. |
| Ninja | 1.13.2, Windows x64 | Apache-2.0; `Tools/host-tools/licenses/Ninja-COPYING.txt` | Compressed upstream release archive; SHA-256 and byte size are recorded in `Tools/host-tools/manifest.json`. |
| Arm GNU Toolchain | 13.3.Rel1, Windows x64, arm-none-eabi | The binary archive retains upstream notices; `Tools/host-tools/licenses/Arm-GNU-Toolchain/` includes the release EULA, source manifest, and copied GCC/GDB/GFortran license notices | The binary toolchain is in `.7z`; the corresponding source snapshot is in `Tools/host-tools/sources/arm-gnu-toolchain-src-snapshot-13.3.rel1.tar.xz` with Arm's published SHA-256 sidecar. Asset hashes and byte counts are recorded in `Tools/host-tools/manifest.json`. |
| STM32CubeProgrammer CLI | 2.23.0, Windows x64 | SLA0048 Rev6 and Additional License Terms for 2.23.0; both are retained under `Tools/CubeProgrammer/doc/` | Pre-expanded CLI runtime at `Tools/CubeProgrammer/`; `Tools/CubeProgrammer/runtime-manifest.json` records the included file sizes and SHA-256 values, and `Tools/host-tools/manifest.json` records the validated ST source tree provenance. The optional J-Link library and bundled external loaders are omitted; ART-Pi2 uses ST-Link and supplies its own external Loader. The installer was downloaded by the project maintainer from ST and verified as ST-signed. |
| .NET Desktop Runtime | 10.0.13, Windows x64 | `Tools/host-tools/licenses/DotNet-Runtime-LICENSE.txt` and `DotNet-Runtime-ThirdPartyNotices.txt` | Runtime files are placed in `Tools/ArtPi2StudioRuntime/dotnet`; the official Microsoft runtime ZIP hashes and the extracted-file hashes are recorded in the runtime manifest. |
| ST-LINK Windows driver | STSW-LINK009 2.02, Windows x64 | SLA0048 Rev4; `Tools/host-tools/licenses/STSW-LINK009-SLA0048.txt` | Signed ST driver ZIP and file hashes are locked in `Tools/host-tools/manifest.json`. ART-Pi2 Studio offers installation only after detecting an affected ST-Link device and obtaining explicit Windows UAC confirmation; it does not force-replace an installed driver. |
| 7-Zip command-line extractor | 26.03, Windows x64 | `Tools/host-tools/7zip/License.txt`; includes LGPL terms, the unRAR license restriction, and BSD notices for the listed code | `7z.exe` and `7z.dll` are included so the GUI can extract tool archives without a system 7-Zip install. |
| Noto Sans SC font data | Generated from Noto Sans SC Regular | SIL Open Font License 1.1 | Font data in `Appli/User/ui/src/fonts`; license at `Appli/User/ui/assets/LICENSE-NotoSansSC-OFL.txt` |

The submission package includes the exact WHD 3.3.3.26314 source/resource tree identified by dependencies/third_party.lock and its accompanying EULA and resource license files. The WHD firmware, CYW43438 resources, and resulting firmware image remain subject to their component terms; the included notices identify the relevant files.

## Project-local changes to bundled components

The locked CherryUSB 1.5.2 source has a project build-configuration guard around
its setup-packet debug printer so Release builds do not compile an unused
debug-only helper. The locked WHD 3.3.3.26314 source uses bounded copies for
logged IOVAR strings, validates IOVAR name and payload lengths before formatting
request buffers, and explicitly terminates the WoWL pattern operation string
before constructing the driver request. These are local source changes;
component copyrights, licenses, and EULA terms remain applicable.

The bundled RT-Thread 5.3.0 scheduler has one ART-Pi2 integration point in its
stack-overflow check. It calls the project watchdog capture hook directly, so
Release can record the affected thread and stack bounds without enabling the
kernel's optional function-pointer hook set. The upstream license and notices
remain unchanged.

## Component license references

The root LICENSE applies to project-authored source. Mongoose source includes its GPLv2 license text; the vendor also offers other licensing options. Preserve the applicable component license and copyright notices when redistributing the project.

The WHD EULA and radio-resource license files are included alongside the bundled source. The ART-Pi2 Loader is included in Tools/ and its locked hash is recorded in dependencies/third_party.lock.

The submission package includes compressed Windows host-tool archives for CMake, Ninja, and Arm GNU Toolchain. Their required upstream contents and license notices are retained. `Tools/host-tools/manifest.json` records each archive's version, source URL, byte size, SHA-256, and license paths; it also records Arm's corresponding-source snapshot and its official checksum file. ART-Pi2 Studio verifies build-tool archives before extracting them to the project-local `.artpi2-studio/tools` cache by default. STM32CubeProgrammer 2.23.0 is pre-expanded in `Tools/CubeProgrammer` and its per-file SHA-256 inventory is in `runtime-manifest.json`.

The package also includes ST's signed STSW-LINK009 2.02 Windows driver and its SLA0048 notice. ART-Pi2 Studio installs it only after Windows reports a problem on a matching ST-Link device and the user confirms the Windows UAC prompt. Driver installation is never automatic at startup and does not force-remove or downgrade an existing driver.
