# Chapter 13: Building CHI Systems in Legacy Ruby and the Stdlib

> *CHI in gem5 is not one API; it is a protocol implementation plus two real ways of building systems around it.*

This chapter will show both configuration paths for CHI systems: the legacy path (`configs/ruby/CHI.py`, `CHI_config.py`) and the modern stdlib path (`gem5/components/cachehierarchies/chi/*`).
It will walk through building a RISC-V CHI system using `chi-with-isa.py` as the primary runnable example, with Arm and x86 as reality anchors.
The relationship between the two configuration styles will be made explicit rather than pretending one fully replaced the other.
The failure-mode section will address misconceptions like "CHI is Arm-only" or "CHI is just another MESI variant."
By the end, readers can build, configure, and reason about both legacy and modern stdlib CHI hierarchies on RISC-V.
