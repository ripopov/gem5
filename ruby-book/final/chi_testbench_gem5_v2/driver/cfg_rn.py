# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
Per-tile CHI nodes for the v2 direct-injection testbench.

Each tile in v2 contains TWO CHI network nodes on the same mesh
router:

    ChiDriverNode (synthetic upstream RN)
          │  CHI msgs (reqOut/snpOut/rspOut/datOut)
          ▼
      Mesh Router  ◀──  CHI_TileCacheController_v2 (RN-F or RN-I)
          ▲                (reqIn/snpIn/rspIn/datIn, etc.)
          │
      (to mesh / HN-Fs)

The `CHI_Tile_v2` wrapper inherits from `CHI_config.CHI_RNI_DMA` for
the same reason as in v1: `CustomMesh.makeTopology` buckets by
`isinstance` and CHI_RNI_DMA is the least-constrained classifier, so
both controllers end up connected to the same router via ExtLink. No
side-router is created (that's only for CHI_RNF).

`CHI_TileCacheController_v2` mirrors the shape of v1's
`CHI_TileCacheController` (same rnf_l2 / rni parameter set) but does
not require a sequencer — the driver replaces the sequencer as the
upstream stimulus, so `sequencer = NULL` is hard-coded.

MachineID allocation: both the tile and the driver register as
`MachineType_Cache` in Ruby. They share the `Versions.getVersion(
CHI_Cache_Controller)` counter so every MachineID is unique within the
Cache type (HNFs use the same pool).
"""

from __future__ import annotations

from ruby import CHI_config

from m5.objects import (
    NULL,
    CHI_Cache_Controller,
    RubyCache,
)


class CHI_TileCacheController_v2(CHI_config.Base_CHI_Cache_Controller):
    """
    v2 tile cache controller: no sequencer (the driver is the only
    upstream stimulus). `mode="rni"` → cache-less DMA-style pass-
    through; `mode="rnf_l2"` → coherent L2-class leaf cache.

    Parameters mirror v1's `CHI_TileCacheController`.
    """

    def __init__(
        self,
        ruby_system,
        mode,
        l2_size="256KiB",
        l2_assoc=8,
    ):
        super().__init__(ruby_system)
        self.sequencer = NULL

        self.is_HN = False
        self.enable_DMT = False
        self.enable_DCT = False
        self.prefetcher = NULL
        self.use_prefetcher = False
        self.number_of_TBEs = 32
        self.number_of_repl_TBEs = 16
        self.number_of_snoop_TBEs = 16
        self.number_of_DVM_TBEs = 1
        self.number_of_DVM_snoop_TBEs = 1
        self.unify_repl_TBEs = False

        if mode == "rni":

            class DummyCache(RubyCache):
                dataAccessLatency = 0
                tagAccessLatency = 1
                size = "128"
                assoc = 1

            self.cache = DummyCache()
            self.allow_SD = False
            self.send_evictions = False
            self.alloc_on_seq_acc = False
            self.alloc_on_seq_line_write = False
            self.alloc_on_readshared = False
            self.alloc_on_readunique = False
            self.alloc_on_readonce = False
            self.alloc_on_writeback = False
            self.alloc_on_atomic = False
            self.dealloc_on_unique = False
            self.dealloc_on_shared = False
            self.dealloc_backinv_unique = False
            self.dealloc_backinv_shared = False
        elif mode == "rnf_l2":
            self.cache = RubyCache(
                size=l2_size,
                assoc=l2_assoc,
                dataAccessLatency=6,
                tagAccessLatency=2,
            )
            self.allow_SD = True
            # Evictions notify the sequencer (for LL/SC monitor clears
            # and similar CPU-side plumbing). v2 has no sequencer, so
            # this callback would nullptr. Keep it off.
            self.send_evictions = False
            self.alloc_on_seq_acc = True
            self.alloc_on_seq_line_write = False
            self.alloc_on_readshared = True
            self.alloc_on_readunique = True
            self.alloc_on_readonce = True
            self.alloc_on_writeback = True
            self.alloc_on_atomic = False
            self.dealloc_on_unique = False
            self.dealloc_on_shared = False
            self.dealloc_backinv_unique = True
            self.dealloc_backinv_shared = True
        else:
            raise ValueError(
                f"CHI_TileCacheController_v2: unknown mode {mode!r} "
                "(expected 'rni' or 'rnf_l2')"
            )


class CHI_Tile_v2(CHI_config.CHI_RNI_DMA):
    """
    Direct-mesh tile: one cache controller + one colocated driver.

    Both controllers ExtLink to the same mesh router because the node
    is classified as `CHI_RNI_DMA` by `CustomMesh` and its
    `getNetworkSideControllers()` returns both.

    Construction order:
      1. Build the tile cache controller.
      2. Accept the pre-built ChiDriverNode from the caller (the
         scenario module builds it with sequence/params already set).
      3. Wire both controllers' eight CHI MessageBuffers to the mesh
         via `connectController`.
      4. Point the driver's `downstream_destinations` at the tile
         controller — that's where its CHI requests go.
    """

    class NoC_Params(CHI_config.CHI_RNI_DMA.NoC_Params):
        router_list = list(range(16))

    def __init__(self, ruby_system, driver, mode):
        CHI_config.CHI_Node.__init__(self, ruby_system)

        self._tile_cntrl = CHI_TileCacheController_v2(ruby_system, mode)

        # Driver picks its own version out of the shared CHI_Cache
        # counter so its MachineID is unique within MachineType_Cache.
        driver.version = CHI_config.Versions.getVersion(CHI_Cache_Controller)
        driver.ruby_system = ruby_system

        self.tile_cntrl = self._tile_cntrl
        # Keep the driver as a reference only (it's already parented to
        # system.cpu); assigning it as `self.driver = driver` would try
        # to reparent it under the tile and warn about orphan/parent.
        self._driver = driver

        self.connectController(self._tile_cntrl)
        self.connectController(driver)

        # Driver's default target: the colocated tile cache controller.
        driver.downstream_destinations = [self._tile_cntrl]

    def getSequencers(self):
        # No RubySequencer in v2; the driver replaces the sequencer.
        return []

    def getAllControllers(self):
        return [self._tile_cntrl, self._driver]

    def getNetworkSideControllers(self):
        return [self._tile_cntrl, self._driver]

    def setDownstream(self, cntrls):
        # Called by CHI.py with hnf_dests. Only the tile controller
        # forwards to HN-Fs; the driver's downstream (set in __init__)
        # is the tile controller and must not be overwritten.
        self._tile_cntrl.downstream_destinations = cntrls

    @classmethod
    def make_generator(cls, mode):
        """
        Returns a callable compatible with the `system._rnf_gen` hook
        in `configs/ruby/CHI.py:125`. Expects each entry of `cpus` to
        already be a ChiDriverNode (scenario modules build them).
        """

        def generate(options, ruby_system, cpus):
            return [cls(ruby_system, drv, mode) for drv in cpus]

        return generate
