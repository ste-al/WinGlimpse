# How Glimpse decides

This is the exact behaviour of the geometry engine and the resolver in `Glimpse.cs`
(`Geometry`, `Resolver`). Every build of Glimpse must make the same decisions; the shared
cases in [`tests/geometry-cases.txt`](../tests/geometry-cases.txt) check this
(`Glimpse.exe /selftest`, `Glimpse.ps1 -SelfTest`). A future native build has to pass the same file.

All coordinates are physical pixels. Rectangles are `L T R B` with right and bottom exclusive.

## Inputs

* **Monitors**: bounds, work area (without the taskbar) and DPI.
* **Windows**: the frame (DWM extended frame bounds, without the invisible resize border) and flags:

| Flag | Meaning |
|---|---|
| Subject | must keep a free glimpse (counted windows that are not maximized and not topmost) |
| Blocker | other glimpses must keep clear of it (every counted window except maximized/full-screen ones and those beyond `MaxWindows`) |
| Movable | the resolver may move or resize it. Not movable: elevated (higher integrity level), stubborn, excluded, hung, topmost |
| Resizable | has a resize border; minimum size and shrinking apply |
| MinW, MinH | minimum size from the first matching title rule, `px × DPI / 96` |

* **Glimpse size** `P = round(GlimpseSize × DPI / 96)`, at least 8, using the DPI of the monitor that
  holds most of the window. `Util.Scale` and `GlimpsePx` use .NET `Math.Round`, which rounds half to even.

## Free glimpse zone (`Geometry.BestZone`)

A zone is a `P × P` square inside the frame that touches one of its edges, lies inside **one**
monitor's work area, and overlaps no blocker. Each edge is checked in the order **Top, Bottom, Left, Right**:

1. The square slides along a strip of depth `P`. Positions are the start coordinate `s`:
   `[F.L, F.R − P]` for top and bottom, `[F.T, F.B − P]` for left and right.
2. Allowed positions are the union, over monitors whose work area contains the strip's cross range,
   of `[max(s0, work.start), min(s1, work.end − P)]`.
3. Each blocker that overlaps the strip's cross range removes `[O.start − P + 1, O.end − 1]`.
4. **Quality**:
   * Top edge: a map of what a click does along the title bar. Without probing it is: icon
     `[L, L + 32·dpi/96)` = 1 (avoid), caption buttons `[R − 138·dpi/96, R)` = 0 (never), the rest = 3.
     With probing, `WM_NCHITTEST` results give HTCAPTION/borders 3, client/other 2, HTSYSMENU 1, and
     buttons 0; `DWMWA_CAPTION_BUTTON_BOUNDS` marks 0. The square's quality is the minimum over the
     range it covers; for q = 3, 2, 1 the first allowed position with at least q is taken. Quality 0
     never counts.
   * Bottom edge: quality 2, the first allowed position.
   * Left and right edges: the band `[T, T + 32·dpi/96)` inherits the lowest top-map quality over the
     strip's x range. If that is 0 (caption buttons), positions overlapping the band are removed. If it
     is 1, positions below the band get quality 2, otherwise the zone has quality 1. Else quality 2,
     first allowed position.
5. The best zone has the highest quality; ties keep the earlier edge. Quality 3 stops the search.

A window "has a glimpse" when `BestZone` is not null.

## Requests

`Request.For(cause, mode, target, placeNewWindows)`:

| Cause | Minimum size | Glimpse fix |
|---|---|---|
| Drop (user drag ends) | the target, if it was resized (the dragged edges) | victims |
| New, Restore | the target | intruder (if PlaceNewWindows) |
| AppMove | | intruder in Strict |
| AppResize, title change | the target | intruder in Strict |
| Display change | all; rescue first | victims |
| Arrange now | all | victims |
| X-ray Shift+click | | intruder (also in Guide) |

Guide mode fixes nothing (except X-ray Shift+click), and neither does a paused Glimpse. Strict allows
shrinking. Windows already moved during the same user action are not moved again.

## Minimum size (`Geometry.GrowToMin`)

For each window in scope that is resizable, movable and a subject:

* Width below `MinW`: if the work area is not wider than `MinW`, the window fills the work area's
  width; else if only the left edge was dragged, `L = R − MinW`; otherwise `R = L + MinW`.
  Height works the same with the top edge.
