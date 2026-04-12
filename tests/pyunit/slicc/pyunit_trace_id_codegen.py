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

import shutil
import sys
import tempfile
import textwrap
import unittest
import warnings
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "build_tools"))
sys.path.insert(0, str(REPO_ROOT / "ext" / "ply"))
sys.path.insert(0, str(REPO_ROOT / "src" / "mem"))

from slicc.parser import SLICC

warnings.simplefilter("ignore", ResourceWarning)


RUBY_INTERFACES = (
    REPO_ROOT
    / "src"
    / "mem"
    / "ruby"
    / "protocol"
    / "RubySlicc_interfaces.slicc"
)
RUBY_SLICC_INCLUDES = ["mem/ruby/slicc_interface/RubySlicc_includes.hh"]


def _compile_protocol(
    protocol_path: Path, base_dir: Path, output_dir: Path
) -> None:
    slicc = SLICC(
        str(protocol_path),
        [str(RUBY_INTERFACES)],
        str(base_dir),
        verbose=False,
    )
    slicc.process()
    slicc.writeCodeFiles(str(output_dir), RUBY_SLICC_INCLUDES)


def _write_fixture_protocol(protocol_dir: Path) -> Path:
    (protocol_dir / "TraceContextFixture.slicc").write_text(
        textwrap.dedent("""\
            protocol "TraceContextFixture";
            include "TraceContextFixture-msg.sm";
            include "TraceContextFixture-cache.sm";
            """),
        encoding="utf-8",
    )

    (protocol_dir / "TraceContextFixture-msg.sm").write_text(
        textwrap.dedent("""\
            enumeration(TestRequestType, desc="...") {
              REQ, desc="...";
            }

            structure(RequestMsg, desc="...", interface="Message") {
              Addr addr, desc="...";
              TestRequestType Type, desc="...";
              NetDest Destination, desc="...";
              MessageSizeType MessageSize, desc="...";

              bool functionalRead(Packet *pkt) {
                return false;
              }

              bool functionalWrite(Packet *pkt) {
                return false;
              }
            }

            structure(TriggerMsg, desc="...", interface="Message") {
              Addr addr, desc="...";
              NetDest Destination, desc="...";
              MessageSizeType MessageSize, desc="...";

              bool functionalRead(Packet *pkt) {
                return false;
              }

              bool functionalWrite(Packet *pkt) {
                return false;
              }
            }
            """),
        encoding="utf-8",
    )

    (protocol_dir / "TraceContextFixture-cache.sm").write_text(
        textwrap.dedent("""\
            machine(MachineType:L1Cache, "Trace Context Fixture")
                : Cycles issue_latency := 1;
                  MessageBuffer * requestIn;
                  MessageBuffer * nestedIn;
                  MessageBuffer * requestOut;
                  MessageBuffer * triggerOut;
            {
              state_declaration(State, desc="...") {
                I, AccessPermission:Invalid, desc="...";
              }

              enumeration(Event, desc="...") {
                Incoming, desc="...";
              }

              Tick clockEdge();
              Cycles ticksToCycles(Tick t);

              State getState(Addr addr) {
                return State:I;
              }

              void setState(Addr addr, State state) {
              }

              AccessPermission getAccessPermission(Addr addr) {
                return AccessPermission:NotPresent;
              }

              int functionalWrite(Addr addr, Packet *pkt) {
                return 0;
              }

              void functionalRead(Addr addr, Packet *pkt) {
              }

              out_port(requestOutPort, RequestMsg, requestOut);
              out_port(triggerOutPort, TriggerMsg, triggerOut);

              in_port(requestInPort, RequestMsg, requestIn) {
                if (requestInPort.isReady(clockEdge())) {
                  peek(requestInPort, RequestMsg) {
                    trigger(Event:Incoming, in_msg.addr);
                  }
                }
              }

              in_port(nestedInPort, TriggerMsg, nestedIn) {
                if (nestedInPort.isReady(clockEdge())) {
                  peek(nestedInPort, TriggerMsg) {
                    trigger(Event:Incoming, in_msg.addr);
                  }
                }
              }

              action(sendRequest, desc="...") {
                peek(requestInPort, RequestMsg) {
                  enqueue(requestOutPort, RequestMsg, issue_latency) {
                    out_msg.addr := address;
                    out_msg.Type := TestRequestType:REQ;
                    out_msg.MessageSize := MessageSizeType:Request_Control;
                  }
                }
              }

              action(sendNested, desc="...") {
                peek(requestInPort, RequestMsg) {
                  peek(nestedInPort, TriggerMsg) {
                    enqueue(triggerOutPort, TriggerMsg, issue_latency) {
                      out_msg.addr := address;
                      out_msg.MessageSize := MessageSizeType:Response_Control;
                    }
                  }
                }
              }

              action(deferTrigger, desc="...") {
                defer_enqueueing(triggerOutPort, TriggerMsg) {
                  out_msg.addr := address;
                  out_msg.MessageSize := MessageSizeType:Response_Control;
                }
              }

              transition(I, Incoming) {
                sendRequest;
              }
            }
            """),
        encoding="utf-8",
    )

    return protocol_dir / "TraceContextFixture.slicc"


