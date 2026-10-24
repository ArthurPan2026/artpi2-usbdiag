# wifi_profile_manager v0.1.0 candidate

This local RT-Thread package prototype is extracted from ART-Pi2's Wi-Fi
profile state machine. It provides a bounded C99 core for saved profiles,
automatic reconnect selection, temporary connections, asynchronous radio jobs,
asynchronous persistence jobs, cancellation, and public snapshots.

The manager core depends only on the C standard library. It does not call WHD,
RT-Thread WLAN, HAL, flash, LCD, Web, or application services. Consumers
execute JOIN, LEAVE, and STORE jobs through their own adapters and report
results back to the core. The optional FinSH command demonstrates the job
boundary only; it does not connect to a radio.

All time arguments (`now_ms`) and the snapshot field `retry_in_ms` use
monotonic milliseconds. RT-Thread examples must convert ticks according to
`RT_TICK_PER_SECOND` before calling the core. Use one time source per core;
all scheduled intervals must be shorter than `2^31` milliseconds for the
32-bit wrap-safe deadline comparisons.

## Integration contract

Serialize every call for one manager instance through a single owner context.
The core is neither thread-safe nor ISR-safe. An asynchronous radio or storage
worker should execute a copied job, then send its result back to the owner
before calling the corresponding result API. Use `wpm_core_adapter_snapshot()`
for adapter state, `wpm_core_report_error()` for external operation failures,
and the dedicated cleanup, auto-suspend, and DHCP-reset APIs instead of reading
or writing fields inside `wpm_core_t`.

`wpm_core_next_job()` copies credentials into the returned job. The adapter
must securely clear that job after use and must not log its profile or password
fields. Public snapshots omit passwords. Manager return codes use the named
`WPM_E*` constants in the header; `WPM_CORE_ALREADY_CONNECTED` is a positive
no-op result for a matching durable online profile.

## Optional transactional persistence

`wpm_nv_journal.h` and `src/wpm_nv_journal.c` provide a separate optional
module for storing bounded opaque payloads. The caller owns each journal
instance and supplies its I/O callbacks and geometry. Journal v1 supports two
equal erase sectors, a 16-byte program unit, `0xFF` erased bytes, and payloads
up to 2048 bytes (subject to the selected sector size). The backend receives
region-relative offsets and sector indices; the package contains no MCU
addresses, HAL calls, or implicit hardware backend.

The module writes append-only records, validates header and payload CRCs,
commits only after the record body is written, verifies erase operations, and
recovers the newest valid sequence after interruption. `scrub_old` commits
the replacement before retiring the old sector. Callers must serialize access;
the busy flag detects callback reentry and is not a thread lock. CRC detects
accidental corruption and does not encrypt or authenticate credentials.
Storage adapters remain responsible for physical write/erase guarantees,
protected reads, access control, and any at-rest encryption.
`wpm_nv_journal_reset()` is an explicit destructive operation: it erases and
reads back every configured sector, and succeeds only when the whole region is
verified blank. Initialization and loading never call it implicitly; the
application must decide when discarding the stored payload is appropriate.

Enable `PKG_USING_WIFI_PROFILE_MANAGER_NV` in RT-Thread Kconfig to include the
module from `SConscript`. The standalone CMake project builds it as the
separate `wifi_profile_manager_nv` target.

## Format and feature limits

- At most WPM_PROFILE_MAX saved profiles, configurable from 1 to 8.
- SSIDs are limited to 32 bytes. Security modes currently cover open, WPA2,
  and WPA/WPA2 mixed mode; WPA3, enterprise credentials, and binary secrets
  are not implemented by this prototype.
- The v1 configuration codec stores bounded binary profile records. It is
  not encryption. Adapters must protect stored credentials and must not expose
  private profile structures through public status.
- The v1 configuration codec uses the package-defined WN01 wire identifier.
  This is the package's only decoder and its initial persisted format; there is
  no legacy fallback or automatic migration path. A future incompatible
  format requires an explicit version/migration decision.
- Wire v1 requires all reserved and unused padding bytes to be zero. Reducing
  `WPM_PROFILE_MAX` below a value already stored will make that payload fail
  validation; the consumer must migrate or clear that data intentionally.
- `wpm_core_ip_ready()` accepts a caller-provided IPv4 or IPv6 presentation
  string up to 45 bytes. The adapter decides when usable network addressing
  has completed; the manager does not parse or configure IP addresses.
- The manager snapshot contains only state maintained by this core. Radio
  bootstrap phases, UI startup gating, transport request results, and
  application-specific save receipts belong in an application-owned wrapper.

## ART-Pi2 integration

ART-Pi2 uses this package as its production Wi-Fi profile and connection-policy
core. The firmware build compiles this package's `src/wifi_profile_manager.c`
directly; `Appli/User/wifi/include/wifi_manager_core.h` is a compatibility
facade that maps ART-Pi2 application names to the package's `wpm_*` API.
The Wi-Fi host regression also compiles this same package source, rather than a
copied test implementation. ART-Pi2's Wi-Fi worker uses the package's
`wpm_nv_journal` for persistence; its adapter maps the journal's region offsets
to STM32H7R7 internal Flash and supplies the WHD radio operations. The project
therefore exercises both the manager core and the journal on the target.

The extraction also builds outside the firmware tree. It does not yet prove
interoperability with a second Wi-Fi driver implementation. This is additional
portability evidence, not a substitute for the current ART-Pi2 integration or
the package's own adapter/API contract.

## Build and test

Use CMake with C99, then run CTest. The host tests exercise connection
requests, job ordering, profile persistence, automatic retries, cancellation,
stale operation results, bounded capacity, and configuration
encoding/decoding through the same core source used by SCons. If a C++ compiler
is available, CMake also links a C++ consumer against the C99 library to check
the public header's C linkage.

## Release status

This is a local review prototype only. Public repository and archive URLs are
intentionally TODO values; no package release or index submission has been
made. Source files are project-authored and covered by the accompanying
Apache-2.0 license. The package contains no vendor code;
device services stay in the application adapter.
