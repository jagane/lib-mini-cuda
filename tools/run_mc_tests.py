#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: MIT
"""
run_mc_tests.py — sweep the mc test binaries across a parameter
matrix and report pass / fail per cell.

Covers every test in tests/mc/ except mc_host_register_bench,
which is a benchmark rather than a pass/fail test and is run by hand.

The mc test set has two shapes:

  - parameterised demos (--size / --iters / --h2d, sometimes --fb / --sysmem):
      mc_demo, mc_carrier_demo, mc_sm_owner_demo,
      mc_compute_dbell_chain_demo, mc_dbell_chain_demo
  - one-shot tests with no flags:
      mc_allocation_test, mc_compute_dbell_demo, mc_dbell_demo,
      mc_features_demo

For each test we exercise the cross product of relevant parameters
(small + big sizes, multiple iters, both directions, both carriers
where applicable) and a couple of stress points (>= the 512-entry
GPFIFO ring boundary so the wrap path is hit).

Usage
-----

  python3 tools/run_mc_tests.py
      [--bin-dir bin]
      [--quick]          (only one size per test, --iters 4)
      [--stress]         (also run --iters 600 to cross the GPFIFO wrap)
      [--filter PAT]     (only run tests whose binary basename matches PAT)
      [--verbose]        (stream every command's stdout/stderr live)

Exit code is 0 iff every cell passed.

The script does NOT build anything — point --bin-dir at a tree that
already has the binaries.  Default is `bin/` relative to the
repository root inferred from the script's own location.
"""
from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

# ─── matrix definition ────────────────────────────────────────────────────────


@dataclass
class Cell:
    """One concrete test invocation."""
    binary: str
    args: list[str]
    timeout_s: float = 60.0
    # Optional predicate run on stdout+stderr to confirm the test
    # actually did what we expected (some demos print PASS even when
    # internal verifies skipped).  Default: just check exit==0.
    extra_check: Callable[[str], bool] | None = None
    # mc_feature_name() values this cell needs.  A GPU that lacks any of
    # them cannot run the cell at all, and the cell is skipped rather than
    # run and counted as a failure -- see `requires` handling in main().
    # Empty means "works on anything libmc supports".
    requires: tuple[str, ...] = ()

    @property
    def label(self) -> str:
        return f"{self.binary} {' '.join(self.args)}".rstrip()


# ─── capability query ─────────────────────────────────────────────────────────
#
# Not every supported architecture can do everything libmc implements.  Ampere
# and Ada have no BAR1 usermode aperture, so they have no doorbell PTE, and
# therefore no FB carrier and nothing SM-authored -- roughly a third of this
# matrix is unrunnable there as a property of the silicon.
#
# Before this existed the harness ran those cells anyway and reported them as
# failures, so a perfectly correct Ampere run looked like 27 regressions.  Ask
# the library what the GPU can do and skip the rest.

_RE_FEATURES = re.compile(r"^features:(.*)$", re.MULTILINE)


def device_args(device: int) -> list[str]:
    """The "--device N" pair to append to a tool invocation, or nothing for
    device 0 so default runs invoke the tools exactly as before."""
    return ["--device", str(device)] if device else []


def query_features(bin_dir: Path, device: int, verbose: bool) -> set[str] | None:
    """Ask mc_features_demo --porcelain what the selected GPU offers.

    Returns None if the query could not be run, which is deliberately
    different from an empty set: "we do not know" must not silently skip the
    whole matrix.  Callers treat None as "run everything", so a broken query
    degrades to the old behaviour rather than to a vacuous green run.
    """
    exe = bin_dir / "mc_features_demo"
    if not exe.exists():
        return None
    try:
        proc = subprocess.run([str(exe), "--porcelain", *device_args(device)],
                              capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"warning: feature query failed to run ({exc}); "
              f"running every cell", file=sys.stderr)
        return None
    if proc.returncode != 0:
        print(f"warning: feature query exited {proc.returncode}; "
              f"running every cell", file=sys.stderr)
        if verbose:
            print(proc.stdout + proc.stderr, file=sys.stderr)
        return None
    m = _RE_FEATURES.search(proc.stdout)
    if m is None:
        print("warning: feature query printed no 'features:' line; "
              "running every cell", file=sys.stderr)
        return None
    return set(m.group(1).split())


def _expect_pass_keyword(out: str) -> bool:
    """Most mc_* tests print 'PASS' on success."""
    return "PASS" in out


def _expect_no_fail_keyword(out: str) -> bool:
    """Some tests print only 'mc_init ok' / 'OK' on success."""
    return ("FAIL" not in out) and ("error" not in out.lower())


