# Copyright (c) 2021 ARM Limited
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
# Copyright (c) 2009 Advanced Micro Devices, Inc.
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

from m5.objects.BasicRouter import BasicRouter
from m5.objects.MessageBuffer import MessageBuffer
from m5.objects.Network import RubyNetwork
from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject
from m5.util import fatal


class SimpleNetwork(RubyNetwork):
    type = "SimpleNetwork"
    cxx_header = "mem/ruby/network/simple/SimpleNetwork.hh"
    cxx_class = "gem5::ruby::SimpleNetwork"

    buffer_size = Param.Int(
        0,
        "default internal buffer size for links and\
                                routers; 0 indicates infinite buffering",
    )
    endpoint_bandwidth = Param.Int(1000, "bandwidth adjustment factor")

    physical_vnets_channels = VectorParam.Int(
        [],
        "Set to emulate multiple channels for each vnet."
        "If not set, all vnets share the same physical channel.",
    )

    physical_vnets_bandwidth = VectorParam.Int(
        [],
        "Assign a different link bandwidth factor for each vnet channels."
        "Only valid when physical_vnets_channels is set. This overrides the"
        "bandwidth_factor parameter set for the  individual links.",
    )

    def setup_buffers(self):
        # Setup internal buffers for links and routers
        for link in self.int_links:
            link.setup_buffers(self)
        for router in self.routers:
            router.setup_buffers(self)


class BaseRoutingUnit(SimObject):
    type = "BaseRoutingUnit"
    abstract = True
    cxx_header = "mem/ruby/network/simple/routing/BaseRoutingUnit.hh"
    cxx_class = "gem5::ruby::BaseRoutingUnit"


class WeightBased(BaseRoutingUnit):
    type = "WeightBased"
    cxx_header = "mem/ruby/network/simple/routing/WeightBased.hh"
    cxx_class = "gem5::ruby::WeightBased"

    adaptive_routing = Param.Bool(False, "enable adaptive routing")


class SwitchPortBuffer(MessageBuffer):
    """MessageBuffer type used internally by the Switch port buffers"""

    ordered = True
    allow_zero_latency = True


class Switch(BasicRouter):
    type = "Switch"
    cxx_header = "mem/ruby/network/simple/Switch.hh"
    cxx_class = "gem5::ruby::Switch"

    virt_nets = Param.Int(
        Parent.number_of_virtual_networks, "number of virtual networks"
    )

    int_routing_latency = Param.Cycles(
        BasicRouter.latency, "Routing latency to internal links"
    )
    ext_routing_latency = Param.Cycles(
        BasicRouter.latency, "Routing latency to external links"
    )

    # Internal port buffers used between the PerfectSwitch and
    # Throttle objects. There is one buffer per virtual network
    # and per output port.
    # These are created by setup_buffers and the user should not
    # set these manually.
    port_buffers = VectorParam.MessageBuffer([], "Port buffers")

    routing_unit = Param.BaseRoutingUnit(
        WeightBased(adaptive_routing=False), "Routing strategy to be used"
    )

    def setup_buffers(self, network):
        def vnet_buffer_size(vnet, buffer_size):
            """
            Gets the size of the message buffers associated to a vnet
            If physical_vnets_channels is set we just multiply the size of the
            buffers as SimpleNetwork does not actually creates multiple phy
            channels per vnet.
            """
            if len(network.physical_vnets_channels) == 0:
                return buffer_size
            else:
                return buffer_size * network.physical_vnets_channels[vnet]

        def ext_vnet_buffer_size(vnet):
            if network.buffer_size == 0:
                return 0

            buffer_size = max(
                network.buffer_size, int(self.ext_routing_latency) + 1
            )
            return vnet_buffer_size(vnet, buffer_size)

        if len(self.port_buffers) > 0:
            fatal("User should not manually set routers' port_buffers")

        router_buffers = []

        # These buffers are the Switch-local queues between PerfectSwitch and
        # Throttle. They are consumed sequentially by Switch::addOutPort(), so
        # their order must match Topology::createLinks()'s addOutPort order.
        #
        # Topology numbers endpoint-output nodes below router nodes and scans
        # destinations in numeric order. Therefore, for a given router source,
        # external output links are connected before internal output links.
        # Keep external buffers first so the larger ext_routing_latency-sized
        # buffers are consumed by external ports only.
        for link in network.ext_links:
            # Routers can only be int_nodes on ext_links
            if link.int_node == self:
                for i in range(int(network.number_of_virtual_networks)):
                    router_buffers.append(
                        SwitchPortBuffer(buffer_size=ext_vnet_buffer_size(i))
                    )

        # Internal links use the normal network buffer size. This preserves the
        # existing allocation condition based on dst_node. The buffers are still
        # consumed as output-port queues by Switch::addOutPort(); in the
        # bidirectional mesh topologies used here, each router has matching
        # incoming/outgoing internal-link counts, so the existing count/order
        # remains valid while external buffers above get distinct sizing.
        for link in network.int_links:
            if link.dst_node == self:
                for i in range(int(network.number_of_virtual_networks)):
                    router_buffers.append(
                        SwitchPortBuffer(
                            buffer_size=vnet_buffer_size(
                                i, network.buffer_size
                            )
                        )
                    )

        self.port_buffers = router_buffers
