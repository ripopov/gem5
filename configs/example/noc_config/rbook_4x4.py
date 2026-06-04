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
    # SimpleNetwork PerfectSwitch port-buffer depth. Default 4 starves the
    # col-0 incast funnel (BW 18 B/cy); 8 keeps the 1-flit/cy funnel link fed
    # and reaches the structural ceiling (~28 B/cy). See the testbench
    # SimpleLatency.md for the SimpleNetwork buffer/stage diagram.
    router_buffer_size = 8

    router_link_latency = 2  # repeater delay between mesh routers
    node_router_latency = 2  # intermediate mux router

    link_bandwidth_factor = 40  # SimpleNetwork bytes/cycle per link
    # SimpleNetwork-only: PerfectSwitch per-hop routing latency (Cycles).
    # int = switch-to-switch hops, ext = switch-to-controller hops.
    int_routing_latency = router_latency
    ext_routing_latency = router_latency + 2

    # Model the mesh as four 2x2 clusters and add +5 cycles to every
    # mesh link that crosses a cluster boundary. CustomMesh._makeMesh
    # REPLACES router_link_latency with cross_link_latency on matched
    # links (it is not additive), so set it to router_link_latency + 5.
    # Entries are directed (src_router, dst_router) tuples; each physical
    # link is two directed int-links, so both orderings are listed.
    #
    #   0  1 | 2  3       clusters: TL={0,1,4,5}   TR={2,3,6,7}
    #   4  5 | 6  7                 BL={8,9,12,13} BR={10,11,14,15}
    #   -----+-----
    #   8  9 |10 11       boundary links cross col 1<->2 or row 1<->2
    #  12 13 |14 15
    cross_link_latency = router_link_latency + 5
    cross_links = [
        # vertical boundary: col 1 <-> col 2 (both directions)
        (1, 2), (2, 1), (5, 6), (6, 5),
        (9, 10), (10, 9), (13, 14), (14, 13),
        # horizontal boundary: row 1 <-> row 2 (both directions)
        (4, 8), (8, 4), (5, 9), (9, 5),
        (6, 10), (10, 6), (7, 11), (11, 7),
    ]


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
