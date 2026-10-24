# 构建与运行指南

本文介绍如何构建 Debug 或 Release 固件并运行比赛包。普通用户可使用提交包内的 ArtPi2Studio 图形工具完成工具检查、编译和刷写。根目录启动器与应用程序集旁置，.NET Desktop Runtime 位于 `Tools/ArtPi2StudioRuntime/dotnet/`；无需安装 PowerShell、Node.js 或全局 .NET Runtime。比赛包不包含 ArtPi2Studio 源码、测试或项目文件，但同时提供 Debug 与 Release 本地构建配置，并预附稳定的 Release Boot 和 Appli 镜像。主机侧构建会通过 GUI 内置的构建命令生成 Web/PDF 资源并检查字体/UI 源码。有 ART-Pi2、ST-Link 和真实 HID 时，可通过现有 Web API 做短板端功能核验。不要将主机侧检查结果写成板端验证，也不要把 MCU 软件复位称为物理断电或 USB 拔插。

## 1. 环境和依赖

- Windows 10/11 x64。启动项目根目录中的 `ArtPi2Studio.exe` 即可进入图形工具；需要随包提供的 .NET Desktop Runtime 子目录，无需全局安装 .NET。
- 提交包仅包含 ArtPi2Studio 发布文件与运行时，不包含其 C# 源码、项目文件、测试或源码哈希清单。
- 提交包包含 STSW-LINK009 2.02 签名驱动及 SLA0048 许可。只有 Windows 报告匹配的 ST-Link 设备存在驱动问题时，GUI 才提供“安装驱动”；用户确认 UAC 后才运行安装。启动时不会自动安装，也不会强制替换现有驱动。
- 提交包包含固定校验的 Windows x64 版 CMake 4.1.1、Ninja 1.13.2 和 Arm GNU Toolchain 13.3.Rel1 压缩包，以及 7-Zip 提取器。构建默认使用包内工具。首次构建发现包内工具尚未解压时，Studio 会检查磁盘空间并自动验证、解压，过程会显示进度。工具写入默认缓存 `.artpi2-studio/tools`。三种工具同时准备约需 1.33 GiB 可用空间。
- 你可以在工具设置中手动指定本机工具，也可以设置对应环境变量。手动或环境路径优先于包内版本；包内归档版本和哈希仍保持固定。本机 CMake 要求 3.22 或更新版本；Ninja 必须能运行并报告版本；GNU Arm GCC 必须针对 `arm-none-eabi`，并提供编译器和配套工具。Studio 会把所选 GCC 前缀传给 Appli 和 Boot 子构建，并记录实际使用的 GCC 版本。若旧 CMake 缓存绑定了另一工具链，Studio 会停止并提示改用新目录或清理当前配置的构建目录。
- STM32CubeProgrammer CLI 2.23.0 以预展开的精简运行目录随包提供。包内版本通过文件清单、逐文件 SHA-256 和版本核验。本机手动选择的版本不要求与 2.23.0 相同；Studio 检查命令行功能和版本输出，再进行只读 ST-Link 列表查询。探针列表查询不会连接或复位目标板。Arm 官方 13.3.Rel1 对应源码快照、SHA-256、EULA 和组件许可文件也随包提供；源码快照保留为压缩归档，GUI 不会解压。导出工具会测量提交目录实际大小并限制在 1 GiB 以下。普通 GUI 编译不要求 Node.js、PowerShell、全局 .NET Runtime、npm 包或 Playwright。
- RT-Thread、CherryUSB、LVGL、Mongoose、STM32 HAL、CMSIS、NemaGFX、WHD 3.3.3.26314 和 CYW43438 无线资源均包含在提交包中。GUI 编译前会依据依赖锁检查版本与 SHA-256。
- WHD 使用提交包中的锁定源码树，无需另行获取。
- 工程资源中包含目标无线模块使用的生成 MAC 配置 `Appli/ThirdParty/wifi-host-driver/generated_mac_address.txt`。更换无线模块或同时运行多块板时，先确认地址适用于目标硬件且不会重复。
- 板端刷写另需 ART-Pi2、ST-Link 和目标板连接的真实 HID。ART-Pi2 外部 Flash Loader 已包含在 Tools 目录中；GUI 会校验其 SHA-256。

