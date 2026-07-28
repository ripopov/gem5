import argparse
import math
import os

import m5
from m5.objects import *
from m5.util import addToPath

addToPath("../../")
from amd import AmdGPUOptions
from common import GPUTLBOptions, ObjectList, Options
from ruby import Ruby

from runfs import addRunFSOptions
from system.system import makeGpuFSSystem


def _parse_int(value):
    return int(value, 0)


def addHsakmtServerOptions(parser):
    parser.add_argument(
        "--hsakmt-rpc-socket",
        default="",
        help="rocjitsu KMD socket; defaults beneath the runtime root",
    )
    parser.add_argument(
        "--rocjitsu-runtime-root",
        default=os.environ.get("ROCJITSU_RUNTIME_DIR", "/run/rocjitsu"),
        help="Shared directory containing rocjitsu topology and socket state",
    )
    parser.add_argument(
        "--rocjitsu-vram-shared-backstore",
        default="",
        help=(
            "POSIX shared-memory name for exported VRAM; an empty value "
            "uses a run-unique name"
        ),
    )
    parser.add_argument(
        "--hsakmt-auto-start",
        action="store_true",
        default=False,
        help="Publish the rocjitsu KMD listener during startup",
    )
    parser.add_argument(
        "--hsakmt-exit-on-rpc-disconnect",
        action="store_true",
        default=False,
        help="Exit after the attached rocjitsu process disconnects cleanly",
    )
    parser.add_argument(
        "--hsakmt-cold-checkpoint",
        default="",
        help=(
            "Write a checkpoint before publishing the rocjitsu listener; "
            "requires starting without --hsakmt-auto-start"
        ),
    )
    parser.add_argument(
        "--hsakmt-framebuffer-bar-base",
        type=_parse_int,
        default=0x2400000000,
        help="PCI base assigned to AMDGPU BAR0",
    )
    parser.add_argument(
        "--hsakmt-doorbell-bar-base",
        type=_parse_int,
        default=0x2200000000,
        help="PCI base assigned to AMDGPU BAR2",
    )
    parser.add_argument(
        "--hsakmt-mmio-bar-base",
        type=_parse_int,
        default=0xECF00000,
        help="PCI base assigned to AMDGPU BAR5",
    )
    parser.add_argument(
        "--hsakmt-setup-memory-base",
        type=_parse_int,
        default=0x70000000,
        help="GPU VA base for service-owned control memory",
    )
    parser.add_argument(
        "--hsakmt-setup-page-table-device-offset",
        type=_parse_int,
        default=0x1000000,
        help="VRAM offset for service-owned GPUVM page tables",
    )
    parser.add_argument(
        "--hsakmt-setup-page-table-bytes",
        type=_parse_int,
        default=0x100000,
        help="VRAM bytes reserved for service-owned GPUVM page tables",
    )
    parser.add_argument(
        "--hsakmt-setup-interrupt-ring-offset",
        type=_parse_int,
        default=0x1100000,
        help="VRAM offset for the modeled interrupt ring",
    )
    parser.add_argument(
        "--hsakmt-setup-interrupt-ring-bytes",
        type=_parse_int,
        default=0x10000,
        help="VRAM bytes reserved for the modeled interrupt ring",
    )
    parser.add_argument(
        "--hsakmt-setup-interrupt-wptr-offset",
        type=_parse_int,
        default=0x1110000,
        help="VRAM offset for the modeled interrupt write pointer",
    )
    parser.add_argument(
        "--hsakmt-setup-interrupt-doorbell-offset",
        type=_parse_int,
        default=0x101000,
        help="BAR2 doorbell offset used to consume interrupt entries",
    )
    parser.add_argument(
        "--hsakmt-setup-residency-offset",
        type=_parse_int,
        default=0x10000000,
        help="VRAM offset for exported application allocations",
    )
    parser.add_argument(
        "--hsakmt-setup-residency-bytes",
        type=_parse_int,
        default=0x20000000,
        help="VRAM bytes reserved for exported application allocations",
    )
    parser.add_argument(
        "--instantiate-only",
        action="store_true",
        default=True,
        help="Instantiate the rocjitsu-backed GPUFS graph and exit",
    )
    parser.add_argument(
        "--simulate",
        action="store_false",
        dest="instantiate_only",
        help="Run the rocjitsu-backed GPUFS simulation",
    )


def _prepare_gpufs_args(args):
    if ObjectList.is_kvm_cpu(ObjectList.cpu_list.get(args.cpu_type)):
        args.host_parallel = args.num_cpus > 1

    n_cu = args.num_compute_units
    args.num_sqc = int(math.ceil(float(n_cu) / args.cu_per_sqc))
    args.num_scalar_cache = int(
        math.ceil(float(n_cu) / args.cu_per_scalar_cache)
    )


