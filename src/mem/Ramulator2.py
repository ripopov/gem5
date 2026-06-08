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

from m5.citations import add_citation
from m5.objects.AbstractMemory import *
from m5.params import *


class Ramulator2(AbstractMemory):
    type = "Ramulator2"
    cxx_header = "mem/ramulator2.hh"
    cxx_class = "gem5::memory::Ramulator2"

    port = ResponsePort("The port for receiving memory requests")

    ramulator_config = Param.String(
        "",
        "Ramulator2 JSON/YAML configuration string built from the "
        "Ramulator2 Python DSL.",
    )

    post_writes = Param.Bool(
        True,
        "Acknowledge writes to the requestor as soon as they are accepted "
        "into Ramulator2's write buffer (gem5 MemCtrl-style posted writes), "
        "instead of waiting for the DRAM-completion callback. This makes the "
        "requestor-visible write back-pressure match the native gem5 "
        "MemCtrl: the requestor is freed at enqueue and only stalls when the "
        "write buffer is full. The write still drains to DRAM asynchronously. "
        "Set to False for completion-gated writes (requestor blocked until "
        "the write commits).",
    )

    write_frontend_latency = Param.Latency(
        "10ns",
        "Static latency added to a posted write acknowledgement. Mirrors the "
        "native gem5 MemCtrl static_frontend_latency so posted writes on both "
        "backends report the same low-load write response latency. Ignored "
        "when post_writes is False.",
    )


add_citation(
    Ramulator2,
    """@article{Luo:2023:ramulator2,
  author       = {Haocong Luo and
                  Yahya Can Tugrul and
                  Jisung Park and
                  Minesh Patel and
                  Hasan Hassan and
                  Jeremie S. Kim and
                  Onur Mutlu},
  title        = {Ramulator 2.0: A Modern, Modular, and Extensible
                  {DRAM} Simulator},
  journal      = {{IEEE} Computer Architecture Letters},
  volume       = {22},
  number       = {2},
  pages        = {111--114},
  year         = {2023},
  doi          = {10.1109/LCA.2023.3286891}
}
""",
)
