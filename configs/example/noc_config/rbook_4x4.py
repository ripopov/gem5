# 4x4 CHI mesh noc_config for the ruby-book final project (Chapter 17).
#
# CustomMesh parameters for a 4x4 mesh. Routers are numbered in row-major
# order:
#
#  0 --- 1 --- 2 --- 3
#  |     |     |     |
#  4 --- 5 --- 6 --- 7
#  |     |     |     |
#  8 --- 9 ---10 ---11
#  |     |     |     |
# 12 ---13 ---14 ---15
#
# Each tile co-locates one RN-F (CPU) and one HN-F (LLC slice).
# Two SN-F (DDR) controllers sit at diagonally opposite corners (0 and 15).
# One MN handles DVM (architecturally idle in RISC-V SE mode).

from ruby import CHI_config


class NoC_Params(CHI_config.NoC_Params):
    num_rows = 4
    num_cols = 4
    router_latency = 4  # 1 clk input + 2 clk route + 1 clk output
    router_link_latency = 4  # repeater delay between mesh routers
    node_router_latency = 2  # intermediate mux router
    link_bandwidth_factor = 40  # SimpleNetwork bytes/cycle per link


class CHI_RNF(CHI_config.CHI_RNF):
    class NoC_Params(CHI_config.CHI_RNF.NoC_Params):
        router_list = list(range(16))


class CHI_HNF(CHI_config.CHI_HNF):
    class NoC_Params(CHI_config.CHI_HNF.NoC_Params):
        router_list = list(range(16))


class CHI_MN(CHI_config.CHI_MN):
    class NoC_Params(CHI_config.CHI_MN.NoC_Params):
        router_list = [0]


class CHI_SNF_MainMem(CHI_config.CHI_SNF_MainMem):
    class NoC_Params(CHI_config.CHI_SNF_MainMem.NoC_Params):
        router_list = [0, 15]


# --- Unused in SE mode, but CHI.py reads all seven class names at import ---


class CHI_SNF_BootMem(CHI_config.CHI_SNF_BootMem):
    class NoC_Params(CHI_config.CHI_SNF_BootMem.NoC_Params):
        router_list = [3]


class CHI_RNI_DMA(CHI_config.CHI_RNI_DMA):
    class NoC_Params(CHI_config.CHI_RNI_DMA.NoC_Params):
        router_list = [7]


class CHI_RNI_IO(CHI_config.CHI_RNI_IO):
    class NoC_Params(CHI_config.CHI_RNI_IO.NoC_Params):
        router_list = [7]
