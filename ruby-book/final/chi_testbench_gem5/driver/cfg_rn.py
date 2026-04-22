# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
Direct-mesh CHI request node for the gem5-native testbench.

Every tile in this testbench has the same wiring:

    ChiSeqDriver --RequestPort--> RubySequencer
                                     |
                                     v
                             CHI_TileCacheController
                                     |
                                     v (direct ExtLink; no side router)
                                 Mesh Router

The controller is a single `Base_CHI_Cache_Controller` (the SLICC
automaton `CHI_Cache_Controller`), parameterized at construction time
as either:

  * "rni"     : cache-less, behaves like `CHI_DMAController` — every
                access emits a CHI ReadOnce / WriteNoSnpFull transaction
                on the wire and nothing is ever cached at the RN.
  * "rnf_l2"  : coherent CHI leaf cache with L2-class sizing. The
                parameters match `CHI_L1Controller` (alloc_on_seq_acc,
                send_evictions, allow_SD, ...) so the RN participates
                in CHI coherence: ReadShared / ReadUnique / CleanUnique
                and snoop responses all work normally.

Why a single controller class can play both roles: all of
`CHI_L1Controller`, `CHI_L2Controller`, and `CHI_DMAController` in
`configs/ruby/CHI_config.py` derive from the same
`Base_CHI_Cache_Controller` and drive the same SLICC protocol; the
differences between them are purely parameter choices.

Why no side router is created: `configs/topologies/CustomMesh.py:235`
wraps an extra "node router" around a node only when
`isinstance(node, CHI.CHI_RNF)`. `CHI_Tile` inherits from
`CHI_config.CHI_RNI_DMA` (chosen purely so the `isinstance` classifier
places it in the `rni_dma_nodes` bucket without special-casing). The
RNF branch is bypassed, so our node's controller connects directly to
the mesh router via a single ExtLink.
"""

from __future__ import annotations

from ruby import CHI_config

from m5.objects import (
    NULL,
    RubyCache,
    RubySequencer,
)


class CHI_TileCacheController(CHI_config.Base_CHI_Cache_Controller):
    """
    One configurable CHI cache controller: `mode="rni"` disables every
    allocation and gives the controller a dummy 128-byte cache (pure
    pass-through), while `mode="rnf_l2"` enables leaf-cache coherence
    with an L2-sized real cache.
    """

    def __init__(
        self,
        ruby_system,
        sequencer,
        mode,
        l2_size="256KiB",
        l2_assoc=8,
    ):
        super().__init__(ruby_system)
        self.sequencer = sequencer

        # Common to both modes: leaf-level request node, no DMT/DCT,
        # no prefetcher, TBE counts matching the L1/L2 range.
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
            # Mirrors CHI_DMAController param set.
            class DummyCache(RubyCache):
                dataAccessLatency = 0
                tagAccessLatency = 1
                size = "128"
                assoc = 1

            self.cache = DummyCache()
            self.sequencer.dcache = NULL
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
            # Mirrors CHI_L1Controller param shape (leaf cache with
            # coherence), but with L2-class sizing/latency.
            self.cache = RubyCache(
                size=l2_size,
                assoc=l2_assoc,
                dataAccessLatency=6,
                tagAccessLatency=2,
            )
            self.sequencer.dcache = self.cache
            self.allow_SD = True
            self.send_evictions = True
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
                f"CHI_TileCacheController: unknown mode {mode!r} "
                "(expected 'rni' or 'rnf_l2')"
            )


class CHI_Tile(CHI_config.CHI_RNI_DMA):
    """
    Cache-less-or-L2 request node. Inherits from `CHI_RNI_DMA` so
    `CustomMesh.makeTopology` classifies it as an RNI-DMA node and
    places it on a single mesh router with no side router (see
    `configs/topologies/CustomMesh.py:235,341`). The stock
    `CHI_RNI_DMA.__init__` requires a dma_port and immediately binds
    it; we don't have one, so `__init__` is reimplemented.
    """

    class NoC_Params(CHI_config.CHI_RNI_DMA.NoC_Params):
        # Place 16 tiles on routers 0..15 of the 4x4 mesh. Router ids
        # are chosen so CustomMesh's circulate-mode assigns them 1:1 in
        # order.
        router_list = list(range(16))

    def __init__(self, ruby_system, mode):
        # Bypass CHI_RNI_DMA / CHI_RNI_Base __init__: they either
        # require a dma_port or hard-code CHI_DMAController. We want
        # our own configurable controller.
        CHI_config.CHI_Node.__init__(self, ruby_system)

        self._sequencer = RubySequencer(
            version=CHI_config.Versions.getSeqId(),
            ruby_system=ruby_system,
            clk_domain=ruby_system.clk_domain,
        )
        self._cntrl = CHI_TileCacheController(
            ruby_system, self._sequencer, mode
        )
        self.cntrl = self._cntrl
        self.connectController(self._cntrl)

    def getSequencers(self):
        # Ruby.create_system puts this in ruby._cpu_ports, from which
        # the testbench binds `drv.port = ...in_ports`.
        return [self._sequencer]

    def getAllControllers(self):
        return [self._cntrl]

    def getNetworkSideControllers(self):
        return [self._cntrl]

    def setDownstream(self, cntrls):
        self._cntrl.downstream_destinations = cntrls

    @classmethod
    def make_generator(cls, mode):
        """
        Returns a callable compatible with the `system._rnf_gen` hook
        in `configs/ruby/CHI.py:125`.
        """

        def generate(options, ruby_system, cpus):
            return [cls(ruby_system, mode) for _ in cpus]

        return generate
