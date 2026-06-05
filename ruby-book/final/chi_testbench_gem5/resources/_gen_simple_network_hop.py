#!/usr/bin/env python3
"""Generate simple_network_hop.drawio.

One detailed hop of gem5's Ruby SimpleNetwork.  Focus is on Router A
(a SimpleNetwork ``Switch``): its input queues, the PerfectSwitch
routing logic, the per-output-port FIFO buffers, and the per-output
Throttle gates.  Router B shows only the single A->B input queue.

Teaching goal: make visible *where storage exists* (MessageBuffers,
drawn as rectangles with slots, some filled) and *where latency is
applied* (PerfectSwitch routing latency, Throttle link latency), while
keeping routing-algorithm detail abstract (drawn as clouds).

Model references (src/mem/ruby/network/simple/):
  Switch.cc           addOutPort(): one Throttle + one intermediate
                      (port) buffer per vnet; int/ext routing latency.
  PerfectSwitch.cc    operateMessageBuffer(): route, check
                      areNSlotsAvailable(1) on output buffer, enqueue
                      with delay = out_port.latency (routing latency).
  Throttle.cc         operateVnet(): check areNSlotsAvailable(1) on
                      downstream link buffer, enqueue with delay =
                      m_link_latency; bandwidth = endpoint_bw * mult.
  SimpleLink.py       int-link buffer depth = link_latency + 1.
"""

from html import escape as h
from pathlib import Path

OUT = Path(__file__).resolve().parent / "simple_network_hop.drawio"

# HTML entities used in the value strings -> Unicode, so that the final
# html.escape() turns the embedded HTML markup into the &lt;b&gt; form
# drawio expects inside an XML attribute value.
_ENT = {
    "&#8594;": "→",
    "&#8592;": "←",
    "&#215;": "×",
    "&#183;": "·",
    "&#8734;": "∞",
    "&#8226;": "•",
    "&#8230;": "…",
    "&nbsp;": " ",
    "&amp;": "&",
}


def esc(s):
    for k, v in _ENT.items():
        s = s.replace(k, v)
    return h(s, quote=True)


PAGE_W, PAGE_H = 1560, 890

# ---- color tokens --------------------------------------------------------
# MessageBuffer storage (blue)
BUF_F, BUF_S = "#dae8fc", "#6c8ebf"
INF_F = "#e3f2fb"  # unbounded (controller endpoint) container tint
SLOT_FILL = "#7ea6e0"  # occupied slot
SLOT_EMPTY = "#ffffff"  # free slot
# PerfectSwitch routing logic (orange cloud)
PS_F, PS_S = "#ffe6cc", "#d79b00"
# Throttle bandwidth/latency gate (red cloud)
TH_F, TH_S = "#f8cecc", "#b85450"
# Router frames (purple)
RA_F, RA_S = "#f6f0fb", "#9673a6"
RB_F, RB_S = "#faf6fd", "#b39ddb"
# Controller (green)
CT_F, CT_S = "#d5e8d4", "#82b366"
# latency / annotation ink
LAT_INK = "#b85450"
ROUTE_INK = "#9673a6"
GREEN_INK = "#2a7f3f"
GREY = "#9b9b9b"

cells = []


def cell(s):
    cells.append(s)


def box(
    cid,
    x,
    y,
    w,
    hh,
    value,
    fill,
    stroke,
    *,
    rounded=1,
    dashed=0,
    fontsize=11,
    valign="middle",
    sw=1.6,
    fontcolor="#000000",
):
    style = (
        f"rounded={rounded};whiteSpace=wrap;html=1;fillColor={fill};"
        f"strokeColor={stroke};strokeWidth={sw};fontSize={fontsize};"
        f"align=center;verticalAlign={valign};fontColor={fontcolor};"
        f"arcSize=6;"
    )
    if dashed:
        style += "dashed=1;dashPattern=6 4;"
    cell(
        f'<mxCell id="{cid}" value="{esc(value)}" style="{style}" vertex="1" '
        f'parent="1"><mxGeometry x="{x}" y="{y}" width="{w}" '
        f'height="{hh}" as="geometry"/></mxCell>'
    )