def build_matrix(quick: bool, stress: bool) -> list[Cell]:
    """Return the full sweep as a flat list of cells."""

    cells: list[Cell] = []

    # Sizes that exercise:
    #   - latency-bound regime (4 KiB)
    #   - mid-size where per-submission latency starts to amortize (1 MiB)
    #   - big enough to saturate (8 MiB / 64 MiB)
    sizes_full   = ["4K", "1M", "8M", "64M"]
    sizes_quick  = ["8M"]
    sizes        = sizes_quick if quick else sizes_full

    iters_default = "4"

    # ── parameterised carrier demo: mc_carrier_demo (the broadest one) ──
    # 4-way matrix: (carrier × direction).  Each arm names its own carrier VA
    # space, and the FB one needs a BAR1 usermode aperture to exist.
    #
    # Both arms also need xfer_sm.  The demo's subject IS the SM-authored
    # submission path -- its MC_XFER_HOST calls only seed and verify -- so
    # without xfer_sm there is nothing left of it to run.  That means neither
    # arm runs on Ampere or Ada, and no cell in this matrix currently moves
    # data over a carrier VA space on those architectures even though they
    # bring one up successfully.  Closing that needs a host-authored carrier
    # transfer demo, which does not exist yet.
    carrier_feature = {
        "--sysmem": ("carrier_sysmem", "xfer_sm"),
        "--fb":     ("carrier_fb",     "xfer_sm"),
    }
    for carrier_flag in ["--sysmem", "--fb"]:
        for direction in ["", "--h2d"]:
            for size in sizes:
                args = [carrier_flag, "--size", size, "--iters", iters_default]
                if direction:
                    args.append(direction)
                cells.append(Cell(
                    binary="mc_carrier_demo",
                    args=args,
                    extra_check=_expect_pass_keyword,
                    requires=carrier_feature[carrier_flag],
                ))

    # ── host-authored transfers over a carrier: mc_carrier_host_demo ──
    #
    # The same carrier VA spaces, exercised with MC_XFER_HOST only, so these
    # need the carrier and nothing else.  That makes the --sysmem arm the only
    # cell that moves data over a carrier on an architecture without xfer_sm,
    # which is what covers Ampere's and Ada's carrier CE channel -- the one
    # whose USERD placement is architecture-dependent.  Without it, "the
    # channel comes up" was the whole of the evidence.
    host_carrier_feature = {"--sysmem": ("carrier_sysmem",),
                            "--fb":     ("carrier_fb",)}
    for carrier_flag in ["--sysmem", "--fb"]:
        for direction in ["", "--h2d"]:
            for size in sizes:
                args = [carrier_flag, "--size", size, "--iters", iters_default]
                if direction:
                    args.append(direction)
                cells.append(Cell(
                    binary="mc_carrier_host_demo",
                    args=args,
                    extra_check=_expect_pass_keyword,
                    requires=host_carrier_feature[carrier_flag],
                ))

    # ── parameterised host-side demos: mc_demo, mc_sm_owner_demo,
    #    mc_compute_dbell_chain_demo, mc_dbell_chain_demo ──
    # 2-way matrix: direction.
    #
    # mc_demo is the only one with no requirement: a host-authored CE copy is
    # the one thing every supported GPU provides.  The rest need the GPU to
    # ring its own doorbell, to launch a kernel, or both -- all of which need
    # the BAR1 usermode aperture that Ampere and Ada do not have.
    param_requires: dict[str, tuple[str, ...]] = {
        "mc_demo":                     (),
        "mc_sm_owner_demo":            ("xfer_sm",),
        # Calls mc_memcpy_gpu_doorbell_sm -- the SM-authored path, whose
        # feature already implies compute_launch and ce_doorbell_ring
        # through the dependency closure.
        "mc_compute_dbell_chain_demo": ("xfer_sm",),
        "mc_dbell_chain_demo":         ("ce_doorbell_ring",),
    }
    for binary, needs in param_requires.items():
        for direction in ["", "--h2d"]:
            for size in sizes:
                args = ["--size", size, "--iters", iters_default]
                if direction:
                    args.append(direction)
                cells.append(Cell(
                    binary=binary,
                    args=args,
                    extra_check=_expect_pass_keyword,
                    requires=needs,
                ))

    # ── one-shot tests: just exercise that they still link/run ──
    #
    # mc_allocation_test is deliberately unconditional.  It sweeps VA-space
    # pairs, only some of which involve the FB carrier, and it skips those
    # itself; gating the whole binary on carrier_fb would drop the UVM and
    # sysmem-carrier coverage that Ampere and Ada can perfectly well run.
    oneshot_requires: dict[str, tuple[str, ...]] = {
        "mc_allocation_test":    (),
        # Only launches a kernel that writes to scratch, despite the name:
        # it needs compute, not a doorbell.  It passes on Ampere and Ada.
        "mc_compute_dbell_demo": ("compute_launch",),
        "mc_dbell_demo":         ("ce_doorbell_ring",),
        "mc_features_demo":      (),
    }
    for binary, needs in oneshot_requires.items():
        cells.append(Cell(
            binary=binary,
            args=[],
            extra_check=_expect_no_fail_keyword,
            requires=needs,
        ))

    # ── stress: cross the 512-entry GPFIFO wrap ──
    # Two SM submissions per --h2d iter (timed + readback) → 600 iters
    # is ~1200 entries on the SM-author path, well past the wrap.
    if stress:
        for carrier_flag in ["--sysmem", "--fb"]:
            for direction in ["", "--h2d"]:
                args = [carrier_flag, "--size", "4K", "--iters", "600"]
                if direction:
                    args.append(direction)
                cells.append(Cell(
                    binary="mc_carrier_demo",
                    args=args,
                    timeout_s=120.0,
                    extra_check=_expect_pass_keyword,
                    requires=carrier_feature[carrier_flag],
                ))

    return cells


