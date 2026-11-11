# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: MIT
#
# libmc Makefile — builds the library and its example artefacts:
#
#   bin/mc_demo          — D2H transfer on top of libmc, no CUDA library.
#   lib/libmc.so         — the mc library itself (see mc/include/mc/mc.h).
#
# New .c files dropped in tests/mc/ are picked up automatically.
#
# Quick start:
#
#   make                   — build libmc and every tests/mc/*.c binary.
#   make mc-all            — every tests/mc/*.c binary.
#   make mc_demo           — libmc demo (no CUDA toolchain needed).
#   make libmc             — library only.
#   make check-nvidia-source — validate the driver-source dependency.
#   make test-unit         — host-only unit tests (no GPU required).
#   make help              — print this summary.
#   make clean             — remove every produced artefact.
#
# Override toolchain or flags from the command line:
#
#   make CC=clang mc_demo
#   make NVCC=/opt/cuda/bin/nvcc kernels
#   make CFLAGS='-O0 -g -fsanitize=address' mc_demo

# ── Toolchain ──────────────────────────────────────────────────────────────
CC        ?= gcc
NVCC      ?= $(shell command -v nvcc      2>/dev/null || echo /usr/local/cuda/bin/nvcc)
CUOBJDUMP ?= $(shell command -v cuobjdump 2>/dev/null || echo /usr/local/cuda/bin/cuobjdump)
REPO_ROOT := $(CURDIR)

include compat/nvidia.mk

# libmc consumes private RM/UVM ABI definitions from the exact official
# driver release it is built for.  The submodule -- the release this revision
# is verified against, MC_NVIDIA_VERSION -- is the default.  NVIDIA_SRC may
# point at another checkout of that release, or, with
# MC_ALLOW_UNTESTED_DRIVER=1, at a different release: the build then targets
# that release (see MC_NV_CONFIG below), and the runtime version check
# requires it to be the one loaded.
NVIDIA_SRC ?= $(REPO_ROOT)/third_party/open-gpu-kernel-modules
NVIDIA_VERSION_MK := $(NVIDIA_SRC)/version.mk
MC_ALLOW_UNTESTED_DRIVER ?= 0

# ── Flags ──────────────────────────────────────────────────────────────────
# Warnings on, but allow unused helpers: the sources keep a few reference
# implementations around as documentation.
# libmc talks to the driver directly, so it needs the target driver's SDK and
# UVM headers.  tools/nvidia_conftest.sh probes them and writes MC_NV_CONFIG:
# MC_NVIDIA_DRIVER_VERSION, compiled into the strict runtime compatibility
# check performed before any RM object is allocated, and one MC_NV_HAVE_*
# macro per source-level difference between driver releases.  It is
# force-included, so everything compiled with MC_CPPFLAGS depends on it.
CFLAGS       ?= -O2 -g -Wall -Wno-unused-function
MC_NV_CONFIG  = $(BUILD_DIR)/mc_nv_config.h
MC_CPPFLAGS   = -include $(MC_NV_CONFIG)

# Include roots.  -Imc/include exposes only the public <mc/mc.h>; -Imc is the
# library-internal root, so a TU says #include "internal/types.h" or
# "arch/hopper/qmd_v04.h" and the layer it is reaching into is visible at the
# include site.  Consumers get MC_PUBLIC_INC alone and never see internal/.
MC_PUBLIC_INC := -Imc/include
MC_INC        := $(MC_PUBLIC_INC) -Imc
SDK_INC     := -I$(NVIDIA_SRC)/src/common/sdk/nvidia/inc \
                -I$(NVIDIA_SRC)/src/common/sdk/nvidia/inc/class \
                -I$(NVIDIA_SRC)/src/common/sdk/nvidia/inc/alloc \
                -I$(NVIDIA_SRC)/src/nvidia/arch/nvalloc/unix/include \
                -I$(NVIDIA_SRC)/src/common/inc \
                -I$(NVIDIA_SRC)/src/nvidia/inc \
                -I$(NVIDIA_SRC)/kernel-open/nvidia-uvm \
                -I$(NVIDIA_SRC)/kernel-open/common/inc

