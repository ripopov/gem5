# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.MessageBuffer import MessageBuffer
from m5.objects.SimpleLink import (
    SimpleExtLink,
    SimpleIntLink,
)
from m5.objects.SimpleNetwork import (
    SimpleNetwork,
    Switch,
    SwitchPortBuffer,
)
from m5.params import *
from m5.proxy import *
from m5.util import fatal


class XPIntLink(SimpleIntLink):
    type = "XPIntLink"
    cxx_header = "mem/ruby/network/simple/xp/XPLink.hh"
    cxx_class = "gem5::ruby::XPIntLink"

    def setup_buffers(self, network):
        if len(self.buffers) > 0:
            fatal("User should not manually set XP int-link buffers")

        vnets = int(network.number_of_virtual_networks)
        if len(network.xp_credits) not in (0, vnets):
            fatal("xp_credits must be empty or provide one value per vnet")
        if len(network.xp_credit_return_latency) not in (0, vnets):
            fatal(
                "xp_credit_return_latency must be empty or provide one "
                "value per vnet"
            )
        if len(network.physical_vnets_channels) not in (0, vnets):
            fatal(
                "physical_vnets_channels must be empty or provide one "
                "value per vnet"
            )

        def per_vnet(values, vnet, default):
            return values[vnet] if len(values) else default

        def channels(vnet):
            if len(network.physical_vnets_channels) == 0:
                return 1
            count = int(network.physical_vnets_channels[vnet])
            if count <= 0:
                fatal("physical_vnets_channels entries must be positive")
            return count

        buffers = []
        for vnet in range(vnets):
            credits = per_vnet(network.xp_credits, vnet, 0) * channels(vnet)
            credit_return_latency = per_vnet(
                network.xp_credit_return_latency, vnet, 1
            )
            if int(credit_return_latency) <= 0:
                fatal("xp_credit_return_latency entries must be positive")
            buffer_size = credits
            if credits == 0 and len(network.physical_vnets_channels) != 0:
                buffer_size = channels(vnet) * (int(self.latency) + 1)

            buf = MessageBuffer(
                ordered=True,
                buffer_size=buffer_size,
                credits=credits,
                credit_return_latency=credit_return_latency,
                enable_ooo_pop=network.xp_enable_ooo_pop,
            )
            if len(network.physical_vnets_channels) != 0:
                buf.max_dequeue_rate = channels(vnet)
            buffers.append(buf)

        self.buffers = buffers


class XPExtLink(SimpleExtLink):
    type = "XPExtLink"
    cxx_header = "mem/ruby/network/simple/xp/XPLink.hh"
    cxx_class = "gem5::ruby::XPExtLink"


class XPStagingBuffer(SwitchPortBuffer):
    """Switch-local finite staging between XP arbitration and link driving."""


class XPSwitch(Switch):
    type = "XPSwitch"
    cxx_header = "mem/ruby/network/simple/xp/XPSwitch.hh"
    cxx_class = "gem5::ruby::XPSwitch"

    def setup_buffers(self, network):
        if len(self.port_buffers) > 0:
            fatal("User should not manually set XP routers' port_buffers")

        vnets = int(network.number_of_virtual_networks)
        if len(network.physical_vnets_channels) not in (0, vnets):
            fatal(
                "physical_vnets_channels must be empty or provide one "
                "value per vnet"
            )

        def channels(vnet):
            if len(network.physical_vnets_channels) == 0:
                return 1
            count = int(network.physical_vnets_channels[vnet])
            if count <= 0:
                fatal("physical_vnets_channels entries must be positive")
            return count

        def staging_depth(vnet):
            if int(network.xp_staging_depth) <= 0:
                fatal("xp_staging_depth must be greater than zero")
            return int(network.xp_staging_depth) * channels(vnet)

        router_buffers = []

        for link in network.ext_links:
            if link.int_node == self:
                for vnet in range(vnets):
                    router_buffers.append(
                        XPStagingBuffer(buffer_size=staging_depth(vnet))
                    )

        for link in network.int_links:
            if link.dst_node == self:
                for vnet in range(vnets):
                    router_buffers.append(
                        XPStagingBuffer(buffer_size=staging_depth(vnet))
                    )

        self.port_buffers = router_buffers


class XPNetwork(SimpleNetwork):
    type = "XPNetwork"
    cxx_header = "mem/ruby/network/simple/xp/XPNetwork.hh"
    cxx_class = "gem5::ruby::XPNetwork"

    xp_credits = VectorParam.Unsigned(
        [], "Per-vnet internal-link credits; empty disables credited mode"
    )
    xp_credit_return_latency = VectorParam.Cycles(
        [], "Per-vnet credit-return latency in router cycles"
    )
    xp_staging_depth = Param.Unsigned(
        2, "Messages per output/vnet staging buffer before the link driver"
    )
    xp_enable_ooo_pop = Param.Bool(
        True, "Enable oldest-eligible XP input-buffer arbitration"
    )