def _snippet(text: str, anchor: str, length: int = 4000) -> str:
    start = text.index(anchor)
    return text[start : start + length]


def _assert_in_order(testcase: unittest.TestCase, text: str, snippets) -> None:
    cursor = -1
    for snippet in snippets:
        next_cursor = text.find(snippet, cursor + 1)
        testcase.assertNotEqual(
            next_cursor,
            -1,
            f"Missing snippet in generated code: {snippet}",
        )
        cursor = next_cursor


class SliccTraceIdCodegenTestSuite(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp_root = Path(tempfile.mkdtemp(prefix="slicc-trace-id-"))

        mi_out = cls._tmp_root / "mi_example"
        mi_out.mkdir()
        _compile_protocol(
            REPO_ROOT
            / "src"
            / "mem"
            / "ruby"
            / "protocol"
            / "MI_example.slicc",
            REPO_ROOT / "src" / "mem" / "ruby" / "protocol",
            mi_out,
        )
        cls.mi_example_controller = (
            mi_out / "MI_example" / "L1Cache_Controller.cc"
        ).read_text(encoding="utf-8")

        chi_out = cls._tmp_root / "chi"
        chi_out.mkdir()
        _compile_protocol(
            REPO_ROOT
            / "src"
            / "mem"
            / "ruby"
            / "protocol"
            / "chi"
            / "CHI.slicc",
            REPO_ROOT / "src" / "mem" / "ruby" / "protocol" / "chi",
            chi_out,
        )
        cls.chi_controller = (
            chi_out / "CHI" / "Cache_Controller.cc"
        ).read_text(encoding="utf-8")

        fixture_protocol_dir = cls._tmp_root / "fixture_protocol"
        fixture_protocol_dir.mkdir()
        fixture_out = cls._tmp_root / "fixture_out"
        fixture_out.mkdir()
        fixture_protocol = _write_fixture_protocol(fixture_protocol_dir)
        _compile_protocol(fixture_protocol, fixture_protocol_dir, fixture_out)
        cls.fixture_controller = (
            fixture_out / "TraceContextFixture" / "L1Cache_Controller.cc"
        ).read_text(encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls._tmp_root, ignore_errors=True)

    def test_peek_installs_guard_before_enqueue_logic(self):
        allocate_tbe_request = _snippet(
            self.chi_controller, "Cache_Controller::AllocateTBE_Request("
        )
        _assert_in_order(
            self,
            allocate_tbe_request,
            [
                "in_msg_ptr = dynamic_cast<const CHIRequestMsg *>",
                "throw RejectException();",
                "scopedRootTraceContext(in_msg_ptr->getRootTraceId())",
                "out_msg->setRootTraceId(getCurrentRootTraceId());",
                ").enqueue(out_msg, clockEdge()",
            ],
        )

    def test_chi_two_hop_actions_use_controller_context(self):
        initiate_request = _snippet(
            self.chi_controller, "Cache_Controller::Initiate_Request("
        )
        self.assertIn(
            "scopedRootTraceContext(in_msg_ptr->getRootTraceId())",
            initiate_request,
        )

        send_read_shared = _snippet(
            self.chi_controller, "Cache_Controller::Send_ReadShared("
        )
        self.assertNotIn("in_msg_ptr", send_read_shared)
        _assert_in_order(
            self,
            send_read_shared,
            [
                "prepareRequest(",
                "allowRequestRetry(",
                "out_msg->setRootTraceId(getCurrentRootTraceId());",
                ").enqueue(out_msg, clockEdge()",
            ],
        )

    def test_existing_protocol_enqueue_stamps_before_buffer_visibility(self):
        send_getx = _snippet(
            self.mi_example_controller, "CoherenceRequestType_GETX"
        )
        _assert_in_order(
            self,
            send_getx,
            [
                "(*out_msg).m_addr = addr;",
                "out_msg->setRootTraceId(getCurrentRootTraceId());",
                ").enqueue(out_msg, clockEdge()",
            ],
        )

    def test_fixture_generates_nested_peek_guards(self):
        send_nested = _snippet(
            self.fixture_controller,
            "L1Cache_Controller::sendNested(",
        )
        self.assertGreaterEqual(
            send_nested.count(
                "scopedRootTraceContext(in_msg_ptr->getRootTraceId())"
            ),
            2,
        )

    def test_fixture_stamps_deferred_messages_without_peek_context(self):
        defer_trigger = _snippet(
            self.fixture_controller,
            "L1Cache_Controller::deferTrigger(",
        )
        self.assertNotIn("scopedRootTraceContext(", defer_trigger)
        _assert_in_order(
            self,
            defer_trigger,
            [
                "(*out_msg).m_addr = addr;",
                "out_msg->setRootTraceId(getCurrentRootTraceId());",
                ").deferEnqueueingMessage(addr, out_msg);",
            ],
        )
