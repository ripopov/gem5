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

import os

import m5
from m5.objects import (
    FstTrace,
    GoodbyeObject,
    HelloObject,
    Root,
)

root = Root(full_system=False)
root.goodbye = GoodbyeObject(
    buffer_size="32B", write_bandwidth="1000000000000B/s"
)
root.hello = HelloObject(
    time_to_wait="1t",
    number_of_fires=2,
    goodbye_object=root.goodbye,
)
root.trace = FstTrace(trace_file="events.fst", start_active=True)

m5.instantiate()
m5.simulate(1)
root.trace.setDumpActive(False)
m5.simulate(1)
root.trace.setDumpActive(True)
exit_event = m5.simulate()

trace_path = os.path.join(m5.options.outdir, "events.fst")
if not os.path.isfile(trace_path):
    raise RuntimeError(f"missing FST trace file: {trace_path}")

if os.path.getsize(trace_path) == 0:
    raise RuntimeError(f"empty FST trace file: {trace_path}")

print(exit_event.getCause())
