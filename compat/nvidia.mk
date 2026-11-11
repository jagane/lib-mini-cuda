# SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates.
# SPDX-License-Identifier: MIT

# The NVIDIA driver release this libmc revision is verified against on
# hardware.  Keep this value in lockstep with the open-gpu-kernel-modules
# submodule gitlink.  A build for another release must be requested with
# MC_ALLOW_UNTESTED_DRIVER=1 (see the Makefile); either way a binary runs only
# on the exact release it was compiled for.
MC_NVIDIA_VERSION := 610.43.02