def cloud(cid, x, y, w, hh, value, fill, stroke, *, fontsize=11, dashed=0):
    style = (
        f"shape=cloud;whiteSpace=wrap;html=1;fillColor={fill};"
        f"strokeColor={stroke};strokeWidth=1.6;fontSize={fontsize};"
        f"align=center;verticalAlign=middle;"
    )
    if dashed:
        style += "dashed=1;dashPattern=6 4;"
    cell(
        f'<mxCell id="{cid}" value="{esc(value)}" style="{style}" vertex="1" '
        f'parent="1"><mxGeometry x="{x}" y="{y}" width="{w}" '
        f'height="{hh}" as="geometry"/></mxCell>'
    )


def text(
    cid,
    x,
    y,
    w,
    hh,
    value,
    *,
    fontsize=10,
    color="#000000",
    align="center",
    bold=0,
    valign="middle",
):
    v = f"<b>{value}</b>" if bold else value
    style = (
        f"text;html=1;align={align};verticalAlign={valign};"
        f"resizable=0;points=[];strokeColor=none;fillColor=none;"
        f"fontSize={fontsize};fontColor={color};"
    )
    cell(
        f'<mxCell id="{cid}" value="{esc(v)}" style="{style}" vertex="1" '
        f'parent="1"><mxGeometry x="{x}" y="{y}" width="{w}" '
        f'height="{hh}" as="geometry"/></mxCell>'
    )


def _slot(cid, cx, sy, slot, fill, *, dash=0):
    st = (
        f"rounded=0;whiteSpace=wrap;html=1;fillColor={fill};"
        f"strokeColor={BUF_S};strokeWidth=1;"
    )
    if dash:
        st += "dashed=1;dashPattern=4 3;"
    cell(
        f'<mxCell id="{cid}" value="" style="{st}" vertex="1" parent="1">'
        f'<mxGeometry x="{cx:.1f}" y="{sy:.1f}" width="{slot}" '
        f'height="{slot}" as="geometry"/></mxCell>'
    )


def buffer(
    cid,
    x,
    y,
    w,
    hh,
    title,
    ncap,
    nfilled,
    *,
    dashed=0,
    cap_note=None,
    infinite=False,
):
    """A MessageBuffer: titled container + a row of slot cells.

    infinite=True renders an *unbounded* endpoint queue (controller
    MessageBuffer): filled/empty slots that trail off through a dashed
    open slot into an infinity glyph -> no fixed capacity, no
    backpressure, distinct from the bounded fixed-slot router buffers.
    """
    fill_c = INF_F if infinite else BUF_F
    box(
        cid,
        x,
        y,
        w,
        hh,
        title,
        fill_c,
        BUF_S,
        rounded=1,
        dashed=dashed,
        fontsize=10,
        valign="top",
    )
    slot = 22
    gap = 5
    sy = y + hh - slot - 12
    if infinite:
        nsolid = nfilled + 2
        inf_w = 26
        total = nsolid * (slot + gap) + slot + inf_w
        sx = x + (w - total) / 2
        for i in range(nsolid):
            fill = SLOT_FILL if i < nfilled else SLOT_EMPTY
            _slot(f"{cid}_s{i}", sx + i * (slot + gap), sy, slot, fill)
        cxd = sx + nsolid * (slot + gap)
        _slot(f"{cid}_sd", cxd, sy, slot, SLOT_EMPTY, dash=1)
        text(
            cid + "_inf",
            cxd + slot - 2,
            sy - 6,
            inf_w + 8,
            slot + 12,
            "&#8734;",
            fontsize=22,
            bold=1,
            color=BUF_S,
        )
        if cap_note is None:
            cap_note = "depth: unbounded (&#8734;) &#183; no backpressure"
    else:
        total = ncap * slot + (ncap - 1) * gap
        sx = x + (w - total) / 2
        for i in range(ncap):
            fill = SLOT_FILL if i < nfilled else SLOT_EMPTY
            _slot(f"{cid}_s{i}", sx + i * (slot + gap), sy, slot, fill)
    if cap_note:
        text(
            cid + "_cap",
            x,
            y + hh + 1,
            w,
            16,
            cap_note,
            fontsize=9,
            color="#555555",
        )


