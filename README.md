# libmc

[![Contributor-Covenant](https://img.shields.io/badge/Contributor%20Covenant-2.1-fbab2c.svg)](CODE_OF_CONDUCT.md)
[![Maintainer](https://img.shields.io/badge/Maintainer-Cisco-00bceb.svg)](https://opensource.cisco.com)

`libmc` (lib mini-cuda) is a small CUDA-runtime-like library built directly on NVIDIA's raw RM
and UVM interfaces. It does not link against `libcuda` or `libcudart`, and it
does not aim to reimplement them.

It exists to reach something the CUDA stack does not expose.

## A GPU thread that starts its own DMA

On Hopper and later, `libmc` can have **a single SM thread issue a copy-engine
transfer**, with no host involvement between the launch and the data moving:

```c
/* One SM thread authors this copy's entire submission, on the GPU. */
mc_memcpy(ctx, dst, src, n, MC_XFER_SM);
```

That thread writes another channel's pushbuffer method stream, publishes its
GPFIFO entry, advances USERD `GP_PUT`, and finally rings that channel's BAR1
doorbell — from device code. PBDMA wakes up and the copy engine moves the bytes.

Two things this deliberately does *not* claim. The SM does not perform the
copy: it is the **producer of copy-engine commands**, and dedicated DMA
hardware still does the moving — which is the point, since the SM stays free.
And this is not device-side `cudaMemcpyAsync`, which is a request handed back
to the driver's scheduling machinery; here the command stream and the doorbell
write are authored by the thread itself.

Getting there needs the command formats, the GPFIFO and USERD layouts, and a
doorbell page mapped where a kernel can store to it. A library that goes
through `libcuda` has none of those; that is why `libmc` does not.

### Why Hopper and later

The doorbell has to be reachable from device code, which means a BAR1-mapped
usermode aperture to install a PTE against. `HOPPER_USERMODE_A` and
`BLACKWELL_USERMODE_A` take the parameter struct that selects one;
`AMPERE_USERMODE_A` is `RS_NONE` and has no such concept, so Ampere and Ada
cannot do this on any driver. It is a property of the silicon, not a gap in the
library — so ask rather than assume:

```c
if (mc_feature_supported(ctx, MC_FEAT_XFER_SM))
    mc_memcpy(ctx, dst, src, n, MC_XFER_SM);
```

Implemented and verified on H100 today. Blackwell has the aperture but no
backend yet.

Everything else `libmc` does — host-authored copies, the allocators, the VA
spaces — works on every supported architecture and is the ordinary path.

## Provenance

The project grew out of the tracing and reverse-engineering work in
[`libcuda-research`](https://github.com/maxime-peim/libcuda-research). Every
bit-exact behaviour here was established by capturing what `libcuda` does
against an instrumented driver, not by guessing. This repository contains the
reusable library, its tests, and the exact NVIDIA source dependency needed to
compile it; the research repository remains the home of the instrumented
driver, trace tooling, captures, and protocol notes.

## Compatibility

This revision targets NVIDIA driver **610.43.02**, with backends for
**Hopper**, **Ampere GA100**, **Ampere GA10x** and **Ada**. Private RM/UVM interfaces are not stable
APIs, so each `libmc` revision pins the matching official
`open-gpu-kernel-modules` source as a submodule. At runtime, `mc_init()`
verifies the loaded driver version before allocating RM objects and returns
`MC_EVERSION` on a mismatch, then identifies the GPU and selects its backend,
returning `MC_EARCH` for a chip it does not support.

Architectures differ in what they can offer, and the difference is in the
silicon rather than in the library — see *Why Hopper and later* above. Query
with `mc_feature_supported()`; a feature that is absent returns
`MC_EUNSUPPORTED` rather than failing deep inside a submission.

`MC_XFER_SM` also needs the default build: `MC_ENABLE_EXPERIMENTAL=0` produces
a library that does not contain the research paths at all, and reports the
feature as unavailable accordingly.

## Build

```sh
git clone --recurse-submodules --shallow-submodules \
  https://github.com/maxime-peim/libmc.git
cd libmc
make libmc mc-all test-unit
```

The normal build uses the committed per-architecture kernel bytes and does not
require the CUDA toolkit. Regenerating them needs `nvcc` and `cuobjdump`; SASS
does not port across SM versions, so each architecture compiles its own.

The host-only tests run without a GPU, and cover more than the version
contract: chip detection, feature derivation, method streams, QMD layouts and
the USERD address-width rules are all checked on the host. Running the
integration demos requires root access to `/dev/nvidia0`, `/dev/nvidiactl`, and
`/dev/nvidia-uvm`, plus the matching 610.43.02 kernel modules. For example:

```sh
sudo ./bin/mc_demo
```

### Other driver releases

libmc compiles against the driver source of the release it will run on.
`tools/nvidia_conftest.sh` probes that source for the few interface
differences between releases and writes `build/mc_nv_config.h`, which every
library file is compiled with. To build for a release other than the verified
one, point `NVIDIA_SRC` at its official source and opt in:

```sh
git clone --depth 1 --branch 595.99.02 \
  https://github.com/NVIDIA/open-gpu-kernel-modules.git ../ogkm-595.99.02
make NVIDIA_SRC=../ogkm-595.99.02 MC_ALLOW_UNTESTED_DRIVER=1 libmc test-unit
```

The result runs only on that exact release (the runtime version check is
unchanged), and it has not been verified on hardware by this revision unless
the release is listed above.

## Layout

- `mc/include/mc/mc.h` — the public header; the only one consumers include
- `mc/` — library implementation, grouped by layer: `core/` (lifecycle and
  VA-space policy), `driver/` (RM and UVM ioctl surfaces), `hal/` (method
  streams, submission, and the architecture descriptor), `arch/<name>/`
  (per-architecture code — `hopper`, `ampere` = GA100, `ampere_ga10x`, `ada`),
  `experimental/` (research paths), `internal/` (private headers), `kernels/`
  (device-side sources)
- `tests/mc/` — integration demos and benchmarks
- `tests/unit/` — host-only ABI and selection tests
- `tools/run_mc_tests.py` — integration-test matrix runner
- `docs/` — concise architecture and compute-launch guides
- `third_party/open-gpu-kernel-modules/` — pinned NVIDIA source submodule

Start with [`mc/README.md`](mc/README.md) for the API and constraints. The
implementation is intentionally experimental: it writes GPU command streams and
doorbells directly. It has been exercised on H100 SXM5, A100 SXM4 (80 GB and
40 GB), L4 and A2, all on driver 610.43.02. Those are not interchangeable — the
H100 evidence is SXM, not the PCIe card, and each board's link width and PCIe
generation set a different transfer ceiling, so compare a number only against
one from the same board.

## License

The `libmc` sources are MIT licensed, see [`LICENSE`](LICENSE). The NVIDIA submodule retains NVIDIA's
original license terms. See [`NOTICE`](NOTICE).

## Contact

Maxime Peim - [maxime.peim@gmail.com](maxime.peim@gmail.com)
