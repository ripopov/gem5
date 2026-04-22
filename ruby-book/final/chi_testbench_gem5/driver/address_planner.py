# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
HNF-aware address layout helpers for the CHI-Garnet testbench.

Reproduces the interleaving logic from configs/ruby/CHI_config.py
CHI_HNF.createAddrRanges, which partitions the memory space across the
HNF slices by matching the low bits of the cache-line index.

Interleave layout for cache_line=64B and num_l3caches=N where N is a
power of two:

    bits [6 .. 6+log2(N)-1]  -> HNF index
    all higher bits          -> line offset within that HNF's stripe

So for cache_line=64 and 16 L3 slices (N=16, log2(N)=4):
    bits [6..9]              -> HNF index (0..15)
    bits [10..]              -> stride within that HNF
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import (
    Iterable,
    List,
    Tuple,
)


@dataclass(frozen=True)
class MeshLayout:
    """Key parameters that define the address map of the target system."""

    cache_line_size: int = 64
    num_hnfs: int = 16
    num_rows: int = 4
    num_cols: int = 4
    base_addr: int = 0  # low end of the system memory range

    def __post_init__(self) -> None:
        if self.num_hnfs & (self.num_hnfs - 1) != 0:
            raise ValueError(
                f"num_hnfs={self.num_hnfs} must be a power of two"
            )
        if self.num_rows * self.num_cols < self.num_hnfs:
            raise ValueError("mesh dimensions smaller than num_hnfs")

    @property
    def block_size_bits(self) -> int:
        return int(math.log2(self.cache_line_size))

    @property
    def llc_bits(self) -> int:
        return int(math.log2(self.num_hnfs))

    @property
    def stride_per_hnf(self) -> int:
        """Bytes between consecutive lines owned by the same HNF."""
        return self.cache_line_size << self.llc_bits


class AddressPlanner:
    """
    Compute addresses targeting specific HNF slices.

    All addresses returned are guaranteed to be cache-line aligned, to
    lie within the stripe owned by the requested HNF, and to be
    disjoint across distinct HNF indices.
    """

    def __init__(self, layout: MeshLayout | None = None) -> None:
        self.layout = layout or MeshLayout()

    # --- core primitives ---

    def address_for_hnf(self, hnf_idx: int, line_offset: int = 0) -> int:
        """Return the (line_offset)-th line homed at HNF `hnf_idx`."""
        self._check_hnf(hnf_idx)
        lyt = self.layout
        addr = (
            lyt.base_addr
            + (line_offset << (lyt.block_size_bits + lyt.llc_bits))
            + (hnf_idx << lyt.block_size_bits)
        )
        return addr

    def line_range(
        self, hnf_idx: int, num_lines: int, line_offset: int = 0
    ) -> list[int]:
        """Return `num_lines` consecutive HNF-homed cache-line addresses."""
        return [
            self.address_for_hnf(hnf_idx, line_offset + i)
            for i in range(num_lines)
        ]

    def adjacent_bytes(self, line_addr: int, n: int = 2) -> list[int]:
        """
        Return `n` byte-granular addresses inside the same cache line.
        Useful for false-sharing scenarios.
        """
        if n <= 0 or n > self.layout.cache_line_size:
            raise ValueError(f"n={n} out of range 1..cache_line_size")
        # Evenly space the offsets so writers don't overlap accidentally.
        step = max(1, self.layout.cache_line_size // n)
        return [line_addr + i * step for i in range(n)]

    # --- convenience helpers ---

    def one_line_per_hnf(self, line_offset: int = 0) -> list[int]:
        """16 addresses, one per HNF slice — default smoke-test payload."""
        return [
            self.address_for_hnf(i, line_offset)
            for i in range(self.layout.num_hnfs)
        ]

    def diagonal_pair(self) -> tuple[int, int]:
        """(HNF 0 base line, HNF (N-1) base line) — max-hop pair."""
        return (
            self.address_for_hnf(0, 0),
            self.address_for_hnf(self.layout.num_hnfs - 1, 0),
        )

    def hnf_of(self, addr: int) -> int:
        """Inverse map: which HNF slice is this address homed at?"""
        lyt = self.layout
        return (addr >> lyt.block_size_bits) & (lyt.num_hnfs - 1)

    # --- internal ---

    def _check_hnf(self, idx: int) -> None:
        if not 0 <= idx < self.layout.num_hnfs:
            raise ValueError(
                f"hnf_idx={idx} out of range 0..{self.layout.num_hnfs - 1}"
            )
