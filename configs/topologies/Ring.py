# Copyright (c) 2026 The gem5 Authors
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

from topologies.BaseTopology import SimpleTopology

from m5.objects import *
from m5.params import *


class Ring(SimpleTopology):
    """A unidirectional-pair ring: one router per controller, and every
    router joined to its two ring neighbours by a link in each direction.

    Works for both the simple and Garnet networks.  Garnet should be run
    with the default weight-based ("table") routing -- XY routing is for
    mesh topologies only.  The ring degrades gracefully for tiny node
    counts (a 1-node ring has no internal links, a 2-node ring has a
    single bidirectional link).
    """

    description = "Ring"

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes
        num_routers = len(nodes)

        # default values for link latency and router latency.
        # Can be over-ridden on a per link/router basis
        link_latency = options.link_latency  # used by simple and garnet
        router_latency = options.router_latency  # only used by garnet

        # One router per controller.
        routers = [
            Router(router_id=i, latency=router_latency)
            for i in range(num_routers)
        ]
        network.routers = routers

        # Attach each controller to its own router with an external link.
        link_count = 0
        ext_links = []
        for (i, n) in enumerate(nodes):
            ext_links.append(
                ExtLink(
                    link_id=link_count,
                    ext_node=n,
                    int_node=routers[i],
                    latency=link_latency,
                )
            )
            link_count += 1
        network.ext_links = ext_links

        # Join the routers into a ring.  Each hop gets a link in both
        # directions so traffic can travel either way around the ring.
        int_links = []
        # A 2-node ring needs a single bidirectional pair; a >2-node ring
        # walks every neighbour once.
        hops = num_routers if num_routers > 2 else num_routers // 2
        for i in range(hops):
            j = (i + 1) % num_routers
            int_links.append(
                IntLink(
                    link_id=link_count,
                    src_node=routers[i],
                    dst_node=routers[j],
                    latency=link_latency,
                )
            )
            link_count += 1
            int_links.append(
                IntLink(
                    link_id=link_count,
                    src_node=routers[j],
                    dst_node=routers[i],
                    latency=link_latency,
                )
            )
            link_count += 1
        network.int_links = int_links
