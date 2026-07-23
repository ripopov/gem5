"""CHI node placement for the JitCPU 16-core 4x4 mesh regression."""

from ruby import CHI_config

setup_network_parameters = CHI_config.setup_network_parameters


# Main routers are row-major:
#
#   0 --- 1 --- 2 --- 3
#   |     |     |     |
#   4 --- 5 --- 6 --- 7
#   |     |     |     |
#   8 --- 9 ---10 ---11
#   |     |     |     |
#  12 ---13 ---14 ---15
#
# Each router has one RNF and one HNF. The four memory-side SNFs are placed
# at the corners, so both dimensions and all four quadrants carry memory
# traffic. The miscellaneous and I/O request nodes use opposite interior
# routers and do not create a memory-side hot spot.


class NoC_Params(CHI_config.NoC_Params):
    network = "simple"
    topology = "CustomMesh"
    num_rows = 4
    num_cols = 4


class CHI_RNF(CHI_config.CHI_RNF):
    class NoC_Params(CHI_config.CHI_RNF.NoC_Params):
        router_list = list(range(16))
        num_nodes_per_router = 1


class CHI_HNF(CHI_config.CHI_HNF):
    class NoC_Params(CHI_config.CHI_HNF.NoC_Params):
        router_list = list(range(16))
        num_nodes_per_router = 1


class CHI_MN(CHI_config.CHI_MN):
    class NoC_Params(CHI_config.CHI_MN.NoC_Params):
        router_list = [5]
        num_nodes_per_router = 1


class CHI_SNF_MainMem(CHI_config.CHI_SNF_MainMem):
    class NoC_Params(CHI_config.CHI_SNF_MainMem.NoC_Params):
        router_list = [0, 3, 12, 15]
        num_nodes_per_router = 1


class CHI_SNF_BootMem(CHI_config.CHI_SNF_BootMem):
    class NoC_Params(CHI_config.CHI_SNF_BootMem.NoC_Params):
        router_list = [0, 3, 12, 15]


class CHI_RNI_DMA(CHI_config.CHI_RNI_DMA):
    class NoC_Params(CHI_config.CHI_RNI_DMA.NoC_Params):
        router_list = [5, 10]


class CHI_RNI_IO(CHI_config.CHI_RNI_IO):
    class NoC_Params(CHI_config.CHI_RNI_IO.NoC_Params):
        router_list = [10]
        num_nodes_per_router = 1
