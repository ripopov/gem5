from m5.params import *
from m5.proxy import Parent
from m5.SimObject import SimObject


class HsakmtServerPlatform(SimObject):
    type = "HsakmtServerPlatform"
    cxx_header = "dev/hsakmt-service/service/hsakmt_server_platform.hh"
    cxx_class = "gem5::hsa::HsakmtServerPlatform"

    host_port = RequestPort("Host-side request port for PCI config and BAR PIO")
    system = Param.System(Parent.any, "System containing the server platform")
    gpu_device = Param.AMDGPUDevice(NULL, "AMDGPUDevice enumerated by platform")
    device_memories = VectorParam.AbstractMemory(
        [], "GPU device-memory objects made reachable for server DMA requestors"
    )
    pci_config_base = Param.Addr(
        "0xC000000000000000", "Physical base of PCI config space"
    )
    pci_config_device_bits = Param.UInt8(
        8, "Number of config-space bits allocated per PCI function"
    )
    gpu_pci_bus = Param.UInt8(0, "PCI bus number for the server GPU")


class RocjitsuKmdServer(SimObject):
    type = "RocjitsuKmdServer"
    cxx_header = "dev/hsakmt-service/service/rocjitsu_kmd_server.hh"
    cxx_class = "gem5::hsa::RocjitsuKmdServer"

    runtime_root = Param.String(
        "/run/rocjitsu", "Shared runtime root for the socket and topology"
    )
    socket_path = Param.String(
        "/run/rocjitsu/daemon.sock", "rocjitsu KMD RPC endpoint"
    )
    auto_start = Param.Bool(True, "Create the KMD endpoint during startup")
    exit_on_disconnect = Param.Bool(
        True, "Exit after the single supported client disconnects"
    )

    framebuffer_bar_pci_base = Param.Addr(
        "0x2400000000", "Server-mode PCI base for AMDGPU BAR0"
    )
    doorbell_bar_pci_base = Param.Addr(
        "0x2200000000", "Server-mode PCI base for AMDGPU BAR2"
    )
    mmio_bar_pci_base = Param.Addr(
        "0xecf00000", "Server-mode PCI base for AMDGPU BAR5"
    )
    setup_memory_base = Param.Addr(
        "0x70000000", "GPU VA base of service-owned control memory"
    )
    page_table_offset = Param.Addr(
        "0x1000000", "VRAM offset of the direct GPUVM page-table arena"
    )
    page_table_bytes = Param.UInt64(
        1048576, "Size of the direct GPUVM page-table arena"
    )
    interrupt_ring_offset = Param.Addr(
        "0x1100000", "VRAM offset of the modeled IH ring"
    )
    interrupt_ring_bytes = Param.UInt64(
        65536, "Size of the modeled IH ring"
    )
    interrupt_wptr_offset = Param.Addr(
        "0x1110000", "VRAM offset of the modeled IH write pointer"
    )
    interrupt_doorbell_offset = Param.Addr(
        "0x101000", "BAR2 offset used to consume IH entries"
    )
    queue_doorbell_base = Param.Addr(
        "0x4000", "BAR2 base added to rocjitsu queue doorbell offsets"
    )
    interrupt_poll_interval = Param.Tick(
        1000, "Simulated ticks between IH consumer polls"
    )
    allocation_offset = Param.Addr(
        "0x10000000", "VRAM offset of the exported allocation heap"
    )
    allocation_bytes = Param.UInt64(
        536870912, "Size of the exported allocation heap"
    )
    allocation_alignment = Param.UInt64(
        4096, "Minimum exported allocation alignment"
    )
    process_pasid = Param.UInt16(
        0x8000, "PASID used by the first supported attached process"
    )

    # Defaults describe the MI300X/gfx942 target used by the lab bundle.
    architecture = Param.UInt32(2, "rocjitsu architecture enum (CDNA3)")
    gpu_id = Param.UInt32(50148, "KFD topology GPU identifier")
    gfx_target_version = Param.UInt32(90402, "KFD gfx target version")
    vendor_id = Param.UInt32(0x1002, "PCI vendor identifier")
    device_id = Param.UInt32(0x74A1, "PCI device identifier")
    family_id = Param.UInt32(146, "KFD family identifier")
    drm_render_minor = Param.UInt32(128, "Synthetic DRM render minor")
    simd_count = Param.UInt32(1280, "Topology SIMD count")
    max_waves_per_simd = Param.UInt32(8, "Topology wave slots per SIMD")
    num_shader_engines = Param.UInt32(4, "Topology shader-engine count")
    num_shader_arrays_per_engine = Param.UInt32(
        1, "Topology shader arrays per engine"
    )
    num_cu_per_sh = Param.UInt32(10, "Topology compute units per array")
    simd_per_cu = Param.UInt32(4, "Topology SIMD count per CU")
    wave_front_size = Param.UInt32(64, "Topology wavefront size")
    num_xcc = Param.UInt32(8, "Topology XCC count")
    max_slots_scratch_cu = Param.UInt32(32, "Scratch slots per CU")
    local_mem_size = Param.MemorySize("192GiB", "Reported local memory")
    lds_size_kb = Param.UInt32(64, "Reported LDS size in KiB")
    num_sdma_engines = Param.UInt32(4, "Reported SDMA engine count")
    num_cp_queues = Param.UInt32(128, "Reported command queue count")
    capability = Param.UInt32(
        0xFC37E4A2,
        (
            "KFD topology capability bits; the direct backend supports "
            "shared SVM registration but not migration or memory advice"
        ),
    )
    capability2 = Param.UInt32(0, "Extended KFD topology capability bits")
    marketing_name = Param.String("AMD Instinct MI300X", "Topology name")

    gpu_device = Param.AMDGPUDevice(NULL, "AMDGPUDevice driven by the KMD")
    sdma_engines = VectorParam.SDMAEngine(
        [], "SDMA engines whose DMA traffic targets exported GPU memory"
    )
    server_platform = Param.HsakmtServerPlatform(
        NULL, "CPU-less PCI/BAR platform"
    )
    hsapp = Param.HSAPacketProcessor(NULL, "HSA packet processor")
    pm4_pkt_proc = Param.PM4PacketProcessor(NULL, "PM4 packet processor")
    gpu_cmd_proc = Param.GPUCommandProcessor(NULL, "GPU command processor")
    shader = Param.Shader(NULL, "GPU shader")
