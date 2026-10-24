# watchdog_crashlog v0.1.0 candidate

`watchdog_crashlog` is a local RT-Thread package candidate for a portable C99
fault and event record core. The record ABI uses the ART-Pi2 v2 fixed-size
wire image of 4096 bytes. The package does not own a watchdog peripheral,
retained-memory address, exception vector, or persistent flash region.

This candidate is prepared locally for review and is not a public release. Its
host record-core build and focused tests pass. RT-Thread and ART-Pi2 target
builds are verified separately in the product tree. The package index URLs
remain explicit pending markers and must be replaced with real, reviewed
public URLs only after a release repository and archive exist.

## Package boundary

The RT-Thread `SConscript` includes `src/watchdog_record.c` and publishes
`include/`; it adds `src/watchdog_rtt_supervisor.c` only when the optional
`PKG_USING_WATCHDOG_CRASHLOG_RTT_SUPERVISOR` switch is enabled. The STM32H7R7
port now lives in `Drivers/ART-Pi2/watchdog_h7r7_port.c`, outside the package.
It owns IWDG setup and refresh, reset-cause translation, H7 retention/flash
geometry, and the Cortex-M fault-context mapping. Boot retains BKPSRAM setup,
exception-entry assembly, and the HyperFlash crash-log ring.

The record core operates on caller-provided memory and consumes normalized
current/previous reset causes from the platform. Raw reset bits remain opaque
in the v2 record for diagnostics. A platform port owns reset-cause decoding,
memory retention and ECC/cache/MPU policy, fault-frame capture, access
serialization, and any transport to persistent storage. CRC and commit markers
detect incomplete or accidental corruption; they do not authenticate records
or make storage power-fail-safe by themselves. A port must preserve the exact
alignment and access guarantees of its target memory.

`watchdog_trace.c` supplies a separate caller-placed 912-slot execution trace
ring. A producer may append one cycle-counted PC/LR/SP sample at 1 kHz using
aligned 32-bit stores and a commit-last sequence. The trace ring is not part of
the 4096-byte v2 record and does not define its retention or flash transport;
the platform owns both. Its CRC/commit checks detect incomplete writes but are
not an authenticity mechanism.

The v2 fault record's 95 reserved words remain part of the persisted ABI. A
consumer may use them for platform-specific runtime breadcrumbs while keeping
the outer 4096-byte image unchanged. ART-Pi2 uses these words for five
component checkpoints, the latest CPU/peripheral sample, and two rotating
Cortex-M frame samples. The package core treats those bytes as opaque; the
consumer owns their versioning, CRC/commit rules, reset lifetime and decoder.

The optional `PKG_USING_WATCHDOG_CRASHLOG_RTT_SUPERVISOR` component starts one
periodic feeder thread. It calls the consumer's health callback and calls the
consumer's hardware-feed callback only when the system tick advances and the
health callback returns nonzero. A zero return means unhealthy and prevents
feeding. Thread scheduling is reusable; health definitions,
deadlines, operation leases, and the hardware feed implementation remain
consumer policy. It does not make the watchdog peripheral independent of the
RT-Thread kernel: only the platform's hardware watchdog provides that reset
path.

## V2 wire layout

The public header fixes `WATCHDOG_RECORD_SIZE` at 4096 bytes and contains
compile-time size assertions for the v2 record components:

| Region | Size |
| --- | ---: |
| Two alternating headers | 256 bytes total (128 bytes each) |
| Fault record | 512 bytes |
| Event ring | 96 records × 32 bytes = 3072 bytes |
| Footer | 256 bytes |
| Total | 4096 bytes |

Treat this layout as a persisted wire format. Do not reorder fields, change
record sizes, commit markers, CRC coverage, or the event capacity without an
explicit format version and matching decoder/deployment policy. The package preserves the
ART-Pi2 v2 layout; it does not define the BSP memory address or a separate
HyperFlash ring format. Version 1 records are rejected; the current product
clears the reserved journal before deploying this format and does not migrate
older records.

## RT-Thread integration

Enable `PKG_USING_WATCHDOG_CRASHLOG` in menuconfig. Enable the optional
`PKG_USING_WATCHDOG_CRASHLOG_RTT_SUPERVISOR` only when the application provides
both callbacks and a valid hardware-watchdog owner. The package path and fixed
candidate version are declared in `Kconfig`; the root `SConscript` follows the
normal `DefineGroup` package flow. Core calls that share one record must be
serialized by the consumer unless its integration establishes another safe
ownership model. Do not assume every API is safe from interrupt or exception
context; use the platform's fault-entry path only for operations reviewed for
that execution context. The package-local CMake target builds the C99 record
core and its host tests. To compile the optional RT-Thread adapter through
CMake, enable `WATCHDOG_CRASHLOG_BUILD_RTTHREAD_ADAPTER` and set
`WATCHDOG_CRASHLOG_RTTHREAD_INCLUDE_DIR` to the directory containing
`rtthread.h`.

## License and publication status

`package.json` and `LICENSE` identify Apache-2.0 as the candidate license, in
line with the project's other authored package candidates. Confirm authorship
and licensing rights for every included source file before publication. The
package index currently uses the literal `PENDING_PUBLIC_*` markers; these are
not repository, homepage, or downloadable archive URLs. No public URL, package
index submission, or third-party publication is claimed.