# ── Artefacts ──────────────────────────────────────────────────────────────
# Auto-discovered from source directories — add a file, it builds automatically.
# Auto-discover test programs.  Files ending in _helpers.c are
# shared-helper translation units (mc_test_helpers.c etc.), NOT
# stand-alone tests — exclude them from binary discovery and link
# them into every test instead.
MC_ALL_SRCS := $(wildcard tests/mc/*.c)
MC_HELPERS  := $(filter %_helpers.c,$(MC_ALL_SRCS))
MC_SRCS     := $(filter-out %_helpers.c,$(MC_ALL_SRCS))
MC_BINS     := $(patsubst tests/mc/%.c,bin/%,$(MC_SRCS))

# The library carries a SONAME, so the file on disk must BE that name: the
# loader resolves a binary's DT_NEEDED (libmc.so.0) against real files, and a
# lone lib/libmc.so would leave every demo failing at exec with
# "cannot open shared object file".  Ship the real object as the SONAME and
# keep the unversioned name as the development symlink linkers look for.
LIBMC_SONAME := libmc.so.0
LIBMC_REAL   := lib/$(LIBMC_SONAME)
LIBMC        := lib/libmc.so
LIBMC_MAP    := mc/libmc.map
# Experimental research paths.  On by default while the library IS a research
# vehicle; MC_ENABLE_EXPERIMENTAL=0 produces a build whose .so does not contain
# them and whose exported ABI is the ten stable functions plus the feature
# queries.  The flag is compiled into the feature engine too, so
# mc_feature_supported() reports the truth for the build in hand.
MC_ENABLE_EXPERIMENTAL ?= 1

# Architectures with a backend in this build: every directory under mc/arch/.
# Discovered rather than listed, so adding a backend really is adding a
# directory -- a hand-maintained list here would silently compile nothing for
# a new mc/arch/<name>/ and the omission would look like the backend not
# working.  Trimming it (MC_ARCHS=hopper) gives a leaner library and skips the
# cubin builds for the others.
MC_ARCHS ?= $(sort $(notdir $(patsubst %/,%,$(wildcard mc/arch/*/))))

# SM version each architecture's kernels are compiled for.  SASS is not
# portable across these, which is why every architecture needs its own cubin
# of every kernel rather than sharing one.
ARCH_SM_ampere       := sm_80
ARCH_SM_ampere_ga10x := sm_86
ARCH_SM_ada       := sm_89
ARCH_SM_hopper    := sm_90
ARCH_SM_blackwell := sm_100

# Sources, grouped by layer.  core/ and hal/ are architecture-neutral;
# driver/ wraps the RM and UVM ioctl surfaces; arch/<name>/ holds
# per-architecture code; experimental/ holds the research paths.
MC_CORE_SRCS := mc/core/context.c mc/core/alloc.c mc/core/channel.c \
                mc/core/feature.c \
                mc/core/memcpy.c mc/core/vaspace.c
MC_HAL_SRCS  := mc/hal/submit.c mc/hal/methods.c mc/hal/registry.c \
                mc/hal/detect.c mc/hal/cbuf.c
MC_DRV_SRCS  := mc/driver/rm.c mc/driver/uvm.c
# Some backends carry only a descriptor and share another architecture's
# implementation: Ada and GA10x both use Ampere's ops table (mc/arch/ampere/
# ops.c) and its QMD V03 builder (mc/arch/ampere/qmd_v03.c), because they
# allocate the same usermode and channel classes and use the same QMD version.
# Selecting one of them without ampere must still compile those, or the
# library links with mc_arch_ampere_ops and mc_arch_ampere_compute undefined
# -- which a shared library tolerates, so it builds cleanly and only fails
# when something links against it.
ARCH_SHARES_ada          := ampere
ARCH_SHARES_ampere_ga10x := ampere

# Directories contributing sources: the selected backends in full, plus the
# implementations they share.  A shared-but-unselected directory contributes
# everything EXCEPT its backend.c, so its descriptor is not linked and it gets
# no MC_HAVE_ARCH_ define -- sharing an implementation must not silently
# register a backend the caller did not ask for.
MC_ARCH_SHARED := $(filter-out $(MC_ARCHS),\
                    $(sort $(foreach a,$(MC_ARCHS),$(ARCH_SHARES_$(a)))))