def edge(
    cid,
    src,
    tgt,
    label="",
    *,
    color="#333333",
    sw=1.8,
    dashed=0,
    sx=None,
    sy=None,
    tx=None,
    ty=None,
    waypoints=None,
    labelcolor=None,
    fontsize=9,
    rounded=1,
    labelbg=1,
    ortho=0,
):
    style = (
        f"endArrow=classic;html=1;strokeWidth={sw};strokeColor={color};"
        f"rounded={rounded};"
    )
    if ortho:
        style += "edgeStyle=orthogonalEdgeStyle;jettySize=auto;"
    if dashed:
        style += "dashed=1;dashPattern=6 4;"
    if sx is not None:
        style += f"exitX={sx};exitY={sy};exitDx=0;exitDy=0;"
    if tx is not None:
        style += f"entryX={tx};entryY={ty};entryDx=0;entryDy=0;"
    geo = ""
    if waypoints:
        pts = "".join(
            f'<mxPoint x="{px}" y="{py}" as="point"/>' for px, py in waypoints
        )
        geo = f'<Array as="points">{pts}</Array>'
    cell(
        f'<mxCell id="{cid}" style="{style}" edge="1" parent="1" '
        f'source="{src}" target="{tgt}"><mxGeometry relative="1" '
        f'as="geometry">{geo}</mxGeometry></mxCell>'
    )
    if label:
        lc = labelcolor or color
        bg = "labelBackgroundColor=#ffffff;" if labelbg else ""
        cell(
            f'<mxCell id="{cid}_l" value="{esc(label)}" '
            f'style="edgeLabel;html=1;align=center;verticalAlign=middle;'
            f"resizable=0;points=[];fontSize={fontsize};fontColor={lc};"
            f'{bg}" vertex="1" connectable="0" parent="{cid}">'
            f'<mxGeometry x="0" relative="1" as="geometry">'
            f'<mxPoint as="offset"/></mxGeometry></mxCell>'
        )


# ==========================================================================
# Title
# ==========================================================================
text(
    "title",
    20,
    10,
    PAGE_W - 40,
    30,
    "SimpleNetwork — one detailed hop (Router A &#8594; Router B)",
    fontsize=18,
    bold=1,
)
text(
    "subtitle",
    20,
    40,
    PAGE_W - 40,
    20,
    "Where storage lives (MessageBuffers = FIFO slots) and where latency "
    "is applied (PerfectSwitch routing latency, Throttle link latency). "
    "Routing/bandwidth logic shown as clouds.",
    fontsize=11,
    color="#555555",
)

# ==========================================================================
# RN-F controller (left)  --  both ExtLink directions attach on this side
# ==========================================================================
box(
    "ctrl",
    30,
    250,
    205,
    300,
    "<b>Local RN-F tile</b><br>controller",
    CT_F,
    CT_S,
    fontsize=11,
    valign="top",
)
# Controller output buffer (shared with the switch input) on top; controller
# input buffer (ExtLink-out destination) below. Both face Router A (right).
buffer(
    "ctrl_out",
    46,
    300,
    173,
    92,
    "<b>output MessageBuffer</b><br><font style='font-size:8px'>"
    "m_toNetQueues (reqOut/datOut&#8230;)<br>"
    "<b>= Switch&nbsp;A input queue (shared)</b></font>",
    0,
    2,
    infinite=True,
    cap_note="unbounded (&#8734;)",
)
buffer(
    "ctrl_in",
    46,
    420,
    173,
    92,
    "<b>input MessageBuffer</b><br><font style='font-size:8px'>"
    "m_fromNetQueues (reqIn/datIn&#8230;)<br>"
    "&#8592; ExtLink-out destination</font>",
    0,
    1,
    infinite=True,
    cap_note="unbounded (&#8734;)",
)

