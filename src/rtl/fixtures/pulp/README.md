# PULP AMBA Vendor Fixtures

These fixtures package pinned PULP APB and AXI components as four independent
V1 vendor shared libraries. They validate the portable adapter boundary and
both transactor directions without gem5 or SCR1.

| Fixture | RTL role | PULP component exercised |
| --- | --- | --- |
| `rtl_cosim_pulp_apb_master` | Initiator | `apb_demux` |
| `rtl_cosim_pulp_apb_slave` | Target | `apb_demux` |
| `rtl_cosim_pulp_axi_master` | Initiator | `axi_modify_address` |
| `rtl_cosim_pulp_axi_slave` | Target | `axi_sim_mem` |

The wrappers flatten PULP structs, bind every real pin to a canonical V1 role,
tie unsupported atomic inputs to zero, implement reset and complete-cycle
clocking, and keep all Verilator and PULP types inside `fixture_adapter.cc`.

The four `pulp_*_checker` tests load each library as a separate process-level
integration scenario. `PulpRuntimeLoop.*` loads matching master and slave
libraries simultaneously and connects them only through `TransactionBackend`
and `TransactionSource`. Readback expectations prove that traffic traversed
both independently Verilated endpoints.

See `src/rtl/README.md` for build, sanitizer, checker, and troubleshooting
commands.