* Then the window shifts inward only by what growing pushed out of the work area (a window that was
  already partly outside stays as far out as before).

## Rescue (display change)

A movable window without a `P × P` part inside any work area moves to the monitor whose work area is
nearest to its centre, at the same relative position it had in its previous work area
(`rx = (L − old.L) / (old.W − W)`, clamped to 0..1, likewise `ry`). It shrinks to the work area if it is
larger and resizable.

## Victims (drop, display change, arrange)

1. Victims are subjects without a glimpse (for a drop: only those touching the dropped window's old or
   new frame), sorted by area, then input order.
2. For each victim that still has no glimpse: if it is not movable it is reported with its reason;
   otherwise **BestNudge**:
   * `maxNudge = min(min(W, H) / 4, round(240 × DPI / 96))`
   * Offsets from every blocker C that intersects the frame F:
     `dy ∈ {C.T − P − F.T, C.B + P − F.B, C.B − F.T, C.T − F.B}`,
     `dx ∈ {C.L − P − F.L, C.R + P − F.R, C.R − F.L, C.L − F.R}`, plus 0 and the offsets to the work
     area's edges. Both lists keep their first occurrence order.
   * Candidates are all `(dx, dy)` pairs except `(0, 0)`, in the order dx-major, dy-minor. Each is
     clamped into the work area (`ClampInto`) and dropped if it ends up as no move, moves further than
     `maxNudge` (Euclidean), or repeats an earlier candidate. Cost is `|dx| + |dy|`, plus 100000 if
     the window changes monitor.
   * Strict mode, resizable windows: for each blocker C the frame cut at C (right edge to `C.L`, left
     edge to `C.R`, bottom to `C.T`, top to `C.B`), if it keeps at least `max(Min, 2P)` and two thirds
     of each dimension. Cost is `3 × pixels shrunk`.
   * Sort by cost, then generation order. Among the cheapest valid candidates the one whose zone has
     the highest quality wins (first one on ties). **Valid** means the window has a glimpse there, and
     every subject that has a glimpse now and intersects the new frame keeps one.
3. A victim with no valid candidate is reported as `noSpace`.

## Intruder (new, restored, app-moved window, X-ray Shift+click)

1. `keep` = subjects that have a glimpse when the intruder is absent. Nothing happens if the intruder
   has a glimpse and every window in `keep` still has one.
2. Sizes to try: the current size; in Strict, for resizable windows, also 70% and 50% (each at least
   `max(Min, 2P)`).
3. For each size, **NearestSpot** on the work area of the monitor holding the window:
   * x candidates: `F.L`, `work.L`, `work.R − W`, and for every blocker C on that work area
     `C.R`, `C.L − W`, `C.L + P_C`, `C.R − P_C − W`; y likewise with `C.B`, `C.T − H`, `C.T + P_C`,
     `C.B − P_C − H`. Each is clamped into the work area; duplicates are dropped.
   * All (x, y) pairs sorted by squared distance from `(F.L, F.T)`, then `|dy|`, `|dx|`, `y`, `x`;
     the first valid one wins (at most 8000 are tried).
4. Last resort: a cascade offset of `(P, P)` from the subject it covers most, clamped into the work
   area, taken only if it leaves fewer windows without a glimpse.

## Snap guides

While dragging, for every neighbour's glimpse zone Z (computed without the dragged window) the
dragged frame is aligned so its bottom is at `Z.T`, its top at `Z.B`, its right at `Z.L` or its left
at `Z.R`. A guide is offered when that is within 60 px (`|dx| + |dy|`), does not cut through Z, lies in
the work area, and is valid (the dragged window has a glimpse and every neighbour keeps one).
Guides are sorted by distance; equal distances keep the order they were found in (neighbours in
input order, then bottom, top, right, left). A drop within 12 px of a shown guide snaps to it.

## Never fighting (runtime, not in the pure engine)

* Location events within 500 ms after Glimpse moved a window are ignored.
* A window that returns to where it was before a fix, or refuses a fix, twice within 10 seconds is
  stubborn for the session.
* More than 10 moves within 10 seconds pause automatic fixes for 5 minutes (tray warning).
* Only the `MaxWindows` most recently used windows (by foreground activation, then z-order) count.
