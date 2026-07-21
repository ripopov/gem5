from gem5.components.processors.cpu_types import CPUTypes, get_mem_mode
from gem5.components.processors.simple_core import SimpleCore
from gem5.components.boards.mem_mode import MemMode
from gem5.isas import ISA


cpu_class = SimpleCore.cpu_class_factory(CPUTypes.JIT, ISA.RISCV)
assert cpu_class.__name__ == "RiscvJitCPU"
assert get_mem_mode(CPUTypes.JIT) == MemMode.ATOMIC_NONCACHING
print("JitCPU standard-library factory smoke test passed")
