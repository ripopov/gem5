# Chapter 15: Optional Advanced Backends: HBM, NVM, DRAMSys, and HMC

> *The core book should master gem5's built-in path first, then widen the design space deliberately.*

This chapter will survey gem5's advanced memory backends: HBM controller (hbm_ctrl), NVM interface, heterogeneous memory controller, and external integrations (DRAMSys, DRAMSim3, HMC).
It will explain what each backend adds over the standard DRAMInterface and when the additional complexity is justified.
Readers will run optional small labs using the provided example scripts (dramsys.py, hmctest.py, hmc_hello.py).
The failure-mode section will explain why pulling external-memory complexity into the main learning path too early makes the book broader but weaker.
By the end, readers understand what these backends offer, when they are worth the integration cost, and why they remain optional in the core learning path.