# ==========================================================================
# Router A frame
# ==========================================================================
RA_X, RA_Y, RA_W, RA_H = 285, 90, 905, 625
box("routerA", RA_X, RA_Y, RA_W, RA_H, "", RA_F, RA_S, sw=2.2, valign="top")
text(
    "routerA_lbl",
    RA_X + 12,
    RA_Y + 8,
    RA_W - 24,
    22,
    "Router A  &#8212;  SimpleNetwork <b>Switch</b> (BasicRouter)",
    fontsize=13,
    align="left",
)
text(
    "routerA_sub",
    RA_X + 12,
    RA_Y + 30,
    RA_W - 24,
    18,
    "one PerfectSwitch + one Throttle &amp; one port buffer per "
    "(output port &#215; vnet) &#183; diagram shows a single vnet",
    fontsize=10,
    align="left",
    color="#6a4f86",
)
text(
    "port_in_hdr",
    RA_X + 18,
    150,
    230,
    16,
    "&#9660; INPUT side (into PerfectSwitch)",
    fontsize=10,
    bold=1,
    align="left",
    color="#6a4f86",
)

# ---- input queues --------------------------------------------------------
IN_X, IN_W = 300, 215
# NOTE: controller->switch (ExtLink in) has NO router-side buffer. The
# switch input port *is* the controller output buffer (m_toNetQueues).
box(
    "note_extin",
    IN_X,
    172,
    IN_W,
    70,
    "<b>No router-side RN-F input buffer</b><br>"
    "<font style='font-size:8px'>PerfectSwitch reads the controller's output "
    "buffer<br>directly &#8212; m_toNetQueues <i>is</i> the switch input "
    "queue.<br>One shared object, no copy and no link latency.</font>",
    "#fff8e1",
    "#d6b656",
    fontsize=9,
    sw=1.2,
)
buffer(
    "inA_neigh",
    IN_X,
    350,
    IN_W,
    115,
    "<b>A input &#8592; neighbor router</b><br>"
    "<font style='font-size:9px'>= SimpleIntLink.m_buffers<br>"
    "(also the delayed link buffer)</font>",
    3,
    3,
    cap_note="depth = L + 1",
)

# ---- PerfectSwitch cloud -------------------------------------------------
cloud(
    "ps",
    575,
    240,
    205,
    360,
    "<b>PerfectSwitch</b><br><font style='font-size:9px'>"
    "routing logic &#183; <b>no storage</b><br><br>"
    "for each ready input msg:<br>"
    "&#8226; route() &#8594; output port(s)<br>"
    "&#8226; check areNSlotsAvailable(1)<br>&nbsp;&nbsp;on output buffer "
    "(backpressure)<br>"
    "&#8226; enqueue into output buffer<br><br>"
    "<b>no crossbar bw limit:</b><br>many msgs/cycle into one<br>"
    "output buffer (bounded only<br>by its 8 free slots)</font>",
    PS_F,
    PS_S,
    fontsize=11,
)

# ---- output port: A -> B (the detailed remote hop, on the right) ---------
buffer(
    "outA_B",
    825,
    215,
    215,
    120,
    "<b>A output port &#8594; B</b><br>"
    "<font style='font-size:9px'>Switch.port_buffers[vnet]</font>",
    8,
    5,
    cap_note="depth = router_buffer_size = 8",
)
cloud(
    "thr_B",
    1080,
    235,
    105,
    95,
    "<b>Throttle</b><br>A&#8594;B<br><font style='font-size:8px'>"
    "bw 40 B/cy &#183; 1 msg/cy/vnet<br>needs free B-buf slot</font>",
    TH_F,
    TH_S,
    fontsize=10,
)

