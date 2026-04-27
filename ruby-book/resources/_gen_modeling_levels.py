#!/usr/bin/env python3
"""Generate gem5_modeling_levels.drawio.

Three columns side-by-side, each showing the same logical hardware
(cores, L1 caches, interconnect, home/memory) at a different gem5
modeling fidelity. The interconnect block opens up progressively to
visualise where simulation detail lives.
"""

from html import escape as h
from pathlib import Path

OUT = Path(__file__).resolve().parent / "gem5_modeling_levels.drawio"

# Page geometry
PAGE_W, PAGE_H = 1320, 600

# Subtitle (no title — slide H2 handles that)
SUBTITLE_Y = 14

# Panel geometry
PANEL_Y = 50
PANEL_H = 540

# Three panels
PANEL_W = 410
PANEL_GAP = 25
PANEL_X = [
    20,
    20 + PANEL_W + PANEL_GAP,
    20 + 2 * (PANEL_W + PANEL_GAP),
]

# Bottom fidelity ribbon
RIBBON_Y = PANEL_Y + PANEL_H + 14
RIBBON_H = 38

# Color tokens
BLUE_F, BLUE_S, BLUE_INK = "#DBEAFE", "#2563eb", "#1d4ed8"
VIOLET_F, VIOLET_S, VIOLET_INK = "#EDE9FE", "#7c3aed", "#6d28d9"
GREEN_F, GREEN_S, GREEN_INK = "#DCFCE7", "#15803d", "#166534"
GOLD_F, GOLD_S, GOLD_INK = "#FEF3C7", "#c58b1b", "#92400e"
SLATE_F, SLATE_S, SLATE_INK = "#F1F5F9", "#475569", "#334155"
TEAL_F, TEAL_S, TEAL_INK = "#CCFBF1", "#0f766e", "#115e59"
ROSE_F, ROSE_S, ROSE_INK = "#FFE4E6", "#be123c", "#9f1239"
WHITE = "#FFFFFF"
LIGHT_TINT = "#F8FAFC"