# ─── runner ───────────────────────────────────────────────────────────────────


@dataclass
class Result:
    cell: Cell
    rc: int
    duration_s: float
    stdout: str
    stderr: str
    bandwidth_gbps: float | None = None
    note: str = ""
    # Set when the cell was skipped without being run, because the GPU does
    # not offer a feature it declared in Cell.requires.  Names the missing
    # feature so the report says why, not just that.
    skip_reason: str = ""

    @property
    def skipped(self) -> bool:
        """The GPU or build cannot provide what this cell exercises.

        Not every supported architecture can do everything libmc implements:
        Ampere and Ada have no BAR1 usermode aperture, so no doorbell PTE, so
        no FB carrier and nothing SM-authored.  A demo that reports the
        capability as absent has not failed -- there is nothing there to
        test -- and counting it as a failure would make a correct Ampere run
        look broken.

        Two ways to land here.  `skip_reason` is the deliberate one: the cell
        declared a required feature the GPU does not have and was never run.
        The output heuristic below is the fallback, for a demo that discovers
        mid-run that something is unavailable and says so -- it predates the
        declared requirements and still covers cases too fine-grained to
        express per-cell.

        The heuristic is all-or-nothing, and that is a trap worth knowing
        about: it marks the WHOLE cell skipped on a substring match anywhere
        in the output.  A test that skips only part of its own work and still
        does real testing must therefore avoid these phrases, or its genuine
        coverage is silently discounted.  mc_allocation_test is exactly that
        case and words its per-pair skip line to dodge them.
        """
        if self.skip_reason:
            return True
        out = (self.stdout + self.stderr).lower()
        return ("unsupported" in out or "unavailable on this gpu" in out)

    @property
    def ok(self) -> bool:
        if self.skipped:
            return True
        if self.rc != 0:
            return False
        if self.cell.extra_check is None:
            return True
        return self.cell.extra_check(self.stdout + self.stderr)


# Matches both shapes the test binaries emit:
#   "  iter  3: H2D=  627.6 us (13.37 GB/s)"        ← carrier / dbell demos
#   "  Peak: 0.71 GB/s (0.01 ms)"                    ← mc_demo
_RE_BANDWIDTH = re.compile(r"([0-9]+\.[0-9]+)\s*GB/s")


def _peak_bandwidth(out: str) -> float | None:
    """Best bandwidth seen in the test's stdout, in GB/s."""
    matches = [float(m.group(1)) for m in _RE_BANDWIDTH.finditer(out)]
    return max(matches) if matches else None


def run_cell(cell: Cell, bin_dir: Path, device: int, verbose: bool) -> Result:
    bin_path = bin_dir / cell.binary
    if not bin_path.exists():
        return Result(cell, rc=127, duration_s=0.0, stdout="",
                      stderr=f"binary not found: {bin_path}",
                      note="missing")

    cmd: list[str] = [str(bin_path)]
    cmd.extend(device_args(device))
    cmd.extend(cell.args)

    if verbose:
        print(f"  $ {' '.join(shlex.quote(x) for x in cmd)}")

    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=cell.timeout_s,
        )
        rc = proc.returncode
        out = proc.stdout
        err = proc.stderr
    except subprocess.TimeoutExpired as exc:
        return Result(
            cell,
            rc=124,
            duration_s=cell.timeout_s,
            stdout=exc.stdout or "",
            stderr=(exc.stderr or "") + f"\n[TIMEOUT after {cell.timeout_s:.0f}s]",
            note="timeout",
        )

    duration = time.monotonic() - t0
    bw = _peak_bandwidth(out)
    return Result(cell, rc=rc, duration_s=duration,
                  stdout=out, stderr=err, bandwidth_gbps=bw)