# ---- output port: A -> RN-F (LOCAL, on the controller side) --------------
# Same Switch output port + Throttle structure as A->B, but the destination
# is the local controller input buffer, so it lives on the left.
text(
    "port_out_hdr",
    RA_X + 18,
    495,
    250,
    16,
    "&#9650; OUTPUT side &#8594; RN-F (local port)",
    fontsize=10,
    bold=1,
    align="left",
    color="#6a4f86",
)
buffer(
    "outA_rnf",
    372,
    530,
    165,
    92,
    "<b>A output port &#8594; RN-F</b><br>"
    "<font style='font-size:8px'>Switch.port_buffers[vnet]</font>",
    8,
    1,
    cap_note="depth = 8",
)
cloud(
    "thr_rnf",
    292,
    535,
    78,
    82,
    "<b>Throttle</b><br>A&#8594;RN-F",
    TH_F,
    TH_S,
    fontsize=9,
)

# ==========================================================================
# Router B frame (only the A->B input is shown)
# ==========================================================================
RB_X, RB_Y, RB_W, RB_H = 1275, 175, 260, 300
box("routerB", RB_X, RB_Y, RB_W, RB_H, "", RB_F, RB_S, sw=2.2, valign="top")
text(
    "routerB_lbl",
    RB_X + 12,
    RB_Y + 8,
    RB_W - 24,
    20,
    "Router B",
    fontsize=13,
    align="left",
)
text(
    "routerB_sub",
    RB_X + 12,
    RB_Y + 28,
    RB_W - 24,
    30,
    "only the single<br>A&#8594;B input shown",
    fontsize=9,
    align="left",
    color="#7a6a9a",
)
buffer(
    "inB_AB",
    RB_X + 25,
    240,
    210,
    120,
    "<b>B input &#8592; A</b><br><font style='font-size:9px'>"
    "= SimpleIntLink.m_buffers<br>(delayed A&#8594;B link)</font>",
    3,
    2,
    cap_note="depth = L + 1",
)
cloud(
    "psB",
    RB_X + 50,
    400,
    160,
    60,
    "<b>PerfectSwitch B</b><br><font style='font-size:8px'>"
    "consumes ready msg</font>",
    PS_F,
    PS_S,
    fontsize=10,
)

# ==========================================================================
# Edges
# ==========================================================================
# --- INPUT side ---
# controller output buffer (shared) is read directly by the PerfectSwitch
edge(
    "e_extin",
    "ctrl_out",
    "ps",
    "Switch&nbsp;A dequeues this buffer directly<br>"
    "(isReady) &#183; no ExtLink-in buffer or latency",
    color=GREEN_INK,
    sx=1,
    sy=0.4,
    tx=0,
    ty=0.16,
    labelcolor=GREEN_INK,
    fontsize=9,
)
edge(
    "e_in_neigh",
    "inA_neigh",
    "ps",
    "isReady()",
    color="#333333",
    sx=1,
    sy=0.5,
    tx=0,
    ty=0.62,
    labelcolor="#444444",
)
# neighbor-router source stub into inA_neigh (stands in for the other
# mesh-router internal links that are not drawn)
cell(
    '<mxCell id="stub_in" style="endArrow=classic;html=1;strokeWidth=1.8;'
    f'strokeColor={GREY};dashed=1;dashPattern=6 4;rounded=1;" edge="1" '
    'parent="1" target="inA_neigh"><mxGeometry relative="1" '
    'as="geometry"><mxPoint x="248" y="300" as="sourcePoint"/>'
    '<mxPoint x="300" y="398" as="targetPoint"/>'
    '<Array as="points"><mxPoint x="262" y="300"/>'
    '<mxPoint x="262" y="398"/></Array></mxGeometry></mxCell>'
)
text(
    "stub_in_lbl",
    228,
    262,
    96,
    30,
    "from a neighbor /<br>other mesh router",
    fontsize=8,
    color=GREY,
)

