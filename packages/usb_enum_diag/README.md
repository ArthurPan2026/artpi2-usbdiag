# usb_enum_diag v0.1.0

`usb_enum_diag` is a portable C99 core for evaluating USB enumeration evidence
as a bounded session. Firmware and host-side tests use the same implementation.
The core performs no dynamic allocation and does not depend on RT-Thread,
CherryUSB, HAL, LVGL, or a network stack.

## Rules and evidence

The session API evaluates twelve enumeration and descriptor rules. Results
include bounded source-event and byte references, first-failure witnesses, and
counts for references omitted at capacity. Missing evidence, forgotten source
events, capacity exhaustion, and unknown speed cannot become a passing result.

The application acquires USB events, stores the original bytes, publishes
immutable snapshots, and presents results. An RT-Thread adapter maps
CherryUSB events and core verdicts into the application schema without
duplicating rule calculations.

## Build and run the C99 core tests

Run these commands from the firmware project root. The build output stays next
to the project checkout:

```powershell
cmake -S packages/usb_enum_diag -B ..\build\artpi2-usbdiag\usbdiag-host -G Ninja
cmake --build ..\build\artpi2-usbdiag\usbdiag-host
ctest --test-dir ..\build\artpi2-usbdiag\usbdiag-host --output-on-failure
```

The RT-Thread build uses the package `SConscript`; the optional example adds a
FinSH smoke command when enabled. It does not simulate USB hardware or replace
the board's live capture path.

## License

The shared core source is licensed under Apache-2.0; see `LICENSE`.
