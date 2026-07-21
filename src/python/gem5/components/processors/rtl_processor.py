# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

"""Standard-library processor containing vendor RTL cores."""

from typing import Sequence

from ...utils.override import overrides
from ..boards.abstract_board import AbstractBoard
from ..boards.mem_mode import MemMode
from .abstract_processor import AbstractProcessor
from .rtl_core import RtlCore


class RtlProcessor(AbstractProcessor):
    def __init__(self, cores: Sequence[RtlCore]) -> None:
        if not cores:
            raise ValueError("an RTL processor requires at least one core")
        super().__init__(cores=list(cores))

    @overrides(AbstractProcessor)
    def incorporate_processor(self, board: AbstractBoard) -> None:
        board.set_mem_mode(MemMode.TIMING)
