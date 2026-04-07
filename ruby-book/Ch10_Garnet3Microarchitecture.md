# Chapter 10: Garnet 3.0 Microarchitecture

> *When every flit, credit, and router pipeline stage matters, Garnet becomes the center of the story.*

This chapter will do a detailed walkthrough of the Garnet 3.0 router microarchitecture: NetworkInterface, Router, InputUnit, OutputUnit, SwitchAllocator, CrossbarSwitch, NetworkLink, and NetworkBridge.
It will trace a flit from injection at the NI through the router pipeline (route compute, VC allocation, switch allocation, switch traversal, link traversal) and credit return.
Readers will run Garnet-backed Ruby experiments and map simulation output to the pipeline stages described in the code.
The failure-mode section will cover VC starvation, credit deadlock, and the breakdown of topology-independent intuition when buffer depth and link width matter.
By the end, readers can follow the Garnet README's code-flow description and connect it to cycle-by-cycle NoC behavior in simulation.