def attr_esc(s):
    """Escape a string for use inside an XML attribute value.

    drawio embeds HTML inside the `value=` attribute, so `<`, `>`, `&`,
    and `"` must all be entity-encoded.
    """
    return (
        s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def cell(cid, value, x, y, w, ht, style, vertex=True):
    kind = 'vertex="1"' if vertex else 'edge="1"'
    return (
        f'<mxCell id="{cid}" value="{attr_esc(value)}" '
        f'style="{style}" {kind} parent="1">\n'
        f'  <mxGeometry x="{x}" y="{y}" width="{w}" height="{ht}" '
        f'as="geometry"/>\n'
        f"</mxCell>"
    )


def edge(cid, src, dst, style):
    return (
        f'<mxCell id="{cid}" style="{style}" edge="1" source="{src}" '
        f'target="{dst}" parent="1">\n'
        f'  <mxGeometry relative="1" as="geometry"/>\n'
        f"</mxCell>"
    )


def text(
    cid,
    value,
    x,
    y,
    w,
    ht,
    font_size=12,
    align="center",
    color="#16324f",
    weight="normal",
    italic=False,
):
    style_extra = ""
    if italic:
        style_extra += "fontStyle=2;"
    style = (
        f"text;html=1;align={align};verticalAlign=middle;resizable=0;"
        f"points=[];autosize=0;strokeColor=none;fillColor=none;"
        f"fontSize={font_size};fontColor={color};{style_extra}"
    )
    val = f"<b>{value}</b>" if weight == "bold" else value
    return cell(cid, val, x, y, w, ht, style)


def box(
    cid,
    label,
    sublabel,
    x,
    y,
    w,
    ht,
    fill,
    stroke,
    ink,
    bold_size=14,
    sub_size=10,
    sub_color=None,
    accent_top=False,
):
    sub_color = sub_color or "#475569"
    accent = f"borderTop=1;" if accent_top else ""
    inner = f'<b style="font-size:{bold_size}px">{h(label)}</b>'
    if sublabel:
        inner += (
            f'<div style="font-size:{sub_size}px;color:{sub_color};'
            f'margin-top:3px;line-height:1.25;font-weight:normal">'
            f"{sublabel}</div>"
        )
    style = (
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={fill};"
        f"strokeColor={stroke};strokeWidth=2;verticalAlign=middle;"
        f"fontColor={ink};arcSize=10;{accent}"
    )
    return cell(cid, inner, x, y, w, ht, style)


def panel_header(
    cid, name, source_path, x, y, w, ht, fill, stroke, ink, badge=None
):
    badge_html = ""
    if badge:
        badge_html = (
            f'<span style="display:inline-block;background:{ink};'
            f"color:#FFFFFF;font-size:11px;font-weight:700;padding:2px 8px;"
            f'border-radius:999px;margin-left:10px;vertical-align:middle">'
            f"{h(badge)}</span>"
        )
    inner = (
        f'<div style="text-align:left;line-height:1.15">'
        f'<span style="font-size:18px;font-weight:800;letter-spacing:0.02em">'
        f"{h(name)}</span>{badge_html}"
        f'<div style="font-size:11px;color:{ink};opacity:0.85;'
        f'font-family:monospace;margin-top:4px">{h(source_path)}</div>'
        f"</div>"
    )
    style = (
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={fill};"
        f"strokeColor={stroke};strokeWidth=2;verticalAlign=middle;"
        f"fontColor={ink};arcSize=8;align=left;spacingLeft=14;"
    )
    return cell(cid, inner, x, y, w, ht, style)


def panel_frame(cid, x, y, w, ht, stroke, tint="#FFFFFF"):
    style = (
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={tint};"
        f"strokeColor={stroke};strokeWidth=1.5;dashed=0;arcSize=4;"
    )
    return cell(cid, "", x, y, w, ht, style)


def icn_band(cid, label, x, y, w, fill, stroke, ink):
    """A horizontal pill/band that flags the interconnect row."""
    style = (
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={fill};"
        f"strokeColor={stroke};strokeWidth=1.5;fontColor={ink};"
        f"fontSize=10;verticalAlign=middle;arcSize=40;"
    )
    inner = (
        f'<span style="font-family:Inter,sans-serif;'
        f"text-transform:uppercase;letter-spacing:0.14em;"
        f'font-size:11px;font-weight:800">{label}</span>'
    )
    return cell(cid, inner, x, y, w, 22, style)


def vline(
    cid,
    src,
    dst,
    color="#94a3b8",
    width=1.5,
    dashed=False,
    exit_x=0.5,
    entry_x=0.5,
):
    """Straight vertical line. exit_x / entry_x are 0..1 fractions
    on the source's bottom edge and the target's top edge — set them
    so the line drops straight without any horizontal jog."""
    style = (
        f"endArrow=none;startArrow=none;strokeWidth={width};"
        f"strokeColor={color};exitX={exit_x};exitY=1;exitDx=0;exitDy=0;"
        f"entryX={entry_x};entryY=0;entryDx=0;entryDy=0;"
    )
    if dashed:
        style += "dashed=1;"
    return edge(cid, src, dst, style)


def harrow(cid, src, dst, color="#475569", width=1.5, label=""):
    style = (
        f"endArrow=classic;startArrow=none;strokeWidth={width};"
        f"strokeColor={color};exitX=1;exitY=0.5;exitDx=0;exitDy=0;"
        f"entryX=0;entryY=0.5;entryDx=0;entryDy=0;fontSize=10;"
        f"fontColor={color};"
    )
    if label:
        return (
            f'<mxCell id="{cid}" value="{attr_esc(label)}" '
            f'style="{style}" edge="1" source="{src}" target="{dst}" '
            f'parent="1">\n'
            f'  <mxGeometry relative="1" as="geometry"/>\n'
            f"</mxCell>"
        )
    return edge(cid, src, dst, style)


cells = []


# ---- Subtitle (slide H2 carries the title) --------------------------
cells.append(
    text(
        "subtitle",
        "Same logical hardware · three precision levels — "
        "pick the stack that matches the question you are asking",
        0,
        SUBTITLE_Y,
        PAGE_W,
        22,
        font_size=13,
        italic=True,
        color="#475569",
    )
)


# ---- Panel layout helper --------------------------------------------
def panel_layout(panel_idx):
    """Return y-offsets for the elements inside a panel."""
    px = PANEL_X[panel_idx]
    py = PANEL_Y
    return {
        "px": px,
        "py": py,
        "header_y": py + 10,
        "header_h": 44,
        "core_y": py + 70,
        "core_h": 38,
        "cache_y": py + 124,
        "cache_h": 52,
        "icn_y": py + 196,
        "icn_h": 260,
        "home_y": py + 472,
        "home_h": 56,
    }


# ---- Panel 1: Classic -----------------------------------------------
P = panel_layout(0)
px = P["px"]
cells.append(
    panel_frame(
        "p1_frame", px, PANEL_Y, PANEL_W, PANEL_H, SLATE_S, tint="#FAFBFC"
    )
)
cells.append(
    panel_header(
        "p1_hdr",
        "Classic",
        "src/mem/cache/ · coherent_xbar.cc",
        px + 8,
        P["header_y"],
        PANEL_W - 16,
        P["header_h"],
        SLATE_F,
        SLATE_S,
        SLATE_INK,
    )
)

# Cores
core_w = (PANEL_W - 16 - 16) // 2  # gap of 16 between cores
cells.append(
    box(
        "p1_core0",
        "Core 0",
        "",
        px + 8,
        P["core_y"],
        core_w,
        P["core_h"],
        BLUE_F,
        BLUE_S,
        BLUE_INK,
        bold_size=14,
        sub_size=10,
    )
)
cells.append(
    box(
        "p1_core1",
        "Core 1",
        "",
        px + 8 + core_w + 16,
        P["core_y"],
        core_w,
        P["core_h"],
        BLUE_F,
        BLUE_S,
        BLUE_INK,
        bold_size=14,
        sub_size=10,
    )
)
# L1 caches (classic) — MOESI state lives here, in the block flags.
# This is the visually-prominent "rigid" element of the Classic stack.
cells.append(
    box(
        "p1_l1a",
        "L1 cache",
        '<b style="color:#9f1239">MOESI</b> = '
        '<code style="font-size:9px">valid·writable·dirty</code> bits<br>'
        '<span style="font-size:9px;color:#475569">'
        'transitions in <code style="font-size:9px">Cache::access</code> / '
        '<code style="font-size:9px">handleSnoop</code></span>',
        px + 8,
        P["cache_y"] - 4,
        core_w,
        P["cache_h"] + 12,
        "#FECACA",
        ROSE_S,
        ROSE_INK,
        bold_size=12,
        sub_size=10,
    )
)
cells.append(
    box(
        "p1_l1b",
        "L1 cache",
        '<b style="color:#9f1239">MOESI</b> = '
        '<code style="font-size:9px">valid·writable·dirty</code> bits<br>'
        '<span style="font-size:9px;color:#475569">'
        'transitions in <code style="font-size:9px">Cache::access</code> / '
        '<code style="font-size:9px">handleSnoop</code></span>',
        px + 8 + core_w + 16,
        P["cache_y"] - 4,
        core_w,
        P["cache_h"] + 12,
        "#FECACA",
        ROSE_S,
        ROSE_INK,
        bold_size=12,
        sub_size=10,
    )
)
# Interconnect: CoherentXBar is the broadcast / filter fabric — it
# carries the snoop traffic, it does not own coherence state.
icn_x = px + 8
icn_w = PANEL_W - 16
xbar_h = 130
cells.append(
    box(
        "p1_xbar",
        "CoherentXBar",
        '<span style="font-size:11px">broadcast snoop · '
        '<code style="font-size:9px">forwardTiming()</code></span>'
        '<br><span style="font-size:10px;color:#475569">'
        '+ optional <code style="font-size:9px">SnoopFilter</code> '
        "(presence-tracking bitmasks)</span>"
        '<br><span style="color:#94a3b8;font-size:9px">'
        "no protocol FSM · no virtual channels · no flits</span>",
        icn_x,
        P["icn_y"] + 50,
        icn_w,
        xbar_h,
        SLATE_F,
        SLATE_S,
        SLATE_INK,
        bold_size=14,
        sub_size=11,
    )
)
# Interconnect band header
cells.append(
    icn_band(
        "p1_icn_band",
        "interconnect",
        icn_x + 100,
        P["icn_y"] + 8,
        icn_w - 200,
        SLATE_F,
        SLATE_S,
        SLATE_INK,
    )
)
cells.append(
    text(
        "p1_icn_caption",
        '<span style="color:#94a3b8;font-size:10px;font-style:italic">'
        "broadcast fabric · presence-tracking filter, no microarchitecture"
        "</span>",
        icn_x,
        P["icn_y"] + 32,
        icn_w,
        14,
        font_size=10,
        color="#94a3b8",
    )
)
# Memory
cells.append(
    box(
        "p1_mem",
        "L2 + Memory Controller",
        "DRAM model below",
        icn_x,
        P["home_y"],
        icn_w,
        P["home_h"],
        GREEN_F,
        GREEN_S,
        GREEN_INK,
        bold_size=13,
        sub_size=10,
    )
)
# Connections — straight verticals (left-side and right-side enter
# the wide interconnect / memory boxes at matching x fractions)
cells.append(vline("p1_e1", "p1_core0", "p1_l1a"))
cells.append(vline("p1_e2", "p1_core1", "p1_l1b"))
cells.append(vline("p1_e3", "p1_l1a", "p1_xbar", exit_x=0.5, entry_x=0.25))
cells.append(vline("p1_e4", "p1_l1b", "p1_xbar", exit_x=0.5, entry_x=0.75))
cells.append(vline("p1_e5a", "p1_xbar", "p1_mem", exit_x=0.25, entry_x=0.25))
cells.append(vline("p1_e5b", "p1_xbar", "p1_mem", exit_x=0.75, entry_x=0.75))


# ---- Panel 2: Ruby + SimpleNetwork ----------------------------------
P = panel_layout(1)
px = P["px"]
cells.append(
    panel_frame(
        "p2_frame", px, PANEL_Y, PANEL_W, PANEL_H, BLUE_S, tint="#F7FAFE"
    )
)
cells.append(
    panel_header(
        "p2_hdr",
        "Ruby + SimpleNetwork",
        "src/mem/ruby/ · network/simple/",
        px + 8,
        P["header_y"],
        PANEL_W - 16,
        P["header_h"],
        BLUE_F,
        BLUE_S,
        BLUE_INK,
    )
)
# Cores
cells.append(
    box(
        "p2_core0",
        "Core 0",
        "",
        px + 8,
        P["core_y"],
        core_w,
        P["core_h"],
        BLUE_F,
        BLUE_S,
        BLUE_INK,
        bold_size=14,
        sub_size=10,
    )
)
cells.append(
    box(
        "p2_core1",
        "Core 1",
        "",
        px + 8 + core_w + 16,
        P["core_y"],
        core_w,
        P["core_h"],
        BLUE_F,
        BLUE_S,
        BLUE_INK,
        bold_size=14,
        sub_size=10,
    )
)
# L1 SLICC controllers
cells.append(
    box(
        "p2_l1a",
        "L1 SLICC ctrl",
        '<code style="font-size:9px">.sm</code> FSM · '
        "states / events / actions",
        px + 8,
        P["cache_y"],
        core_w,
        P["cache_h"],
        GOLD_F,
        GOLD_S,
        GOLD_INK,
        bold_size=12,
        sub_size=10,
    )
)
cells.append(
    box(
        "p2_l1b",
        "L1 SLICC ctrl",
        '<code style="font-size:9px">.sm</code> FSM · '
        "states / events / actions",
        px + 8 + core_w + 16,
        P["cache_y"],
        core_w,
        P["cache_h"],
        GOLD_F,
        GOLD_S,
        GOLD_INK,
        bold_size=12,
        sub_size=10,
    )
)
# Interconnect band header
cells.append(
    icn_band(
        "p2_icn_band",
        "interconnect",
        px + 8 + 100,
        P["icn_y"] + 8,
        PANEL_W - 16 - 200,
        BLUE_F,
        BLUE_S,
        BLUE_INK,
    )
)
cells.append(
    text(
        "p2_icn_caption",
        '<span style="color:#94a3b8;font-size:10px;font-style:italic">'
        "analytical: per-link latency + bandwidth queues</span>",
        px + 8,
        P["icn_y"] + 32,
        PANEL_W - 16,
        14,
        font_size=10,
        color="#94a3b8",
    )
)
# SimpleNetwork — Switch + Throttle. Inside the box we show four
# horizontal vnet lanes (REQ/SNP/RSP/DAT) each with a small queue
# of MessageBuffer slots, because the actual queues live per
# (output port × vnet) inside the Switch.
sn_x = px + 8
sn_w = PANEL_W - 16
sn_h = 150
sn_y = P["icn_y"] + 50
cells.append(
    cell(
        "p2_sn",
        "",
        sn_x,
        sn_y,
        sn_w,
        sn_h,
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={BLUE_F};"
        f"strokeColor={BLUE_S};strokeWidth=2;arcSize=10;",
    )
)
cells.append(
    text(
        "p2_sn_title",
        '<b style="font-size:13px;color:#1d4ed8">SimpleNetwork</b>'
        '<span style="font-size:10px;color:#475569"> · Switch + Throttle</span>',
        sn_x + 8,
        sn_y + 4,
        sn_w - 16,
        16,
        font_size=11,
        align="center",
        color="#1d4ed8",
    )
)
# Four vnet lanes — REQ/SNP/RSP/DAT — each a row of small slots.
vnet_names = [
    ("REQ", BLUE_F, BLUE_S, BLUE_INK),
    ("SNP", "#FEF3C7", GOLD_S, GOLD_INK),
    ("RSP", "#DCFCE7", GREEN_S, GREEN_INK),
    ("DAT", "#EDE9FE", VIOLET_S, VIOLET_INK),
]
lane_top = sn_y + 22
lane_h = 18
lane_gap = 4
slot_w = 14
slot_count = 4
for i, (vname, vfill, vstroke, vink) in enumerate(vnet_names):
    ly = lane_top + i * (lane_h + lane_gap)
    # Lane label on the left
    cells.append(
        text(
            f"p2_lane_lbl_{i}",
            f'<b style="font-size:10px;color:{vink}">{vname}</b>',
            sn_x + 6,
            ly,
            30,
            lane_h,
            font_size=10,
            align="left",
            color=vink,
        )
    )
    # Queue slots
    qx0 = sn_x + 42
    for s in range(slot_count):
        cells.append(
            cell(
                f"p2_lane_{i}_slot_{s}",
                "",
                qx0 + s * (slot_w + 2),
                ly + 2,
                slot_w,
                lane_h - 4,
                f"rounded=1;whiteSpace=wrap;html=1;fillColor=#FFFFFF;"
                f"strokeColor={vstroke};strokeWidth=1;arcSize=30;",
            )
        )
    # Throttle gate at the right end
    tx = qx0 + slot_count * (slot_w + 2) + 8
    cells.append(
        cell(
            f"p2_lane_throttle_{i}",
            "",
            tx,
            ly + 2,
            18,
            lane_h - 4,
            f"rounded=0;whiteSpace=wrap;html=1;fillColor={vstroke};"
            f"strokeColor={vstroke};",
        )
    )
    # bw label after throttle
    cells.append(
        text(
            f"p2_lane_bw_{i}",
            f'<span style="font-size:9px;color:#64748b;font-style:italic">bw</span>',
            tx + 22,
            ly,
            sn_w - (tx + 22 - sn_x) - 6,
            lane_h,
            font_size=9,
            align="left",
            color="#64748b",
        )
    )
# Footer text inside the SimpleNetwork box
cells.append(
    text(
        "p2_sn_foot",
        '<span style="font-size:9px;color:#475569">'
        "queue per (output port × vnet) · "
        '<code style="font-size:8px">physical_vnets_channels</code> '
        "splits bw per vnet</span>",
        sn_x + 4,
        sn_y + sn_h - 16,
        sn_w - 8,
        12,
        font_size=9,
        align="center",
        color="#475569",
    )
)
# Home + memory
cells.append(
    box(
        "p2_home",
        "HN-F SLICC ctrl + DRAM",
        "directory FSM · memory below",
        sn_x,
        P["home_y"],
        sn_w,
        P["home_h"],
        VIOLET_F,
        VIOLET_S,
        VIOLET_INK,
        bold_size=13,
        sub_size=10,
    )
)
cells.append(vline("p2_e1", "p2_core0", "p2_l1a"))
cells.append(vline("p2_e2", "p2_core1", "p2_l1b"))
cells.append(vline("p2_e3", "p2_l1a", "p2_sn", exit_x=0.5, entry_x=0.25))
cells.append(vline("p2_e4", "p2_l1b", "p2_sn", exit_x=0.5, entry_x=0.75))
cells.append(vline("p2_e5a", "p2_sn", "p2_home", exit_x=0.25, entry_x=0.25))
cells.append(vline("p2_e5b", "p2_sn", "p2_home", exit_x=0.75, entry_x=0.75))


# ---- Panel 3: Ruby + Garnet -----------------------------------------
P = panel_layout(2)
px = P["px"]
cells.append(
    panel_frame(
        "p3_frame", px, PANEL_Y, PANEL_W, PANEL_H, VIOLET_S, tint="#FAF8FE"
    )
)
cells.append(
    panel_header(
        "p3_hdr",
        "Ruby + Garnet",
        "src/mem/ruby/ · network/garnet/",
        px + 8,
        P["header_y"],
        PANEL_W - 16,
        P["header_h"],
        VIOLET_F,
        VIOLET_S,
        VIOLET_INK,
    )
)
# Cores
cells.append(
    box(
        "p3_core0",
        "Core 0",
        "",
        px + 8,
        P["core_y"],
        core_w,
        P["core_h"],
        BLUE_F,
        BLUE_S,
        BLUE_INK,
        bold_size=14,
        sub_size=10,
    )
)
cells.append(
    box(
        "p3_core1",
        "Core 1",
        "",
        px + 8 + core_w + 16,
        P["core_y"],
        core_w,
        P["core_h"],
        BLUE_F,
        BLUE_S,
        BLUE_INK,
        bold_size=14,
        sub_size=10,
    )
)
# L1 SLICC ctrl + NI label
cells.append(
    box(
        "p3_l1a",
        "L1 SLICC ctrl",
        'CHI-cache <code style="font-size:9px">.sm</code> · ' "4 VNets out",
        px + 8,
        P["cache_y"],
        core_w,
        P["cache_h"],
        GOLD_F,
        GOLD_S,
        GOLD_INK,
        bold_size=12,
        sub_size=10,
    )
)
cells.append(
    box(
        "p3_l1b",
        "L1 SLICC ctrl",
        'CHI-cache <code style="font-size:9px">.sm</code> · ' "4 VNets out",
        px + 8 + core_w + 16,
        P["cache_y"],
        core_w,
        P["cache_h"],
        GOLD_F,
        GOLD_S,
        GOLD_INK,
        bold_size=12,
        sub_size=10,
    )
)
# Interconnect band header
cells.append(
    icn_band(
        "p3_icn_band",
        "interconnect",
        px + 8 + 100,
        P["icn_y"] + 8,
        PANEL_W - 16 - 200,
        VIOLET_F,
        VIOLET_S,
        VIOLET_INK,
    )
)
cells.append(
    text(
        "p3_icn_caption",
        '<span style="color:#94a3b8;font-size:10px;font-style:italic">'
        "cycle-accurate routers · VCs · credits · flits</span>",
        px + 8,
        P["icn_y"] + 32,
        PANEL_W - 16,
        14,
        font_size=10,
        color="#94a3b8",
    )
)
# Garnet routers — show 2 routers with internal pipeline pills
icn_top = P["icn_y"] + 46
ni_h = 22
ni_w = (PANEL_W - 16 - 16) // 2
cells.append(
    cell(
        "p3_ni0",
        '<b style="font-size:10px">NI</b>'
        '<span style="font-size:9px;color:#475569"> · flitisize · VC alloc</span>',
        px + 8,
        icn_top,
        ni_w,
        ni_h,
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={TEAL_F};"
        f"strokeColor={TEAL_S};fontColor={TEAL_INK};fontSize=10;"
        f"verticalAlign=middle;arcSize=20;",
    )
)
cells.append(
    cell(
        "p3_ni1",
        '<b style="font-size:10px">NI</b>'
        '<span style="font-size:9px;color:#475569"> · flitisize · VC alloc</span>',
        px + 8 + ni_w + 16,
        icn_top,
        ni_w,
        ni_h,
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={TEAL_F};"
        f"strokeColor={TEAL_S};fontColor={TEAL_INK};fontSize=10;"
        f"verticalAlign=middle;arcSize=20;",
    )
)

# Two routers (R0, R1) shown side by side with stages
router_y = icn_top + ni_h + 16
router_h = 105
router_w = (PANEL_W - 16 - 16) // 2
r0_x = px + 8
r1_x = px + 8 + router_w + 16
cells.append(
    cell(
        "p3_r0",
        '<div style="text-align:center"><b style="font-size:11px">'
        "Router R0</b></div>"
        '<div style="font-size:9px;color:#64748b;text-align:center;'
        'margin-top:1px">5-port · per-input VCs</div>',
        r0_x,
        router_y,
        router_w,
        router_h,
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={WHITE};"
        f"strokeColor={VIOLET_S};fontColor={VIOLET_INK};fontSize=10;"
        f"verticalAlign=top;arcSize=8;strokeWidth=2;spacingTop=4;",
    )
)
cells.append(
    cell(
        "p3_r1",
        '<div style="text-align:center"><b style="font-size:11px">'
        "Router R1</b></div>"
        '<div style="font-size:9px;color:#64748b;text-align:center;'
        'margin-top:1px">5-port · per-input VCs</div>',
        r1_x,
        router_y,
        router_w,
        router_h,
        f"rounded=1;whiteSpace=wrap;html=1;fillColor={WHITE};"
        f"strokeColor={VIOLET_S};fontColor={VIOLET_INK};fontSize=10;"
        f"verticalAlign=top;arcSize=8;strokeWidth=2;spacingTop=4;",
    )
)


# Pipeline pills inside each router
def pipeline_pills(prefix, rx, ry, rw):
    pills = ["RC", "VA", "SA", "XB"]
    pill_w = 32
    pill_h = 22
    gap = 4
    total = len(pills) * pill_w + (len(pills) - 1) * gap
    start_x = rx + (rw - total) / 2
    py = ry + 38
    cells_local = []
    for i, name in enumerate(pills):
        cells_local.append(
            cell(
                f"{prefix}_pill_{i}",
                f'<b style="font-size:10px">{name}</b>',
                start_x + i * (pill_w + gap),
                py,
                pill_w,
                pill_h,
                f"rounded=1;whiteSpace=wrap;html=1;fillColor={VIOLET_F};"
                f"strokeColor={VIOLET_S};fontColor={VIOLET_INK};fontSize=10;"
                f"verticalAlign=middle;arcSize=30;",
            )
        )
    # OutBuf with per-VC subtitle
    ob_w = rw - 24
    cells_local.append(
        cell(
            f"{prefix}_ob",
            '<b style="font-size:10px">OutBuf</b>'
            '<span style="font-size:9px;color:#475569">'
            " · per-VC flit queue</span>",
            rx + 12,
            py + pill_h + 10,
            ob_w,
            22,
            f"rounded=1;whiteSpace=wrap;html=1;fillColor={LIGHT_TINT};"
            f"strokeColor={SLATE_S};fontColor={SLATE_INK};fontSize=10;"
            f"verticalAlign=middle;arcSize=20;",
        )
    )
    return cells_local


cells.extend(pipeline_pills("p3_r0i", r0_x, router_y, router_w))
cells.extend(pipeline_pills("p3_r1i", r1_x, router_y, router_w))

# Inter-router data link (solid) and credit return (dashed) shown
# as a dual edge between routers — kept thick so they read at slide
# rendering size.
cells.append(
    edge(
        "p3_rlink_data",
        "p3_r0",
        "p3_r1",
        f"endArrow=classic;startArrow=none;strokeWidth=2.5;"
        f"strokeColor={VIOLET_S};exitX=1;exitY=0.4;exitDx=0;exitDy=0;"
        f"entryX=0;entryY=0.4;entryDx=0;entryDy=0;",
    )
)
cells.append(
    edge(
        "p3_rlink_cred",
        "p3_r1",
        "p3_r0",
        f"endArrow=classic;startArrow=none;strokeWidth=2;"
        f"strokeColor={GREEN_S};exitX=0;exitY=0.75;exitDx=0;exitDy=0;"
        f"entryX=1;entryY=0.75;entryDx=0;entryDy=0;dashed=1;"
        f"dashPattern=4 3;",
    )
)

# NI ↔ router connections
cells.append(vline("p3_ni0_r0", "p3_ni0", "p3_r0", color=TEAL_S))
cells.append(vline("p3_ni1_r1", "p3_ni1", "p3_r1", color=TEAL_S))
cells.append(vline("p3_l1a_ni0", "p3_l1a", "p3_ni0"))
cells.append(vline("p3_l1b_ni1", "p3_l1b", "p3_ni1"))

# Edge legend below the routers
legend_y = router_y + router_h + 6
cells.append(
    text(
        "p3_legend",
        f'<span style="color:{VIOLET_INK};font-size:10px">'
        f"── flit / data →</span>"
        f"      "
        f'<span style="color:{GREEN_S};font-size:10px;font-style:italic">'
        f"╴╴ credit return ←</span>",
        px + 8,
        legend_y,
        PANEL_W - 16,
        16,
        font_size=10,
        color="#475569",
    )
)

# Home + DRAM
cells.append(
    box(
        "p3_home",
        "HN-F SLICC ctrl · SN-F · DRAM",
        "every link is flitised · cycle-by-cycle",
        px + 8,
        P["home_y"],
        PANEL_W - 16,
        P["home_h"],
        VIOLET_F,
        VIOLET_S,
        VIOLET_INK,
        bold_size=12,
        sub_size=10,
    )
)
cells.append(
    vline(
        "p3_r0_home",
        "p3_r0",
        "p3_home",
        color=VIOLET_S,
        exit_x=0.5,
        entry_x=0.25,
    )
)
cells.append(
    vline(
        "p3_r1_home",
        "p3_r1",
        "p3_home",
        color=VIOLET_S,
        exit_x=0.5,
        entry_x=0.75,
    )
)

cells.append(vline("p3_core0_l1a", "p3_core0", "p3_l1a"))
cells.append(vline("p3_core1_l1b", "p3_core1", "p3_l1b"))


# ---- Emit XML --------------------------------------------------------
HEADER = (
    '<mxfile host="Electron" modified="2026-04-27T00:00:00.000Z" '
    'agent="generator" version="24.0.0">\n'
    '  <diagram name="gem5 modeling levels" id="gem5-modeling-levels">\n'
    f'    <mxGraphModel dx="{PAGE_W}" dy="{PAGE_H}" grid="0" gridSize="10" '
    'guides="1" tooltips="1" connect="1" arrows="1" fold="1" page="1" '
    f'pageScale="1" pageWidth="{PAGE_W}" pageHeight="{PAGE_H}" math="0" '
    'shadow="0" background="#ffffff">\n'
    "      <root>\n"
    '        <mxCell id="0"/>\n'
    '        <mxCell id="1" parent="0"/>\n'
)
FOOTER = (
    "\n      </root>\n" "    </mxGraphModel>\n" "  </diagram>\n" "</mxfile>\n"
)

body = "\n".join(
    "        " + line if line else line
    for cell_xml in cells
    for line in cell_xml.split("\n")
)

OUT.write_text(HEADER + body + FOOTER)
print(f"Wrote {OUT} ({len(cells)} cells)")