GUI 可通过环境变量、PATH 和常见安装目录发现工具，也允许在界面中浏览选择路径：

| 环境变量 | 用途 |
|---|---|
| `ARTPI2_CMAKE_EXE` | CMake 可执行文件路径 |
| `ARTPI2_NINJA_EXE` | Ninja 可执行文件路径 |
| `ARTPI2_GNU_ARM_BIN` | GNU Arm Embedded 工具链 `bin` 目录 |
| `STM32_CUBE_PROGRAMMER_CLI` | STM32CubeProgrammer CLI 可执行文件路径 |
| `ARTPI2_STLINK_SERIAL` | ST-Link 序列号；也可在 GUI 中输入 |

开发者运行 JavaScript 回归或手工资源工具时，可选设置 `ARTPI2_NODE_EXE` 指向 Node.js；它不是普通 GUI 编译的依赖。

第三方组件及其随附许可见 [`../../THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md)。

## 2. 使用 GUI 编译 Debug 或 Release

从提交包根目录打开 `ArtPi2Studio.exe`。GUI 启动后会校验包内 STM32CubeProgrammer 2.23.0 的文件清单和版本，然后扫描 ST-Link。这个过程只枚举探针，不连接或复位开发板；CubeProgrammer 不需要解压。构建默认使用包内 CMake、Ninja 和 GNU Arm GCC。首次构建时，若缓存中缺少包内工具，Studio 会自动准备。手动选择的兼容工具和环境变量优先于包内版本。本机 CubeProgrammer 需支持 Studio 使用的下载、擦除、校验、复位和外部 Loader 命令；版本号可以不同于包内的 2.23.0。更换 GCC 版本后，构建身份会记录实际编译器版本；已有 CMake 缓存若绑定其他编译器，Studio 会拒绝复用，请清理当前配置的构建目录或选择新目录。首次运行不需要预先配置 Node.js、PowerShell、.NET Runtime 或 npm 包。WHD 使用包内锁定版本。

选择 Debug 或 Release 并点击“构建”。构建成功后，GUI 显示产物路径、大小和 SHA-256，并在当前会话选择本次构建的对应配置镜像。Debug 不会回退到 Release 镜像。Appli 与 Boot 的构建目录可在 GUI 中分别选择；默认分别位于 `Appli/build/<配置>/` 与 `Boot/build/<配置>/`。顶层 CMake 协调缓存位于工程根目录 `build/<配置>/`，各配置互相隔离。工具缓存默认位于项目根目录 `.artpi2-studio/tools/`，可在工具设置中改到其他磁盘。解压前 GUI 会检查缓存目录的写入权限和可用空间。

编译与刷写互相独立：编译完成不会自动写入开发板。刷写区域会显示具体镜像路径与哈希，可选择本地构建的 Debug/Release 镜像或包内稳定 Release 镜像。

编译和刷写只通过 GUI 执行。默认构建目录分别位于 `Appli/build/<配置>/`、`Boot/build/<配置>/` 和工程根目录 `build/<配置>/`；可在 GUI 中为 Appli 与 Boot 分别选择目录。Debug 与 Release 产物互相隔离，GUI 显示镜像路径、大小和 SHA-256。比赛包预附稳定的 Release Boot 与 Appli 镜像，不附 Debug 预构建镜像。更新看门狗恢复功能时，应同时刷写匹配配置的 Boot 与 Appli；日常 Appli 更新可只刷外部 XSPI XIP 区 `0x70000000`。Boot 仅占用内部 Flash 的前 48 KiB，GUI 会保护 Wi-Fi 配置区 `[0x0800C000, 0x08010000)`。外部 Flash 顶部 320 KiB（`0x73FAF000–0x73FFFFFF`）包括 64 KiB 的执行轨迹 A/B sidecar，以及原址不变的 260 KiB 日志区（64 个 4 KiB 日志扇区和 1 个事务暂存扇区）；Boot 校验轨迹和日志各自的 CRC、提交标记后，才回收旧记录。

可独立构建共享 C99 USB 规则核心和测试：

```powershell
cmake -S packages/usb_enum_diag -B ..\build\artpi2-usbdiag\usbdiag-host -G Ninja
cmake --build ..\build\artpi2-usbdiag\usbdiag-host
ctest --test-dir ..\build\artpi2-usbdiag\usbdiag-host --output-on-failure
```

共享的看门狗记录格式也可在主机侧检查：

```powershell
.\tests\watchdog_record\run.ps1 -ArtifactDirectory ..\build\artpi2-usbdiag\watchdog-record-host
```

该脚本以 Release/NDEBUG 构建策略执行 BKPSRAM 擦除门控、tamper/POM/IWDG 判定、记录格式 CTest 和日志解码器测试；它也纳入 `test-artpi2-all.ps1` 与比赛包生成前检查。普通软件或看门狗复位配合 `MEF=0` 必须继续启动；由于 RCC 复位标志会累积，只有未伴随软件、看门狗、选项字节或低功耗复位标志的 POR，或当前触发擦除的 tamper，且 `MEF=0` 才进入等待。新鲜 POR 同时带有 PIN/BOR 标志时仍按 POR 等待。

GUI 在构建前会依据 `dependencies/third_party.lock` 检查捆绑依赖与 WHD；GUI 内置的资源生成和字体/UI 检查命令不依赖 Node.js 或 PowerShell。比赛包不提供重复的固件构建、刷写包装脚本。

## 3. 使用 GUI 刷写和核验

连接 ART-Pi2 和 ST-Link。GUI 会自动列出已连接探针。只有检测到一个探针时才自动选择；检测到多个探针时，请明确选择目标。你可以刷写本地 Debug/Release 构建，也可以选择包内稳定 Release 镜像。首次使用默认选择稳定 Release 镜像、勾选 Boot 和 Appli、关闭回读校验，并将 SWD 频率设为自动。自动模式按探针型号，从 ST 文档列出的最高 SWD 档位开始做只读连接探测，选择可建立连接的最高档；确认框会显示实际使用频率。你也可以手动选择较低档位。该探测不下载、复位或写入目标。用户切换的 Boot、校验和频率选项会按工程与配置保存；“恢复默认值”可还原首次部署选择。包内 Loader 会自动使用并校验 SHA-256；如选择其他 Loader，必须与锁定依赖匹配。

首次部署默认同时刷写 Boot 和 Appli。Appli 写入外部 XSPI XIP 区 `0x70000000`，Boot 写入内部 Flash `0x08000000`；Boot 必须限制在内部 Flash 前 48 KiB。若没有有效 Boot 镜像，GUI 会禁用刷写。回读校验默认关闭，用户可勾选启用。刷写前 GUI 展示目标、镜像来源、完整路径、大小、SHA-256、Loader、地址和校验状态供确认。普通刷写只擦除镜像覆盖的扇区，不执行整片擦除，并保留 Wi-Fi 配置区 `[0x0800C000, 0x08010000)` 与看门狗历史。不要只根据文件名判断镜像新旧，按 GUI 显示的完整路径和 SHA-256 核对。

“烧录前完整擦除 Flash”选项默认不勾选，也不会保存到下次启动。启用后，GUI 强制要求同时刷入有效 Boot 和 Appli 镜像，并在确认框列出擦除范围。Studio 先擦除整颗 64 MiB XSPI2 外部 Flash，再擦除 MCU 内部 Flash，随后写入 Boot 和 Appli。此操作会清除 Wi-Fi 配置、看门狗日志及外部 Flash 中的其他数据。刷写完成后需重新配置 Wi-Fi。Option Bytes、OTP 和读保护设置保持不变。

完成刷写后，连接真实 HID 并按 [`functional-validation.md`](functional-validation.md) 检查 LCD、Wi-Fi、Web 和证据导出。维护者可以使用 `test-artpi2.ps1` 做 HTTP 冒烟；该脚本是可选的 PowerShell 入口。

### 可选 HTTP 冒烟

可选的 test-artpi2.ps1 只通过公开 Web API 做板端功能冒烟，不是构建或刷写入口。正常使用 GUI 不需要运行该脚本。

### 读取故障现场和恢复 Boot

看门狗日志只在 IWDG 复位或已捕获异常后由 Boot 写入，不会在健康运行期间持续写 Flash。读取日志、执行轨迹和当前 BKPSRAM 保留现场时，只需运行一个脚本：

软件重启、外部复位、正常上电或掉电不会新建故障日志，也不会计入连续 IWDG 复位次数；正常重启会清除连续复位计数。若上一次异常记录因 Flash 写入暂时失败而仍待提交，Boot 会保留并重试写入那条异常记录。

```powershell
.\Tools\watchdog\inspect_watchdog_faults.ps1 -SerialNumber $stlink `
  -AppElf .\Appli\build\Release\artpi2-usbdiag_Appli.elf `
  -BootElf .\Boot\build\Release\artpi2-usbdiag_Boot.elf
```

上例使用 Release ELF；若读取 Debug 构建的地址，将两个路径中的 `Release` 替换为 `Debug`。

若不需要符号化地址，只需传入 `-SerialNumber`；也可预先设置 `ARTPI2_STLINK_SERIAL`。输出保存在 `output\watchdog-readback-<时间戳>`，包含原始读回、SHA-256、逐区解码 JSON 和汇总。BKPSRAM 连续读取两次用于检测读回变化；哈希一致不代表 CPU 运行中的记录具有原子性。空白 Flash 区会标为无记录；非空但 CRC/提交校验失败的区域会单独标示为不完整或损坏，原始读回仍会保留。

Boot 进入连续复位恢复状态时，UART4（115200、8N1）会等待输入 `RECOVER` 加回车。该命令清除连续复位计数并重启 MCU；若故障仍在，后续 IWDG 复位会再次保存现场。Release 与 Debug 都在 4 KiB BKPSRAM 中周期保存 1 Hz 被打断线程的 PC/LR、PSP/MSP、EXC_RETURN、异常屏蔽状态、RT-Thread 线程名/状态/优先级、CFSR/HFSR 与 LTDC/GPU/DMA 错误摘要；另保留最近两份带 CRC/提交字的 Cortex-M 硬件栈帧寄存器（R0-R3、R12、XPSR）。组件检查点覆盖 LVGL、Mongoose、Wi-Fi worker、USB analyzer 和 PDF worker；RTOS 核心、LVGL/Mongoose 循环以及 LTDC 帧进度参与健康判断。看门狗记录不是完整 RAM dump：若全局关中断或 CPU 停在 SysTick 无法运行的状态，只能保留最后一次已提交采样，不能在复位后恢复停机瞬间的寄存器；同步异常帧仍由异常入口单独记录。记录不会复制 WiFi 密码、SSID、HID 序列号或原始描述符。

## 4. 功能核验和报告边界

- 功能核验步骤见 functional-validation.md；可选的 `test-artpi2.ps1` 通过现有 Web API 检查板端 Release 身份、Wi-Fi、真实 HID 和证据 CRC。
- 报告应写明实际构建 profile、主机测试、板测、实际物理操作和未覆盖项；此包内 Web 冒烟脚本核对的是 Release 板端身份。
- Debug 与 Release 本地构建应分别记录各自产物路径和 SHA-256，不要将包内稳定 Release 镜像的身份归给 Debug 构建。
- 固件包含构建时间戳，当前不承诺逐字节可复现的镜像哈希。用依赖锁、源码指纹和每次构建的 SHA-256 记录来源与产物。
- 本机原始截图、临时抓包、构建缓存和多轮候选产物不属于源码发布包；经筛选脱敏的验证摘要可保存在项目文档中。本地原始材料另存于仓库同级的开发归档目录。