def makeHsakmtServerGpuFSSystem(args):
    _prepare_gpufs_args(args)
    if "RocjitsuKmdServer" not in globals():
        m5.util.fatal(
            "This gem5 binary was built without ROCJITSU_KMD_PREFIX"
        )

    # GPU queue and DMA traffic uses Ruby. Completed Ruby requests are also
    # applied to the device AbstractMemory so the attached process observes
    # the same bytes through rocjitsu's shared backing descriptor.
    args.hsakmt_server_gpu_dma = True
    args.access_backing_store = True
    system = makeGpuFSSystem(args)
    system.workload = X86BareMetalWorkload(activate_bootstrap=False)
    system.readfile = ""

    shader = system.cpu[args.num_cpus]
    gpu = system.pc.south_bridge.gpu
    gpu_cmd_proc = gpu.cp

    shared_name = args.rocjitsu_vram_shared_backstore
    if not shared_name:
        shared_name = f"/gem5-rocjitsu-vram-{os.getpid()}"
    if not shared_name.startswith("/"):
        shared_name = "/" + shared_name
    gpu.shared_backstore = shared_name
    gpu.auto_unlink_shared_backstore = True

    system.hsakmt_server_platform = HsakmtServerPlatform(
        gpu_device=gpu,
        device_memories=gpu.memories,
    )
    system.hsakmt_server_platform.host_port = system.iobus.cpu_side_ports

    socket_path = args.hsakmt_rpc_socket
    if not socket_path:
        socket_path = os.path.join(args.rocjitsu_runtime_root, "daemon.sock")
    system.rocjitsu_kmd_server = RocjitsuKmdServer(
        runtime_root=args.rocjitsu_runtime_root,
        socket_path=socket_path,
        auto_start=args.hsakmt_auto_start,
        exit_on_disconnect=args.hsakmt_exit_on_rpc_disconnect,
        framebuffer_bar_pci_base=args.hsakmt_framebuffer_bar_base,
        doorbell_bar_pci_base=args.hsakmt_doorbell_bar_base,
        mmio_bar_pci_base=args.hsakmt_mmio_bar_base,
        setup_memory_base=args.hsakmt_setup_memory_base,
        page_table_offset=args.hsakmt_setup_page_table_device_offset,
        page_table_bytes=args.hsakmt_setup_page_table_bytes,
        interrupt_ring_offset=args.hsakmt_setup_interrupt_ring_offset,
        interrupt_ring_bytes=args.hsakmt_setup_interrupt_ring_bytes,
        interrupt_wptr_offset=args.hsakmt_setup_interrupt_wptr_offset,
        interrupt_doorbell_offset=(
            args.hsakmt_setup_interrupt_doorbell_offset
        ),
        allocation_offset=args.hsakmt_setup_residency_offset,
        allocation_bytes=args.hsakmt_setup_residency_bytes,
        gpu_device=gpu,
        sdma_engines=gpu.sdmas,
        server_platform=system.hsakmt_server_platform,
        hsapp=gpu_cmd_proc.hsapp,
        pm4_pkt_proc=gpu.pm4_pkt_procs[0],
        gpu_cmd_proc=gpu_cmd_proc,
        shader=shader,
    )
    return system


def runHsakmtServerGpuFSSystem(args):
    system = makeHsakmtServerGpuFSSystem(args)
    root = Root(
        full_system=True,
        system=system,
        time_sync_enable=True,
        time_sync_period="1000us",
    )

    if args.host_parallel:
        root.sim_quantum = int(1e8)
    if args.script is not None:
        system.readfile = args.script

    if args.restore_dir is None:
        m5.instantiate()
    else:
        m5.instantiate(args.restore_dir)

    if args.hsakmt_cold_checkpoint:
        if args.restore_dir is not None:
            m5.util.fatal(
                "--hsakmt-cold-checkpoint cannot be combined with --restore"
            )
        if args.hsakmt_auto_start:
            m5.util.fatal(
                "--hsakmt-cold-checkpoint requires the listener to remain "
                "unpublished; omit --hsakmt-auto-start"
            )
        m5.checkpoint(args.hsakmt_cold_checkpoint)
        print(f"Wrote cold rocjitsu checkpoint to {args.hsakmt_cold_checkpoint}")
        return

    if args.instantiate_only:
        print("Instantiated rocjitsu-backed GPUFS graph")
        return

    print("Running rocjitsu-backed GPUFS simulation")
    exit_event = m5.simulate(args.abs_max_tick)
    print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
    try:
        os.unlink(
            args.hsakmt_rpc_socket
            or os.path.join(args.rocjitsu_runtime_root, "daemon.sock")
        )
    except FileNotFoundError:
        pass


if __name__ == "__m5_main__":
    parser = argparse.ArgumentParser()
    addRunFSOptions(parser)
    addHsakmtServerOptions(parser)
    Options.addCommonOptions(parser)
    Ruby.define_options(parser)
    AmdGPUOptions.addAmdGPUOptions(parser)
    GPUTLBOptions.tlb_options(parser)

    runHsakmtServerGpuFSSystem(parser.parse_args())