MC_ARCH_SRCS := $(foreach a,$(MC_ARCHS),$(wildcard mc/arch/$(a)/*.c)) \
                $(foreach a,$(MC_ARCH_SHARED),\
                  $(filter-out mc/arch/$(a)/backend.c,$(wildcard mc/arch/$(a)/*.c)))
# One -D per enabled architecture, so hal/registry.c can compile in exactly
# the backends whose sources are present.  A missing define and a missing
# source would otherwise disagree, and the failure would be a link error
# rather than something the build explains.
MC_ARCH_DEFS := $(foreach a,$(MC_ARCHS),\
                  -DMC_HAVE_ARCH_$(shell echo $(a) | tr '[:lower:]' '[:upper:]')=1)
ifeq ($(MC_ENABLE_EXPERIMENTAL),1)
MC_EXP_SRCS  := mc/experimental/compute.c mc/experimental/sm_owner.c \
                mc/experimental/doorbell.c
else
MC_EXP_SRCS  :=
endif

MC_LIB_SRCS := $(MC_CORE_SRCS) $(MC_HAL_SRCS) $(MC_DRV_SRCS) \
               $(MC_ARCH_SRCS) $(MC_EXP_SRCS)

# Per-translation-unit objects.  The library used to be built by a single
# compiler invocation over every source; that does not scale as the tree grows
# (architecture backends, a HAL layer, split-up core), and it recompiles
# everything on any edit.
BUILD_DIR   := build
MC_LIB_OBJS := $(patsubst mc/%.c,$(BUILD_DIR)/%.o,$(MC_LIB_SRCS))

# Automatic header-dependency tracking.  Replaces the hand-maintained header
# prerequisite list on the link rule, which silently goes stale every time a
# new internal header appears.
DEPFLAGS := -MMD -MP

# Library-only compile flags.  MC_BUILDING_LIBMC turns on the MC_API
# visibility attribute in mc.h; -fvisibility=hidden makes every other symbol
# library-private.  See mc/libmc.map for the link-time half of the contract.
MC_LIB_CPPFLAGS := -DMC_BUILDING_LIBMC \
                   -DMC_ENABLE_EXPERIMENTAL=$(MC_ENABLE_EXPERIMENTAL) \
                   $(MC_ARCH_DEFS)
MC_LIB_CFLAGS   := -fPIC -fvisibility=hidden

# Unit tests are auto-discovered like tests/mc.  Each test may override the
# symbols it interposes and the library sources it links; the defaults suit a
# test that wraps ioctl and links the whole library.
UNIT_SRCS      := $(wildcard tests/unit/*.c)
UNIT_TEST_BINS := $(patsubst tests/unit/%.c,bin/%,$(UNIT_SRCS))
UNIT_WRAP_DEFAULT := ioctl
UNIT_SRCS_DEFAULT := $(MC_LIB_SRCS)

# Per-test overrides.  A test with no entry here gets the defaults above.
UNIT_WRAP_mc_rm_version_test    := ioctl
UNIT_SRCS_mc_rm_version_test    := mc/driver/rm.c
UNIT_WRAP_mc_lce_selection_test := ioctl
UNIT_SRCS_mc_lce_selection_test := mc/driver/rm.c
UNIT_WRAP_mc_init_version_test  := open ioctl close
UNIT_WRAP_mc_channel_alloc_params_test := ioctl
UNIT_SRCS_mc_channel_alloc_params_test := mc/driver/rm.c
# Pure encoding test: links only the emitters, interposes nothing.
# Pure encoding / pure data: link the backend (and what it pulls in) but
# interpose nothing -- neither test issues an ioctl.
# Linking the Hopper descriptor pulls in the tables it points at, and those
# reach rm_alloc.  One variable, so adding a field to the descriptor does not
# mean editing every test that touches it.
# Every backend the build enables, plus what their descriptors reach into.
# Tests that touch the registry link this rather than naming one architecture,
# so adding a backend does not mean editing the tests.
ALL_BACKEND_SRCS := $(MC_ARCH_SRCS) mc/driver/rm.c

UNIT_WRAP_mc_methods_test       := none
UNIT_SRCS_mc_methods_test       := mc/hal/methods.c $(ALL_BACKEND_SRCS)
UNIT_WRAP_mc_feature_test       := none
UNIT_SRCS_mc_feature_test       := mc/core/feature.c mc/driver/rm.c
UNIT_WRAP_mc_arch_desc_test     := none
UNIT_SRCS_mc_arch_desc_test     := $(ALL_BACKEND_SRCS) mc/hal/registry.c
UNIT_SRCS_mc_arch_detect_test   := mc/hal/detect.c mc/hal/registry.c \
                                   $(ALL_BACKEND_SRCS)
UNIT_WRAP_mc_qmd_test           := none
UNIT_SRCS_mc_qmd_test           := mc/arch/ampere/qmd_v03.c \
                                   mc/arch/hopper/qmd_v04.c
UNIT_WRAP_mc_userd_addr_test    := none
UNIT_SRCS_mc_userd_addr_test    := $(ALL_BACKEND_SRCS) mc/hal/registry.c

# Resolve a test's wrap set / source list, falling back to the defaults.
# `none` means "interpose nothing": a test that links driver code but never
# calls into it still needs --wrap suppressed, or the linker demands a
# __wrap_ioctl the test has no reason to define.
unit_wrap = $(if $(UNIT_WRAP_$(1)),$(filter-out none,$(UNIT_WRAP_$(1))),$(UNIT_WRAP_DEFAULT))
unit_srcs = $(if $(UNIT_SRCS_$(1)),$(UNIT_SRCS_$(1)),$(UNIT_SRCS_DEFAULT))

BINS := $(MC_BINS)
LIBS := $(LIBMC) $(LIBMC_REAL)

.DEFAULT_GOAL := all
.PHONY: all bins libs mc-all clean help libmc
.PHONY: check-nvidia-source test-unit check-conftest mc-nv-force
.PHONY: $(notdir $(BINS))

all: $(BINS) $(LIBS)
bins: $(BINS)
libs: $(LIBS)
mc-all: $(MC_BINS)

# Friendly aliases without the directory prefix (auto-generated).
$(foreach b,$(BINS),$(eval $(notdir $b): $b))

libmc: $(LIBMC)
# `|| exit $$?` — a bare `exit` would return the shell's last status rather
# than the failing test's, which can mask a failure as success.
test-unit: $(UNIT_TEST_BINS) check-conftest
	@for test_bin in $(UNIT_TEST_BINS); do ./$$test_bin || exit $$?; done

# The probe generator, tested against synthetic driver trees (no GPU, no
# second driver release, no network).
check-conftest:
	@bash tools/test_nvidia_conftest.sh $(CC)

# Probed on every make (a few syntax-only compiles).  The header is rewritten,
# and the library rebuilt, only when the driver tree changes.
$(MC_NV_CONFIG): tools/nvidia_conftest.sh mc-nv-force | check-nvidia-source
	@bash tools/nvidia_conftest.sh "$(NVIDIA_SRC)" $@ $(CC) $(SDK_INC)

# Refuse to compile against a missing private driver ABI, or against a release
# other than the verified one unless that is asked for explicitly.  The runtime
# repeats the check against the loaded nvidia.ko before initialization.
check-nvidia-source:
	@if [ ! -f "$(NVIDIA_VERSION_MK)" ]; then \
	  echo "ERROR: NVIDIA driver sources not found at $(NVIDIA_SRC)" >&2; \
	  echo "From the repository root, run:" >&2; \
	  echo "  git submodule update --init --depth 1 third_party/open-gpu-kernel-modules" >&2; \
	  exit 1; \
	fi
	@actual=$$(sed -n 's/^NVIDIA_VERSION[[:space:]]*=[[:space:]]*//p' \
	                    "$(NVIDIA_VERSION_MK)" | head -n 1); \
	if [ "$$actual" != "$(MC_NVIDIA_VERSION)" ]; then \
	  if [ "$(MC_ALLOW_UNTESTED_DRIVER)" = "1" ] && [ -n "$$actual" ]; then \
	    echo "WARNING: building for NVIDIA $$actual; this revision is verified" >&2; \
	    echo "         only against $(MC_NVIDIA_VERSION)." >&2; \
	  else \
	    echo "ERROR: libmc is verified against NVIDIA $(MC_NVIDIA_VERSION)," >&2; \
	    echo "       but NVIDIA_SRC contains $${actual:-an unknown version}." >&2; \
	    echo "       To build for that release anyway, add MC_ALLOW_UNTESTED_DRIVER=1." >&2; \
	    exit 1; \
	  fi; \
	fi

# ── Output directories (order-only prereqs so parallel builds don't race) ─
bin lib $(BUILD_DIR):
	mkdir -p $@

# ── Build rules ────────────────────────────────────────────────────────────

# Generic rule for every tests/mc/*.c — links libmc
# and pulls in the test-side helper translation units (mc_test_helpers.c
# etc.).  No SDK headers needed; just public mc.h + the helper
# headers.  Future tests added to tests/mc/ pick up the helpers
# automatically.
bin/%: tests/mc/%.c $(MC_HELPERS) $(LIBMC) | bin
	$(CC) $(CFLAGS) $(MC_PUBLIC_INC) -Itests/mc \
	    -DMC_ENABLE_EXPERIMENTAL=$(MC_ENABLE_EXPERIMENTAL) -o $@ \
	    $< $(MC_HELPERS) -L./lib -lmc \
	    -Wl,-rpath,'$$ORIGIN/../lib'

# ── Device kernels: .cu -> .cubin -> .text bytes -> generated header ───────
#
# libmc embeds two device kernels as byte arrays:
#
#   sm_owner   a single SM thread authors an entire CE-channel submission --
#              pushbuffer methods, GPFIFO entry, USERD GPPut, BAR1 doorbell.
#   doorbell   one thread, one system-scope store.  The minimal demonstration
#              that an SM store can ring a doorbell and wake PBDMA.
#
# Both follow the same pipeline, so it is written once here and instantiated
# per kernel.  The generated headers are committed so a fresh checkout builds
# without nvcc; regenerating them is `make kernels`.  Reviewers see the SASS
# bytes diff in git, which is the only honest review surface for hand-managed
# machine code.
#
# Per-kernel knobs:
#   KERNEL_OPT_<name>      nvcc optimisation level.
#   KERNEL_ASSERTS_<name>  space-separated REGEX:MINIMUM pairs.  Each is
#                          grepped against the disassembly and the build fails
#                          if the mnemonic appears fewer than MINIMUM times.
#
# The assertions are the load-bearing part.  What matters about these kernels
# is not what they compute but the SCOPE and ORDERING of their stores, and
# that is a property of nvcc's lowering rather than anything the source can
# guarantee.  A toolkit that weakened STG.E.STRONG.SYS to a plain store, or
# coalesced the pushbuffer writes, would produce a kernel that still compiles
# and still looks right but lets PBDMA observe the pushbuffer, GPFIFO entry
# and USERD update out of order.  Failing the build is much cheaper than
# discovering that as an intermittent hang.
#
# The mnemonics below were established by running `cuobjdump --dump-sass` on
# real output rather than assumed.  Measured with CUDA 13.2 on 2026-08-26,
# across every SM version libmc targets:
#
#            sm_80    sm_89    sm_90    sm_100
#   sm_owner   STG.E.STRONG.SYS       18       18       18       18
#              STG.E.64.STRONG.SYS     1        1        1        1
#              LDG.E.STRONG.GPU        1        1        1        1
#              MEMBAR.SC.SYS           6        6        6        6
#              REG                    32       38       32        32
#   doorbell   STG.E.STRONG.SYS        1        1        1         1
#
# So the spellings and the counts are the same everywhere, and the thresholds
# below hold unchanged for every architecture -- which is why
# KERNEL_ASSERTS_<kernel>_<arch> exists but is unused.  Two things follow.
# First, sm_owner.cu is NOT sm_90-only: the system-scope store scope it
# depends on lowers identically as far back as sm_80.  Second, the register
# count is NOT constant -- Ada needs 38 where the others need 32 -- which is
# why the QMD's register count comes from the per-architecture kernel blob
# rather than a shared constant.
#
# A future toolkit may lower differently.  Re-measure rather than assume, and
# update this table with what you saw.

KERNELS := sm_owner store_dword

# sm_owner: -O2 so nvcc folds the DRF-derived constants into immediates.
KERNEL_OPT_sm_owner     := -O2
# >= 19 system-scope strong stores: 16 pushbuffer method dwords + the 64-bit
# GPFIFO entry + USERD GPPut + the doorbell.  >= 1 GPU-scope strong load for
# the optional on-GPU semaphore poll (an earlier .relaxed.sys lowering became
# LDG.E.STRONG.SYS and spun on stale L1 forever).  >= 1 system-scope membar
# from __threadfence_system().
KERNEL_ASSERTS_sm_owner := 'STG\.E(\.[0-9]+)?\.STRONG\.SYS:19' \
                           'LDG\.E(\.[0-9]+)?\.STRONG\.GPU:1' \
                           'MEMBAR\.SC\.SYS:1'

# store_dword: -O0.  Nothing to fold, and it keeps the SASS a literal
# transcription of the source, which is what makes the bytes reviewable.
KERNEL_OPT_store_dword     := -O0
# The single store must reach system scope, or it sits in L1: a doorbell would
# never be rung, and a store to host-visible memory would never be observed.
KERNEL_ASSERTS_store_dword := 'STG\.E(\.[0-9]+)?\.STRONG\.SYS:1'

NVCC_KERNEL_BASE ?= --cubin -lineinfo -Imc

# $(1) = kernel name, $(2) = architecture name
define KERNEL_RULE
mc/arch/$(2)/$(1)_sass.h: mc/kernels/$(1).cu
	@echo "  NVCC    mc/kernels/$(1).cu (-> cubin)"
	@test -n "$$(ARCH_SM_$(2))" || { \
	  echo "ERROR: no ARCH_SM_$(2); add the SM version for $(2)" >&2; \
	  exit 1; }
	$$(NVCC) -arch=$$(ARCH_SM_$(2)) $$(NVCC_KERNEL_BASE) $$(KERNEL_OPT_$(1)) \
	    -o mc/kernels/$(1).cubin $$<
	@echo "  EXTRACT .text.mc_$(1)_kernel"
	@# objcopy's BFD has no backend for NVIDIA CUDA ELFs (e_machine = 190),
	@# so parse the section table with readelf -SW and dd the bytes out by
	@# file offset/size.  Columns 5/6 are hex, no 0x prefix.
	@sec=$$$$(readelf -SW mc/kernels/$(1).cubin 2>/dev/null                 \
	         | awk '/\.text\.[a-z_]*$(1)_kernel/ {print $$$$5" "$$$$6; exit}'); \
	 OFF=$$$${sec%% *}; SZ=$$$${sec##* };                                   \
	 if [ -z "$$$$OFF" ] || [ -z "$$$$SZ" ]; then                           \
	   echo "ERROR: no .text section for $(1)_kernel in the cubin" >&2;     \
	   exit 1;                                                              \
	 fi;                                                                    \
	 dd if=mc/kernels/$(1).cubin of=mc/kernels/$(1).text.bin                \
	    bs=1 skip=$$$$((0x$$$$OFF)) count=$$$$((0x$$$$SZ)) status=none
	@for spec in $$(if $$(KERNEL_ASSERTS_$(1)_$(2)),\
	                  $$(KERNEL_ASSERTS_$(1)_$(2)),\
	                  $$(KERNEL_ASSERTS_$(1))); do                                \
	   re=$$$${spec%:*}; min=$$$${spec##*:};                                \
	   n=$$$$($$(CUOBJDUMP) --dump-sass mc/kernels/$(1).cubin               \
	          | grep -cE "$$$$re" || true);                                 \
	   if [ "$$$$n" -lt "$$$$min" ]; then                                   \
	     echo "ERROR: $(1) cubin has $$$$n instruction(s) matching"         \
	          "'$$$$re'; expected >= $$$$min.  NVCC may have weakened or"   \
	          "coalesced a store -- inspect with"                           \
	          "cuobjdump --dump-sass mc/kernels/$(1).cubin" >&2;            \
	     exit 1;                                                            \
	   fi;                                                                  \
	   echo "  ASSERT  $(2)/$(1): $$$$n x $$$$re (>= $$$$min)";                  \
	 done
	@echo "  GEN     $$@"
	@u=$$$$(echo $(1) | tr 'a-z' 'A-Z');                                    \
	(  echo '/*';                                                           \
	   echo ' * SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.'; \
	   echo ' * SPDX-License-Identifier: MIT';                              \
	   echo ' *';                                                           \
	   echo " * $(1)_sass.h — auto-generated by the libmc Makefile";        \
	   echo ' *';                                                           \
	   echo ' * Source: mc/kernels/$(1).cu';                                \
	   echo " * Compiled with: nvcc -arch=$$(ARCH_SM_$(2)) $$(NVCC_KERNEL_BASE) $$(KERNEL_OPT_$(1))"; \
	   echo ' *';                                                           \
	   echo ' * DO NOT EDIT BY HAND.  Modify the .cu and run `make kernels`.'; \
	   echo ' */';                                                          \
	   echo "#ifndef MC_$$$${u}_SASS_H";                                    \
	   echo "#define MC_$$$${u}_SASS_H";                                    \
	   echo '';                                                             \
	   echo '#include <stdint.h>';                                          \
	   echo '';                                                             \
	   size=$$$$(stat -c '%s' mc/kernels/$(1).text.bin);                    \
	   regs=$$$$($$(CUOBJDUMP) --dump-resource-usage mc/kernels/$(1).cubin  \
	           2>&1 | sed -n 's/.*REG:\([0-9]*\).*/\1/p' | head -1);        \
	   echo "/* SASS byte length and register count, harvested from the cubin. */"; \
	   echo "#define MC_$$$${u}_SASS_LEN ((uint32_t)$$$$size)";             \
	   echo "#define MC_$$$${u}_REGS     ((uint32_t)$$$$regs)";             \
	   echo '';                                                             \
	   echo "static const uint8_t mc_$(1)_kernel_sass[] = {";               \
	   xxd -i < mc/kernels/$(1).text.bin;                                   \
	   echo "};";                                                           \
	   echo '';                                                             \
	   echo "#endif /* MC_$$$${u}_SASS_H */";                               \
	) > $$@
	@rm -f mc/kernels/$(1).text.bin
	@echo "  OK      $$@ ($$$$(stat -c '%s' $$@) bytes)"
endef
$(foreach a,$(MC_ARCHS),\
  $(foreach k,$(KERNELS),$(eval $(call KERNEL_RULE,$k,$a))))

.PHONY: kernels sm_owner
kernels: $(foreach a,$(MC_ARCHS),\
           $(foreach k,$(KERNELS),mc/arch/$(a)/$(k)_sass.h))
# Back-compat alias.
sm_owner: mc/arch/hopper/sm_owner_sass.h

# libmc: the ioctl + submission machinery behind a small
# CUDA-runtime-like public API (see mc/include/mc/mc.h).  Uses SDK_INC for the pinned SDK headers;
# compiled -fPIC and linked -shared.  mc/internal/*.h is a prereq
# only for rebuild tracking — public consumers include mc.h.
#
# Sources, one TU per concern:
#   driver/rm.c              RM ioctl wrappers (alloc/free/map/control + typed
#                            object allocators).  Hosts mc_debug() too.
#   driver/uvm.c             UVM ioctl wrappers (initialize, register/unregister,
#                            map/unmap external allocation, register channel).
#   hal/methods.c            every GPU method word libmc emits (CE copy
#                            stream, GPFIFO entries, compute setup/launch).
#                            Pure encoding: no fds, handles or fences.
#                            Includes hal/method_abi.h, whose _Static_assert
#                            bank proves the encodings are shared across the
#                            Ampere/Ada/Hopper/Blackwell classes.
#   hal/registry.c           the compiled-in backend table + pure lookups.
#   hal/detect.c             identify the GPU and select its backend, then
#                            verify the device exposes that backend's classes.
#   hal/submit.c             submission mechanics: produce -> fence -> publish,
#                            USERD accessors, doorbell, semaphore polling,
#                            drain.  The only TU naming the USERD struct.
#   core/context.c           mc_init / mc_fini.
#   core/channel.c           per-kind channel bring-up and teardown.
#   core/alloc.c             allocation table + mc_malloc_* / mc_free /
#                            mc_host_register / mc_gpu_va.
#   core/memcpy.c            mc_memcpy and its VAS/agent dispatch.
#   core/vaspace.c           VA pool (Paper-F1 anchor) + mc_va_space_* helpers.
#   arch/hopper/backend.c    the Hopper architecture descriptor: class ids,
#                            layout offsets, capability bits.  Pure data.
#   arch/hopper/ops.c        Hopper behaviour hooks: usermode allocation,
#                            USERD accessors, engine policy.
#   arch/hopper/qmd_v04.c    Hopper QMD V04 builder + embedded SASS.
#   experimental/compute.c   HOPPER_COMPUTE_A demos (mc_compute_doorbell_kernel,
#                            mc_memcpy_gpu_doorbell_sm, mc_compute_get_scratch).
#   experimental/doorbell.c  host-authored CE-doorbell demos
#                            (mc_dbell_demo_ring, mc_memcpy_gpu_doorbell_ce).
#   experimental/sm_owner.c  SM-authored CE submission helper.  Library-internal
#                            (declared in internal/compute.h); reached through
#                            the public API via mc_memcpy(..., MC_XFER_SM).
#                            Embeds arch/hopper/sm_owner_sass.h.
# Per-TU compile.  Header prerequisites come from the generated .d files
# (DEPFLAGS), so adding an internal header needs no Makefile edit.
$(BUILD_DIR)/%.o: mc/%.c compat/nvidia.mk $(MC_NV_CONFIG) | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(MC_CPPFLAGS) $(MC_LIB_CPPFLAGS) \
	    $(MC_LIB_CFLAGS) $(SDK_INC) $(MC_INC) -c -o $@ $<

# The version script is the authority on what libmc exports; mc/include/mc/mc.h's MC_API
# markers must list the same set.  Without it, every non-static symbol in every
# TU lands in the public ABI.
$(LIBMC_REAL): $(MC_LIB_OBJS) $(LIBMC_MAP) | lib
	$(CC) $(CFLAGS) $(MC_LIB_CFLAGS) -shared -o $@ $(MC_LIB_OBJS) \
	    -Wl,--version-script,$(LIBMC_MAP) -Wl,-soname,$(LIBMC_SONAME)

# Development symlink: `-lmc` resolves through this, while the DT_NEEDED
# recorded in every binary points at $(LIBMC_SONAME).
$(LIBMC): $(LIBMC_REAL)
	ln -sf $(LIBMC_SONAME) $@

# Generic unit-test rule.  Each test links the library sources it needs
# directly (not the .so) and interposes libc entry points with --wrap, so the
# whole suite runs without a GPU, without root, and without /dev/nvidia*.
# Adding tests/unit/foo.c is enough to get bin/foo built and run.
.SECONDEXPANSION:
bin/%: tests/unit/%.c $$(call unit_srcs,$$*) compat/nvidia.mk $(MC_NV_CONFIG) \
       | bin
	$(CC) $(CFLAGS) $(MC_CPPFLAGS) $(MC_LIB_CPPFLAGS) $(SDK_INC) $(MC_INC) \
	    -o $@ $< $(call unit_srcs,$*) \
	    $(foreach w,$(call unit_wrap,$*),-Wl,--wrap=$(w))

# Guard the export contract: the MC_API markers in mc/include/mc/mc.h, the version script,
# and the symbols actually exported by the .so must all agree.  Losing an
# MC_API silently un-exports a public function; forgetting `local: *` silently
# publishes every internal helper.  Neither shows up as a build failure, so
# check it explicitly.
.PHONY: check-abi
check-abi: $(LIBMC)
	@nm -D --defined-only $(LIBMC) | awk '{print $$3}' | sed 's/@@.*//' \
	    | grep '^mc_' | sort -u > $(BUILD_DIR)/.abi-actual
	@{ grep -h '^MC_API' mc/include/mc/mc.h;                                  \
	   [ "$(MC_ENABLE_EXPERIMENTAL)" = "1" ]                                  \
	     && grep -h '^MC_API' mc/include/mc/mc_experimental.h;                \
	   true; }                                                                \
	    | sed -E 's/.*[ *](mc_[a-z_]+)\(.*/\1/' | sort -u > $(BUILD_DIR)/.abi-expected
	@if ! diff -u $(BUILD_DIR)/.abi-expected $(BUILD_DIR)/.abi-actual; then \
	  echo "ERROR: exported ABI does not match the MC_API set in the public headers." >&2; \
	  echo "       Reconcile the headers, mc/libmc.map, and the definitions." >&2; \
	  exit 1;                                                                 \
	fi
	@echo "  OK      ABI: $$(wc -l < $(BUILD_DIR)/.abi-actual) exported symbols match the public headers"

# Guard that every built binary can actually resolve its libraries at exec
# time.  A build+link success does not imply this: the SONAME recorded in
# libmc is what binaries record as DT_NEEDED, so shipping the library under a
# different filename links fine and then fails at exec with "cannot open
# shared object file".  That is invisible to a build-only check and to
# check-abi, and on a host without a GPU it never gets exercised.
.PHONY: check-link
check-link: $(MC_BINS) $(LIBMC)
	@fail=0; \
	undef=$$(ldd -r $(LIBMC) 2>&1 | grep "undefined symbol" || true); \
	if [ -n "$$undef" ]; then \
	  echo "ERROR: $(LIBMC) has undefined symbols:" >&2; \
	  echo "$$undef" >&2; fail=1; \
	fi; \
	for b in $(MC_BINS); do \
	  missing=$$(ldd -r $$b 2>/dev/null | grep "not found" || true); \
	  if [ -n "$$missing" ]; then \
	    echo "ERROR: $$b has unresolved libraries:" >&2; \
	    echo "$$missing" >&2; fail=1; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then exit 1; fi
	@echo "  OK      link: $(LIBMC) + $(words $(MC_BINS)) binaries fully resolved"

# Every backend must build on its own.  MC_ARCHS exists so a deployment can
# carry one architecture, and a backend that shares another's implementation
# (see ARCH_SHARES_*) breaks that silently: a shared library tolerates
# undefined symbols, so the trimmed build succeeds and only fails when
# something links against it.  Ada and GA10x did exactly that.
#
# Builds each architecture alone into a scratch tree and asserts the resulting
# library has no undefined mc_* symbol.  Not part of the default gate -- it is
# one full library build per architecture -- but cheap enough to run before
# adding or resharing a backend.
.PHONY: check-arch-solo
check-arch-solo:
	@rc=0; \
	for a in $(MC_ARCHS); do \
	  d=build/solo/$$a; rm -rf $$d; mkdir -p $$d; \
	  if $(MAKE) --no-print-directory MC_ARCHS=$$a BUILD_DIR=$$d/obj \
	       LIBMC_REAL=$$d/libmc.so.0 LIBMC=$$d/libmc.so \
	       $$d/libmc.so.0 >$$d/build.log 2>&1; then \
	    u=$$(nm -D --undefined-only $$d/libmc.so.0 2>/dev/null \
	         | grep " mc_" || true); \
	    if [ -n "$$u" ]; then \
	      echo "  FAIL    MC_ARCHS=$$a: library has undefined mc_* symbols:" >&2; \
	      echo "$$u" | sed 's/^/            /' >&2; \
	      echo "          add its shared implementation to ARCH_SHARES_$$a" >&2; \
	      rc=1; \
	    else \
	      echo "  OK      MC_ARCHS=$$a builds standalone"; \
	    fi; \
	  else \
	    echo "  FAIL    MC_ARCHS=$$a did not build; see $$d/build.log" >&2; \
	    rc=1; \
	  fi; \
	done; \
	exit $$rc

# ── Utility ────────────────────────────────────────────────────────────────
clean:
	rm -rf bin lib $(BUILD_DIR)

# Header dependencies generated by DEPFLAGS.  Absent on a clean tree, which is
# why this is `-include`.
-include $(MC_LIB_OBJS:.o=.d)

help:
	@echo "Targets:"
	@echo "  all              build everything (bins + libs)"
	@echo "  mc-all           build every tests/mc/*.c binary"
	@echo "  bins             build everything under bin/"
	@echo "  libs             build everything under lib/"
	@echo "  libmc            build $(LIBMC) — the mc library"
	@echo "  check-nvidia-source  validate NVIDIA_SRC (verified release: $(MC_NVIDIA_VERSION))"
	@echo "  check-conftest   test the driver feature-probe generator"
	@echo "  test-unit        run host-only unit tests (auto-discovered)"
	@echo "  check-arch-solo  build each MC_ARCHS backend on its own"
	@echo "  check-abi        verify exported symbols match mc.h's MC_API set"
	@echo "  kernels          regenerate the embedded device-kernel SASS headers"
	@echo "  clean            remove bin/ and lib/ directories"
	@echo "  help             this message"
	@echo
	@echo "Discovered test binaries (mc-all):"
	@$(foreach b,$(MC_BINS),echo "  $(notdir $b)";)
	@echo
	@echo "Each binary can also be built by name, e.g. 'make mc_demo'."
	@echo
	@echo "Overridable variables:"
	@echo "  CC         C compiler          (default: $(CC))"
	@echo "  CFLAGS     C compile flags     (default: $(CFLAGS))"
	@echo "  NVCC       CUDA compiler used only by sm_owner (default: $(NVCC))"
	@echo "  CUOBJDUMP  SASS inspection tool (default: $(CUOBJDUMP))"
	@echo "  NVIDIA_SRC matching driver tree (default: pinned submodule)"
	@echo "  MC_ALLOW_UNTESTED_DRIVER  1 = allow an NVIDIA_SRC other than $(MC_NVIDIA_VERSION)"