# --- OUTPUT side: A -> B (remote, right) ---
edge(
    "e_ob_B",
    "ps",
    "outA_B",
    "ready after routing latency<br>int_routing_latency = 4 cy",
    color=ROUTE_INK,
    sx=1,
    sy=0.22,
    tx=0,
    ty=0.5,
    labelcolor=ROUTE_INK,
    fontsize=9,
)
edge(
    "e_tb_B",
    "outA_B",
    "thr_B",
    "",
    color="#333333",
    sx=1,
    sy=0.5,
    tx=0,
    ty=0.5,
)
edge(
    "e_link",
    "thr_B",
    "inB_AB",
    "ready after link latency L<br>(router_link 2 / cross 7 cy)",
    color=LAT_INK,
    sw=2.2,
    sx=1,
    sy=0.5,
    tx=0,
    ty=0.5,
    labelcolor=LAT_INK,
    fontsize=10,
)
edge("e_bps", "inB_AB", "psB", "", color="#333333", sx=0.5, sy=1, tx=0.5, ty=0)
# "other routers" output stub from the PerfectSwitch
cell(
    '<mxCell id="stub_out_other" style="endArrow=classic;html=1;'
    f"strokeWidth=1.8;strokeColor={GREY};dashed=1;dashPattern=6 4;"
    'rounded=1;exitX=1;exitY=0.78;exitDx=0;exitDy=0;" edge="1" '
    'parent="1" source="ps"><mxGeometry relative="1" as="geometry">'
    '<mxPoint x="1080" y="470" as="targetPoint"/>'
    '<Array as="points"><mxPoint x="900" y="470"/></Array>'
    "</mxGeometry></mxCell>"
)
text(
    "stub_out_other_lbl",
    905,
    474,
    180,
    16,
    "to other mesh routers (not shown)",
    fontsize=8,
    color=GREY,
    align="left",
)

# --- OUTPUT side: A -> RN-F (local, left) ---
edge(
    "e_ob_rnf",
    "ps",
    "outA_rnf",
    "ext_routing_latency = 6 cy",
    color=ROUTE_INK,
    sx=0,
    sy=0.78,
    tx=0.6,
    ty=0,
    labelcolor=ROUTE_INK,
    fontsize=9,
)
edge(
    "e_tb_rnf",
    "outA_rnf",
    "thr_rnf",
    "",
    color="#333333",
    sx=0,
    sy=0.5,
    tx=1,
    ty=0.5,
)
edge(
    "e_thr_ctrlin",
    "thr_rnf",
    "ctrl_in",
    "ExtLink out<br>node_link_latency = 1 cy",
    color=GREEN_INK,
    sx=0,
    sy=0.5,
    tx=1,
    ty=0.5,
    labelcolor=GREEN_INK,
    fontsize=9,
)