# ─── reporting ────────────────────────────────────────────────────────────────


def fmt_result(r: Result) -> str:
    # A skipped cell is not a pass: reporting it as one hides how much of the
    # matrix a given GPU actually exercised, which is the number that matters
    # when comparing an Ampere run against a Hopper one.
    status = "SKIP" if r.skipped else ("PASS" if r.ok else "FAIL")
    bw = f" {r.bandwidth_gbps:5.2f} GB/s" if r.bandwidth_gbps is not None else ""
    note = r.note or r.skip_reason
    note = f" [{note}]" if note else ""
    return f"  [{status}] {r.duration_s:6.2f}s{bw}{note:>10}  {r.cell.label}"


def print_failure_detail(r: Result) -> None:
    print(f"\n--- FAIL: {r.cell.label} (rc={r.rc}) ---")
    if r.stdout.strip():
        print("  stdout (last 20 lines):")
        for line in r.stdout.rstrip().splitlines()[-20:]:
            print(f"    {line}")
    if r.stderr.strip():
        print("  stderr (last 20 lines):")
        for line in r.stderr.rstrip().splitlines()[-20:]:
            print(f"    {line}")


# ─── main ─────────────────────────────────────────────────────────────────────


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin-dir", default=None,
                    help="directory containing test binaries "
                         "(default: <repo>/bin)")
    ap.add_argument("--quick", action="store_true",
                    help="only run one size per test, no stress")
    ap.add_argument("--stress", action="store_true",
                    help="also run iters=600 to cross the GPFIFO 512 wrap")
    ap.add_argument("--filter", default=None,
                    help="only run tests whose binary basename matches this regex")
    ap.add_argument("--device", type=int, default=0, metavar="N",
                    help="which GPU to drive (/dev/nvidiaN); passed to the "
                         "feature query and every cell (default: 0)")
    ap.add_argument("--verbose", action="store_true",
                    help="print every command before running it")
    args = ap.parse_args(argv)

    if args.device < 0:
        print(f"error: --device must be >= 0, got {args.device}",
              file=sys.stderr)
        return 2

    bin_dir = Path(args.bin_dir) if args.bin_dir else repo_root() / "bin"
    if not bin_dir.is_dir():
        print(f"error: bin dir does not exist: {bin_dir}", file=sys.stderr)
        return 2


    cells = build_matrix(quick=args.quick, stress=args.stress)

    if args.filter:
        pat = re.compile(args.filter)
        cells = [c for c in cells if pat.search(c.binary)]
        if not cells:
            print(f"no tests matched --filter {args.filter!r}", file=sys.stderr)
            return 2

    # What this GPU can actually do.  None means the query failed, in which
    # case nothing is pre-skipped and every cell runs as it always did.
    available = query_features(bin_dir, args.device, verbose=args.verbose)

    print(f"running {len(cells)} cells from {bin_dir}")
    print(f"  gpu: /dev/nvidia{args.device}")
    if args.quick:   print("  (quick: one size per test)")
    if args.stress:  print("  (stress: includes iters=600 wrap test)")
    if available is not None:
        print(f"  gpu features: {' '.join(sorted(available)) or '(none)'}")
    print()

    results: list[Result] = []
    for cell in cells:
        missing = ()
        if available is not None:
            missing = tuple(f for f in cell.requires if f not in available)
        if missing:
            results.append(Result(
                cell, rc=0, duration_s=0.0, stdout="", stderr="",
                skip_reason="needs " + ", ".join(missing)))
        else:
            results.append(run_cell(cell, bin_dir, args.device,
                                    verbose=args.verbose))
        print(fmt_result(results[-1]), flush=True)

    # ── summary ──
    skipped = [r for r in results if r.skipped]
    passed  = [r for r in results if r.ok and not r.skipped]
    failed  = [r for r in results if not r.ok]

    print()
    skip_note = f", {len(skipped)} skipped (unsupported)" if skipped else ""
    print(f"summary: {len(passed)}/{len(results) - len(skipped)} passed, "
          f"{len(failed)} failed{skip_note}, "
          f"total wall {sum(r.duration_s for r in results):.1f}s")

    if failed:
        for r in failed:
            print_failure_detail(r)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
