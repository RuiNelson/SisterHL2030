# SisterHL2030

Open-source driver for the Brother HL-2030, targeting native **Apple Silicon**
macOS. Version 1 supports this model only.

The HL-2030 is a host-based laser: it does not speak PostScript, PCL 5 as a
full PDL, AirPrint, or IPP Everywhere. The host must rasterize the page and
send Brother's compressed stream over USB.

## Features

* Apple Silicon support
* When printing a page, it will default to 100% scale
* Floyd-Steinberg half-toning algorithm
* AirPrint support and network sharing support
* Consumable levels reporting

## Quality Modes

* **Draft** — 300dpi, toner saving mode
* **Normal** — 600dpi, toner saving mode
* **Fine** — 1200dpi

## Status

| Piece | State |
| --- | --- |
| Protocol recovered from Linux i386 blobs | done |
| Envelope confirmed vs official macOS `rastertobrother2030` | done |
| Native `arm64` raster encoder (mode 1030) | prints on USB HL-2030 |
| Toner / drum levels via USB PJL | shown in Supply Levels |
| CLI `sister-rawtobr` (PBM → job stream) | done |
| PAPPL printer application (IPP Everywhere façade) | next |
| Signed macOS `.pkg` | later |

This is not a port of the 2005 i386 `rawtobr2` binary. That encoder is a
stripped ELF with no source; SisterHL2030 reimplements the job format
documented in [`docs/protocol.md`](docs/protocol.md).

## License

GNU GPL v2 or later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

The license matches the CUPS wrapper sources Brother already shipped under
GPL-2, and stays compatible with [brlaser](https://github.com/pdewacht/brlaser).

`LinuxDrivers/` keeps the original Brother packages as a protocol reference.
The proprietary i386 blobs are **not** linked into this driver.

## Build

Requires CMake 3.20+ and a C++17 compiler (Apple Clang on macOS is enough).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Apple Silicon this produces native `arm64` binaries. A universal extra
slice can be added later with `CMAKE_OSX_ARCHITECTURES=arm64;x86_64`.

`sister-rawtobr` reads a binary PBM (`P4`) on stdin and writes a HL-2030 job
on stdout:

```sh
./build/sister-rawtobr < page.pbm > job.bin
```

Sending `job.bin` to the printer (USB `04f9:0027`) is a hardware test.

To replace the Intel Brother CUPS package with this native filter, run
the two programs in `Scripts/` **in this order** (from Terminal; you can
drag the file onto the Terminal window and press Enter):

1. `Scripts/Uninstall Official Brother Drivers.sh` — removes the 2014
   x86_64/i386 Brother package.
2. `Scripts/Install Sister HL2030.sh` — builds the arm64 filter, installs
   it into `/Library/Printers/SisterHL2030/`, and points the USB queue at
   it.

The installer **refuses to continue** if it still finds the official
Brother drivers, so they cannot be stacked. macOS will ask for an
administrator password on each step.

The queue is created as **IPP Everywhere** (`ipp://127.0.0.1:8631/ipp/print`)
talking to a local `ippeveprinter` LaunchDaemon. That is the replacement
for a CUPS PPD, which `lpadmin` already reports as deprecated. The daemon
runs the arm64 encoder and sends the job to the HL-2030 over USB.

## Architecture (v1)

```
document → raster (1-bit) → sister encoder → USB
```

The product architecture is a [PAPPL](https://github.com/michaelrsweet/pappl)
printer application: a userspace IPP Everywhere printer that CUPS on modern
macOS talks to without a PPD. Classic CUPS filters are deprecated since
CUPS 2.3 / macOS 11.

## Hardware

| | |
| --- | --- |
| Model | Brother HL-2030 series |
| USB | Vendor `0x04f9`, product `0x0027`, serial `B9J561723` |
| CUPS URI | `usb://Brother/HL-2030%20series?serial=B9J561723` |
| IEEE 1284 | `MFG:Brother;MDL:HL-2030 series` |
| Resolutions | 300, 600, HQ1200 (1200×600 advertised) |
| Duplex | no (v1) |
| Connection | USB full-speed (12 Mbit/s) |

## References

- [`docs/protocol.md`](docs/protocol.md) — job format
- [`docs/reverse-engineering.md`](docs/reverse-engineering.md) — blob inventory
- Brother Linux LPR 2.0.1 / cupswrapper 2.0.1 (GPL-2 glue + i386 encoder)
- [OpenPrinting: Printer Applications](https://openprinting.github.io/cups/drivers.html)
- [brlaser](https://github.com/pdewacht/brlaser) — independent open-source encoder for the same family
