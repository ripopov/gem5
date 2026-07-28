# JitCPU — 7-slide overview deck, plus backup slides

An introduction to `RiscvJitCPU` for experienced gem5 and full-system simulator
developers who do not know QEMU: where the model sits in the CPU hierarchy, what
TCG contributes, how an instruction batch is bounded by the next scheduled event,
what a takeover by `RiscvO3CPU` transfers, and what it measured against
`AtomicSimpleCPU`. The last slide replays the whole execution model as one
looping animated diagram. Scope is the first commit only ("cpu: Add QEMU-backed RISC-V
JitCPU with one-way switching to O3"); the Ruby/CHI round-trip work is deliberately
out of scope.

It assumes no familiarity with this source or with QEMU, but does assume the usual
simulator vocabulary: architectural state, atomic and timing accesses, event queues,
drain/takeover, backing stores, MMIO and coherence state. Function and parameter
names appear only as dim trailing pointers for whoever wants to read the code
afterwards.

Behind the six is a **backup section** — extra slides in a deliberately different
visual style, for questions the main deck raises but does not have room to answer.
They are not part of the talk and are numbered separately, so the deck still says
"7" no matter how many accumulate. So far: why an instruction batch runs on a thread
that is not gem5's; the TCG translation-block lifecycle; physical-access routing and early
termination; which uncore behavior is bypassed while the platform runs in
`atomic_noncaching` mode; why the CLINT's periodic timer event, not `batch_size`, set the
batch length until `Clint.rtc_period` made `mtime` lazy; and a terminology card expanding
every acronym the deck uses.

Content is drawn from [`JITCPU_TUTORIAL.md`](../../JITCPU_TUTORIAL.md) — sections 1,
3, 4.1–4.2 and 8.

## Viewing

Both language versions are standalone:

- `index.html` — English;
- `index.ru.html` — professional Russian localization.

Open either file in any browser. No server, build step or network access is needed:

```sh
xdg-open docs/jitcpu-deck/index.html
xdg-open docs/jitcpu-deck/index.ru.html
```

Each deck is one self-contained file with no external references, so copying or
emailing the selected HTML file on its own is enough. This README is not needed to
view it.

## Controls

| Key | Action |
| --- | --- |
| <kbd>→</kbd> <kbd>space</kbd> <kbd>↓</kbd> <kbd>PgDn</kbd> | next step / slide |
| <kbd>←</kbd> <kbd>↑</kbd> <kbd>PgUp</kbd> | back |
| <kbd>1</kbd>…<kbd>7</kbd> | jump to slide (<kbd>8</kbd>+ reach the backup slides) |
| <kbd>Home</kbd> / <kbd>End</kbd> | first / last slide |
| <kbd>F</kbd> | fullscreen |
| <kbd>P</kbd> | print → save as PDF |

Clicking also advances (left edge goes back). `index.html#4` or `index.ru.html#4`
opens slide 4 with all its steps revealed, which is handy for linking to a single
slide; `#b1` does the same for the first backup slide.

The backup slides sit after slide 7, so <kbd>→</kbd> from the last slide walks into
them and <kbd>End</kbd> lands on the last one. They are also included in the PDF.

## Exporting a PDF

<kbd>P</kbd>, then "Save as PDF" — pages come out 16:9 with every step revealed.
Headless equivalent:

```sh
google-chrome --headless=new --no-pdf-header-footer --virtual-time-budget=4000 \
  --print-to-pdf=jitcpu-deck.pdf file://$PWD/docs/jitcpu-deck/index.html
```

Use `index.ru.html` and a different output name to export the Russian deck.

## Editing

Each language version is self-contained. Everything lives in its HTML file: CSS in
the `<style>` block, one `<section class="slide">` per slide, and a small navigation
script at the bottom. Elements carrying `class="frag"` appear one step at a time, in
document order.

Slides are laid out on a fixed 1280×720 stage that is scaled to fit the window, so
what you see while editing is what every viewer sees at any window size. Nothing
clips if a slide overflows — the extra content simply falls off the bottom edge — so
after editing, check that each slide's content still ends above its `.foot`. Measure
with `offsetTop`/`offsetHeight`; `getBoundingClientRect()` returns stage coordinates
multiplied by the fit scale and will quietly tell you a slide fits when it does not.

An identifier should never be the subject or verb of a sentence: say what the thing
does in plain words, then hang the name off the end in a `<span class="ptr">`. That
keeps every slide readable by someone who has not opened the source.

The three headline figures on slide 6 have their final values in the markup and
`data-count` only drives the count-up animation — edit the text, not just the
attribute.

## The animated slide

Slide 7 is one looping diagram rather than a page of prose, so it does not follow
the rules above. Every box sits at an absolute stage coordinate and a single
`render(t)` at the foot of the file derives the whole frame from one normalised
clock — classes, widths, transforms and text, never geometry. Keeping `render` a
pure function of `t` is what makes the loop reproducible: `window.jitDemoFrame(t)`
freezes any instant, which is how the printed still is produced and how the layout
is checked frame by frame. The animation runs only while the slide is active, the
markup holds the still it prints, and the loop dips out at `RESET`, snaps back to
the opening state underneath and returns already at `t = 0`, so the wrap is
seamless and a stalled clock leaves the opening frame on screen rather than a
blank slide.

The loop ends on a deliberate beat. A third batch — the one carrying the
interrupt in — keeps retiring toward the UART deadline for four seconds at batch
one's pace rather than batch two's, so that closing crawl reads as ordinary
execution; then at `HOLD` the picture freezes for two full seconds before the
reset dip. The freeze is literal: past `HOLD` every frame renders the `HOLD`
frame, down to the wire dots and the flickering low bits of the PC, so nothing
twitches while the eye catches up.

Anything that moves travels along one of the five wires in the `#dwire` overlay,
each routed through a gutter that no box occupies — which is the only reason the
motion can never cover up something being read. Moving a panel means re-routing
its wire.

## Backup slides

Add `class="slide bk"` and the section styling follows: a left stripe in the section
colour, a "backup" tag, and a matching eyebrow and progress bar. Semantic colours are
untouched, so the deck's primary accent still means gem5 and its secondary still means
QEMU — only the chrome says "you have left the talk".

Backup slides skip the step reveals: each one arrives whole and a single press moves to
the next, because it answers one question rather than building an argument. Marking
content `frag` there is harmless — CSS reveals it immediately and the script does not
count it as a step.

The script counts the non-`bk` slides to decide what the main deck's length is, which
is what keeps the counter reading `n / 6` and the progress bar full across the whole
backup section. Nothing needs updating when a backup slide is added; just append
another `<section class="slide bk">` at the end of the stage.

A backup slide earns its place by answering a question the main deck provokes, at the
level the question is actually asked — "why is there a second thread at all", not "here
is the calling sequence". Mechanism is worth a slide only once the reason for it is
already on one.

The terminology card is the one exception to that rule and to the deck's type sizes: it
is looked things up in rather than read out, so it runs five columns of 12.5 px in a
`.gloss` grid. Every abbreviation used anywhere in the deck belongs on it — adding one
elsewhere means adding a line there.
