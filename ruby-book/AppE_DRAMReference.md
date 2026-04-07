# Appendix E: DRAM Parameter and Memory-Model Reference

This appendix will map gem5's DRAM parameters to the underlying memory-model concepts and timing terms (tCAS, tRCD, tRP, tRAS, etc.).
It will cover the stdlib DRAM interface definitions under `src/python/gem5/components/memory/dram_interfaces/` and how they correspond to real device datasheets.
Address mapping parameters (channels, ranks, banks, rows, columns) and their effect on interleaving will be documented.
This serves as a lookup reference for readers configuring and interpreting DRAM experiments in Chapters 15 and 16.
