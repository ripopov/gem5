# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

"""Standard-library wrapper for an RTL co-simulation core."""

from typing import (
    Dict,
    List,
    Optional,
    Sequence,
)

from m5.objects import RtlCoreSimObject
from m5.objects import PcCountTrackerManager
from m5.params import (
    AddrRange,
    PcCountPair,
    Port,
)

from ...isas import ISA
from ...utils.override import overrides
from .abstract_core import AbstractCore


class RtlCore(AbstractCore):
    """Wrap an :class:`RtlCoreSimObject` for use with stdlib caches.

    Connections are collected by vendor API bus name and attached to the
    SimObject vector in ``initiator_bus_names`` order. This makes instruction
    and data wiring independent of the order in which a cache hierarchy calls
    ``connect_icache`` and ``connect_dcache``.
    """

    def __init__(
        self,
        *,
        library: str,
        model_config: str,
        image: str,
        initiator_bus_names: Sequence[str],
        instruction_bus_name: str,
        data_bus_name: str,
        interrupt_input_names: Sequence[str] = (),
        interrupt_output_names: Sequence[str] = (),
        reset_input_names: Sequence[str] = (),
        reset_output_names: Sequence[str] = (),
        io_input_names: Sequence[str] = (),
        io_input_values: Sequence[str] = (),
        io_output_names: Sequence[str] = (),
        target_bus_names: Sequence[str] = (),
        target_addr_ranges: Sequence[AddrRange] = (),
        image_bus: str = "",
        image_format: str = "auto",
        initial_reset_cycles: int = 10,
        error_ranges: Sequence[AddrRange] = (),
        isa: ISA = ISA.RISCV,
    ) -> None:
        super().__init__()

        names = list(initiator_bus_names)
        if len(set(names)) != len(names):
            raise ValueError("initiator_bus_names must be unique")
        if instruction_bus_name == data_bus_name:
            raise ValueError("instruction and data buses must be distinct")
        if len(names) != 2:
            raise ValueError(
                "the standard cache adapter requires exactly two initiator "
                "buses"
            )
        for name in (instruction_bus_name, data_bus_name):
            if name not in names:
                raise ValueError(f"bus {name!r} is not an initiator bus")

        self._isa = isa
        self._initiator_bus_names = names
        self._instruction_bus_name = instruction_bus_name
        self._data_bus_name = data_bus_name
        self._pending_connections: Dict[str, Port] = {}
        self._ports_finalized = False

        self.rtl = RtlCoreSimObject(
            library=library,
            model_config=model_config,
            image=image,
            image_format=image_format,
            image_bus=image_bus,
            initial_reset_cycles=initial_reset_cycles,
            error_ranges=list(error_ranges),
            initiator_bus_names=names,
            target_bus_names=list(target_bus_names),
            target_addr_ranges=list(target_addr_ranges),
            interrupt_input_names=list(interrupt_input_names),
            interrupt_output_names=list(interrupt_output_names),
            reset_input_names=list(reset_input_names),
            reset_output_names=list(reset_output_names),
            io_input_names=list(io_input_names),
            io_input_values=list(io_input_values),
            io_output_names=list(io_output_names),
        )

    def _connect_initiator(self, name: str, port: Port) -> None:
        if self._ports_finalized:
            raise RuntimeError("RTL initiator ports are already connected")
        if name in self._pending_connections:
            raise RuntimeError(f"RTL bus {name!r} was connected twice")
        self._pending_connections[name] = port

        if len(self._pending_connections) == len(self._initiator_bus_names):
            for bus_name in self._initiator_bus_names:
                self.rtl.initiator_ports = self._pending_connections[bus_name]
            self._ports_finalized = True

    @overrides(AbstractCore)
    def get_isa(self) -> ISA:
        return self._isa

    @overrides(AbstractCore)
    def requires_send_evicts(self) -> bool:
        return False

    @overrides(AbstractCore)
    def is_kvm_core(self) -> bool:
        return False

    @overrides(AbstractCore)
    def connect_icache(self, port: Port) -> None:
        self._connect_initiator(self._instruction_bus_name, port)

    @overrides(AbstractCore)
    def connect_dcache(self, port: Port) -> None:
        self._connect_initiator(self._data_bus_name, port)

    @overrides(AbstractCore)
    def connect_walker_ports(self, port1: Port, port2: Port) -> None:
        pass

    @overrides(AbstractCore)
    def connect_interrupt(
        self,
        interrupt_requestor: Optional[Port] = None,
        interrupt_responce: Optional[Port] = None,
    ) -> None:
        pass

    @overrides(AbstractCore)
    def set_workload(self, process: "Process") -> None:
        pass

    @overrides(AbstractCore)
    def set_switched_out(self, value: bool) -> None:
        if value:
            raise NotImplementedError("RTL cores cannot be switched out")

    @overrides(AbstractCore)
    def get_mmu(self):
        raise NotImplementedError("RTL core MMUs are internal to the model")

    @overrides(AbstractCore)
    def _set_simpoint(
        self, inst_starts: List[int], board_initialized: bool
    ) -> None:
        raise NotImplementedError("RTL cores do not expose SimPoint events")

    @overrides(AbstractCore)
    def _set_inst_stop_any_thread(
        self, inst: int, board_initialized: bool
    ) -> None:
        raise NotImplementedError(
            "RTL cores do not expose instruction-count events"
        )

    @overrides(AbstractCore)
    def add_pc_tracker_probe(
        self,
        target_pair: List[PcCountPair],
        manager: PcCountTrackerManager,
    ) -> None:
        raise NotImplementedError("RTL cores do not expose retired PCs")

    @overrides(AbstractCore)
    def get_total_instructions(self) -> int:
        return 0