# ==========================================================================
# Legend
# ==========================================================================
def legend_item(cid, x, y, kind, label):
    if kind == "buf":
        cell(
            f'<mxCell id="{cid}_k" value="" style="rounded=1;html=1;'
            f'fillColor={BUF_F};strokeColor={BUF_S};strokeWidth=1.4;" '
            f'vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" '
            f'width="34" height="22" as="geometry"/></mxCell>'
        )
        cell(
            f'<mxCell id="{cid}_s0" value="" style="rounded=0;html=1;'
            f'fillColor={SLOT_FILL};strokeColor={BUF_S};strokeWidth=0.8;" '
            f'vertex="1" parent="1"><mxGeometry x="{x+4}" y="{y+6}" '
            f'width="9" height="10" as="geometry"/></mxCell>'
        )
        cell(
            f'<mxCell id="{cid}_s1" value="" style="rounded=0;html=1;'
            f'fillColor={SLOT_EMPTY};strokeColor={BUF_S};strokeWidth=0.8;" '
            f'vertex="1" parent="1"><mxGeometry x="{x+15}" y="{y+6}" '
            f'width="9" height="10" as="geometry"/></mxCell>'
        )
    elif kind == "inf":
        cell(
            f'<mxCell id="{cid}_k" value="" style="rounded=1;html=1;'
            f'fillColor={INF_F};strokeColor={BUF_S};strokeWidth=1.4;" '
            f'vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" '
            f'width="34" height="22" as="geometry"/></mxCell>'
        )
        cell(
            f'<mxCell id="{cid}_s0" value="" style="rounded=0;html=1;'
            f'fillColor={SLOT_FILL};strokeColor={BUF_S};strokeWidth=0.8;" '
            f'vertex="1" parent="1"><mxGeometry x="{x+4}" y="{y+6}" '
            f'width="9" height="10" as="geometry"/></mxCell>'
        )
        cell(
            f'<mxCell id="{cid}_sd" value="" style="rounded=0;html=1;'
            f"fillColor={SLOT_EMPTY};strokeColor={BUF_S};strokeWidth=0.8;"
            f'dashed=1;dashPattern=3 2;" vertex="1" parent="1">'
            f'<mxGeometry x="{x+15}" y="{y+6}" width="9" height="10" '
            f'as="geometry"/></mxCell>'
        )
        text(
            cid + "_inf",
            x + 24,
            y - 6,
            16,
            34,
            "&#8734;",
            fontsize=15,
            bold=1,
            color=BUF_S,
        )
    elif kind == "ps":
        cloud(cid + "_k", x, y - 4, 38, 30, "", PS_F, PS_S)
    elif kind == "th":
        cloud(cid + "_k", x, y - 4, 38, 30, "", TH_F, TH_S)
    text(cid + "_t", x + 46, y - 6, 560, 34, label, fontsize=10, align="left")


LEGY = 745
box(
    "legend",
    285,
    LEGY - 12,
    905,
    127,
    "",
    "#fbfbfb",
    "#cccccc",
    sw=1.2,
    valign="top",
)
text(
    "legend_t",
    295,
    LEGY - 8,
    880,
    16,
    "Legend",
    fontsize=10,
    bold=1,
    align="left",
    color="#555555",
)
legend_item(
    "lg1",
    300,
    LEGY + 16,
    "buf",
    "<b>Bounded MessageBuffer</b> (router) &#8212; fixed N slots; "
    "enforces backpressure via areNSlotsAvailable()",
)
legend_item(
    "lg2",
    300,
    LEGY + 43,
    "inf",
    "<b>Unbounded MessageBuffer</b> (controller endpoint) &#8212; "
    "slots trail off to &#8734;; no backpressure",
)
legend_item(
    "lg3",
    300,
    LEGY + 70,
    "ps",
    "<b>PerfectSwitch</b> &#8212; routing logic, no storage; "
    "applies routing latency on enqueue to the output buffer",
)
legend_item(
    "lg4",
    300,
    LEGY + 97,
    "th",
    "<b>Throttle</b> &#8212; per-output bandwidth + link-latency "
    "gate; applies link latency L on enqueue to the downstream "
    "buffer",
)

# ==========================================================================
# Emit file
# ==========================================================================
body = "\n        ".join(cells)
xml = f"""<mxfile host="Electron" modified="2026-06-05T00:00:00.000Z" agent="draw.io" version="29.7.9">
  <diagram name="SimpleNetwork Hop" id="simple-network-hop">
    <mxGraphModel dx="1500" dy="1000" grid="0" gridSize="10" guides="1" tooltips="1" connect="1" arrows="1" fold="1" page="1" pageScale="1" pageWidth="{PAGE_W}" pageHeight="{PAGE_H}" math="0" shadow="0" background="#ffffff">
      <root>
        <mxCell id="0"/>
        <mxCell id="1" parent="0"/>
        {body}
      </root>
    </mxGraphModel>
  </diagram>
</mxfile>
"""
OUT.write_text(xml)
print(f"wrote {OUT}")
