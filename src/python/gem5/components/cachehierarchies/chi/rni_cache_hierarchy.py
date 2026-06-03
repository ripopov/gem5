# Copyright (c) 2026 Arm Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from typing import List

from m5.objects import (
    RubyPortProxy,
    RubySequencer,
    RubySystem,
)
from m5.params import (
    NULL,
    AllMemory,
)

from ....coherence_protocol import CoherenceProtocol
from ....utils.override import overrides
from ....utils.requires import requires
from ...boards.abstract_board import AbstractBoard
from ...processors.abstract_core import AbstractCore
from ..abstract_cache_hierarchy import AbstractCacheHierarchy
from ..ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from ..ruby.topologies.simple_pt2pt import SimplePt2Pt
from .nodes.directory import SimpleDirectory
from .nodes.dma_requestor import DMARequestor
from .nodes.memory_controller import MemoryController

requires(coherence_protocol_required=CoherenceProtocol.CHI)


class RNICacheHierarchy(AbstractRubyCacheHierarchy):
    """A cacheless CHI hierarchy for synthetic request-node experiments.

    Each generator core is connected to a CHI DMARequestor controller, which
    acts as an RN-I. Requests then flow through a single HNF directory and one
    SNF per memory port.
    """

    @overrides(AbstractCacheHierarchy)
    def get_coherence_protocol(self):
        return CoherenceProtocol.CHI

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        super().incorporate_cache(board)
        self.ruby_system = RubySystem()

        self.ruby_system.network = SimplePt2Pt(self.ruby_system)

        # CHI virtual networks: 0=request, 1=snoop, 2=response, 3=data.
        self.ruby_system.number_of_virtual_networks = 4
        self.ruby_system.network.number_of_virtual_networks = 4

        self.directory = SimpleDirectory(
            self.ruby_system.network,
            cache_line_size=board.get_cache_line_size(),
            clk_domain=board.get_clock_domain(),
            addr_ranges=[AllMemory],
        )
        self.directory.ruby_system = self.ruby_system

        self.request_nodes = [
            self._create_request_node(core, i, board)
            for i, core in enumerate(board.get_processor().get_cores())
        ]

        self.memory_controllers = self._create_memory_controllers(board)
        self.directory.downstream_destinations = self.memory_controllers

        self.ruby_system.num_of_sequencers = len(self.request_nodes)

        self.ruby_system.network.connectControllers(
            self.request_nodes + self.memory_controllers + [self.directory]
        )
        self.ruby_system.network.setup_buffers()

        self.ruby_system.sys_port_proxy = RubyPortProxy(
            ruby_system=self.ruby_system
        )
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)

    def _create_request_node(
        self, core: AbstractCore, core_num: int, board: AbstractBoard
    ) -> DMARequestor:
        request_node = DMARequestor(
            self.ruby_system.network,
            board.get_cache_line_size(),
            board.get_clock_domain(),
        )

        request_node.sequencer = RubySequencer(
            version=core_num,
            dcache=NULL,
            clk_domain=request_node.clk_domain,
            ruby_system=self.ruby_system,
        )

        request_node.ruby_system = self.ruby_system
        request_node.sequencer.ruby_system = self.ruby_system

        core.connect_dcache(request_node.sequencer.in_ports)
        core.connect_interrupt()

        request_node.downstream_destinations = [self.directory]
        return request_node

    def _create_memory_controllers(
        self, board: AbstractBoard
    ) -> List[MemoryController]:
        memory_controllers = []
        for rng, port in board.get_mem_ports():
            controller = MemoryController(
                self.ruby_system.network,
                rng,
                port,
            )
            controller.ruby_system = self.ruby_system
            memory_controllers.append(controller)
        return memory_controllers

    @overrides(AbstractRubyCacheHierarchy)
    def _reset_version_numbers(self):
        from .nodes.abstract_node import AbstractNode

        AbstractNode._version = 0
        MemoryController._version = 0
