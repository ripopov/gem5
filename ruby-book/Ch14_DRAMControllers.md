# Chapter 14: DRAM Controllers, Address Mapping, and the Off-Chip Bottleneck

> *A cache miss is not done when it leaves the chip; it enters another queueing system with its own state machine and locality rules.*

This chapter will cover gem5's memory controller stack: MemCtrl, DRAMInterface, MemInterface, and address mapping (AddrMapper).
It will explain command scheduling, bank state machines, row-buffer management, and how addresses are interleaved across channels, ranks, and banks.
Readers will run traffic-generator sweeps across DDR, LPDDR, and HBM configurations using the stdlib memory components.
The failure-mode section will show how interpreting memory latency without checking interleaving, row locality, and bank conflicts is one of the fastest ways to misread gem5 results.
By the end, readers can explain where DRAM latency comes from in gem5 and design experiments that isolate mapping and controller effects.
