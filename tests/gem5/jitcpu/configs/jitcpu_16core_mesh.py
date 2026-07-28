# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

"""Shared setup and validation for the JitCPU 16-core CHI mesh tests."""

from pathlib import Path

NUM_CPUS = 16
NUM_HNFS = 16
NUM_DIRS = 4
MESH_ROWS = 4
MESH_COLS = 4
MAIN_ROUTERS = MESH_ROWS * MESH_COLS
REQUIRED_SET_BITS = 128
SNF_ROUTERS = (0, 3, 12, 15)
MN_ROUTER = 5
IO_ROUTER = 10
CHI_CONFIG = Path(__file__).with_name("chi_mesh_4x4.py").resolve()


def add_16core_mesh_option(parser):
    parser.add_argument(
        "--chi-4x4-mesh",
        action="store_true",
        help=(
            "configure 16 CPUs, 16 HNFs, four memory controllers, and the "
            "validated CHI SimpleNetwork 4x4 XY mesh"
        ),
    )


def apply_16core_mesh_options(args):
    if not args.chi_4x4_mesh:
        return

    args.ruby_chi = True
    args.ruby_network = "simple"
    args.num_cpus = NUM_CPUS
    args.num_l3caches = NUM_HNFS
    args.num_dirs = NUM_DIRS


def validate_16core_build(build_env):
    configured = int(build_env.get("NUMBER_BITS_PER_SET", 0))
    if configured < REQUIRED_SET_BITS:
        raise RuntimeError(
            "the 16-core CHI mesh requires NUMBER_BITS_PER_SET=128; run "
            "`scons setconfig build/RISCV NUMBER_BITS_PER_SET=128` and "
            "rebuild gem5"
        )


def configure_ruby_options(options, enabled):
    if not enabled:
        return

    options.network = "simple"
    options.topology = "CustomMesh"
    options.chi_config = str(CHI_CONFIG)


def _router_id(router):
    return int(router.router_id)


def _controller_router(node):
    controllers = node.getNetworkSideControllers()
    locations = {
        (int(controller._row), int(controller._col))
        for controller in controllers
    }
    if len(locations) != 1:
        raise RuntimeError(
            f"CHI node {type(node).__name__} spans routers {sorted(locations)}"
        )
    row, col = locations.pop()
    return row * MESH_COLS + col


def _require_node_mapping(nodes, expected, label):
    actual = [_controller_router(node) for node in nodes]
    if actual != list(expected):
        raise RuntimeError(
            f"{label} router mapping is {actual}, expected {list(expected)}"
        )
    print(
        f"4x4 mesh {label} mapping: "
        + ", ".join(
            f"{index}->R{router}" for index, router in enumerate(actual)
        )
    )


def validate_16core_mesh(system):
    """Prove that the instantiated network is the intended 4x4 XY mesh."""

    network = system.ruby.network
    if type(network).__name__ != "SimpleNetwork":
        raise RuntimeError(
            f"16-core mesh requires SimpleNetwork, found {type(network).__name__}"
        )
    if str(network.topology) != "CustomMesh":
        raise RuntimeError(
            f"16-core mesh requires CustomMesh, found {network.topology}"
        )

    main_routers = [
        router for router in network.routers if getattr(router, "_main", False)
    ]
    main_ids = sorted(_router_id(router) for router in main_routers)
    if main_ids != list(range(MAIN_ROUTERS)):
        raise RuntimeError(
            f"main mesh routers are {main_ids}, expected 0..{MAIN_ROUTERS - 1}"
        )

    expected_links = set()
    expected_weights = {}
    for row in range(MESH_ROWS):
        for col in range(MESH_COLS):
            router = row * MESH_COLS + col
            if col + 1 < MESH_COLS:
                neighbor = router + 1
                expected_links.update(((router, neighbor), (neighbor, router)))
                expected_weights[(router, neighbor)] = 1
                expected_weights[(neighbor, router)] = 1
            if row + 1 < MESH_ROWS:
                neighbor = router + MESH_COLS
                expected_links.update(((router, neighbor), (neighbor, router)))
                expected_weights[(router, neighbor)] = 2
                expected_weights[(neighbor, router)] = 2

    mesh_links = {}
    for link in network.int_links:
        if not (
            getattr(link.src_node, "_main", False)
            and getattr(link.dst_node, "_main", False)
        ):
            continue
        edge = (_router_id(link.src_node), _router_id(link.dst_node))
        if edge in mesh_links:
            raise RuntimeError(f"duplicate main-router mesh link {edge}")
        mesh_links[edge] = int(link.weight)

    if set(mesh_links) != expected_links:
        missing = sorted(expected_links - set(mesh_links))
        extra = sorted(set(mesh_links) - expected_links)
        raise RuntimeError(
            f"4x4 XY mesh link mismatch: missing={missing}, extra={extra}"
        )
    wrong_weights = [
        (edge, mesh_links[edge], expected_weights[edge])
        for edge in sorted(expected_links)
        if mesh_links[edge] != expected_weights[edge]
    ]
    if wrong_weights:
        raise RuntimeError(
            f"4x4 XY mesh link weight mismatch: {wrong_weights}"
        )

    _require_node_mapping(system.ruby.rnf, range(16), "RNF")
    _require_node_mapping(system.ruby.hnf, range(16), "HNF")
    _require_node_mapping(system.ruby.snf, SNF_ROUTERS, "SNF")
    _require_node_mapping(system.ruby.mn, (MN_ROUTER,), "MN")
    _require_node_mapping((system.ruby.io_rni,), (IO_ROUTER,), "I/O RNI")

    print(
        "Validated CHI SimpleNetwork 4x4 XY mesh: "
        f"{len(main_routers)} main routers, {len(mesh_links)} directed XY "
        f"links, {len(system.ruby.rnf)} RNFs, {len(system.ruby.hnf)} HNFs, "
        f"and {len(system.ruby.snf)} SNFs"
    )
