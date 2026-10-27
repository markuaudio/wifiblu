"""Draw SVG charts from a bleaudiocall serial log (no external libraries).

Reads the 5-second audio stats lines
    I (123456) audio: MUSIC 44100 Hz PA=1 in_peak=.. out_peak=.. mic_peak=.. frames=.. limited=.. P3
and the ROM reset banner lines ("rst:0x1 (POWERON_RESET)...") and writes:
    <out>/<name>-peaks.svg     in_peak / out_peak (dBFS) over time, reset markers, DAC profile
    <out>/<name>-limited.svg   frames limited per 5 s window, reset markers
    <out>/<name>-table.csv     the parsed rows

Usage: python tools/plot_serial_log.py docs/test-data/2026-10-27-live-session.log docs/test-data
"""
import math
import re
import sys
from pathlib import Path

STATS = re.compile(r"I \((\d+)\) audio: (MUSIC|CALL) (\d+) Hz PA=(\d) in_peak=(\d+) out_peak=(\d+) mic_peak=(\d+) frames=(\d+) limited=(\d+) P(\d)")
RESET = re.compile(r"rst:0x([0-9a-f]+) \((\w+)\)")


def dbfs(peak):
    return 20 * math.log10(peak / 32767.0) if peak > 0 else -90.0


def parse(path):
    rows, resets, boot = [], [], 0
    t_offset = 0.0  # seconds; each reset restarts the ESP clock, keep a continuous axis
    last_t = 0.0
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = RESET.search(line)
        if m:
            boot += 1
            t_offset = last_t + 2.0
            resets.append((t_offset, m.group(2)))
            continue
        m = STATS.search(line)
        if m:
            t = t_offset + int(m.group(1)) / 1000.0
            last_t = t
            rows.append({
                "t": t, "boot": boot, "path": m.group(2), "rate": int(m.group(3)), "pa": int(m.group(4)),
                "in_peak": int(m.group(5)), "out_peak": int(m.group(6)), "limited": int(m.group(9)),
                "profile": int(m.group(10)),
            })
    return rows, resets


def svg_chart(rows, resets, series, y_label, y_min, y_max, title, path, fmt=lambda v: v):
    w, h, ml, mr, mt, mb = 1100, 420, 70, 20, 40, 50
    if not rows:
        Path(path).write_text("<svg xmlns='http://www.w3.org/2000/svg' width='400' height='60'><text x='10' y='30'>no data</text></svg>")
        return
    t0, t1 = rows[0]["t"], rows[-1]["t"]
    span = max(t1 - t0, 1.0)

    def X(t):
        return ml + (t - t0) / span * (w - ml - mr)

    def Y(v):
        v = max(y_min, min(y_max, v))
        return mt + (y_max - v) / (y_max - y_min) * (h - mt - mb)

    out = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{h}' font-family='sans-serif' font-size='12'>",
           f"<rect width='{w}' height='{h}' fill='white'/>",
           f"<text x='{ml}' y='22' font-size='15' font-weight='bold'>{title}</text>"]
    # grid
    for i in range(6):
        v = y_min + (y_max - y_min) * i / 5
        y = Y(v)
        out.append(f"<line x1='{ml}' y1='{y:.1f}' x2='{w-mr}' y2='{y:.1f}' stroke='#ddd'/>")
        out.append(f"<text x='{ml-8}' y='{y+4:.1f}' text-anchor='end'>{fmt(v)}</text>")
    for i in range(11):
        t = t0 + span * i / 10
        x = X(t)
        out.append(f"<line x1='{x:.1f}' y1='{mt}' x2='{x:.1f}' y2='{h-mb}' stroke='#eee'/>")
        out.append(f"<text x='{x:.1f}' y='{h-mb+16}' text-anchor='middle'>{t:.0f}s</text>")
    out.append(f"<text x='{(w)/2:.0f}' y='{h-8}' text-anchor='middle'>time (s, continuous across resets)</text>")
    out.append(f"<text x='16' y='{h/2:.0f}' transform='rotate(-90 16,{h/2:.0f})' text-anchor='middle'>{y_label}</text>")
    # resets
    for t, name in resets:
        if t0 - 5 <= t <= t1 + 5:
            x = X(t)
            out.append(f"<line x1='{x:.1f}' y1='{mt}' x2='{x:.1f}' y2='{h-mb}' stroke='#d00' stroke-dasharray='4,3'/>")
            out.append(f"<text x='{x+3:.1f}' y='{mt+12}' fill='#d00' font-size='10'>reset</text>")
    # profile band (text at profile changes)
    last_p = None
    for r in rows:
        if r["profile"] != last_p:
            last_p = r["profile"]
            out.append(f"<text x='{X(r['t'])+2:.1f}' y='{h-mb-6}' fill='#555' font-size='10'>P{last_p}</text>")
    # series
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c"]
    for (key, label), col in zip(series, colors):
        pts, segs = [], []
        prev_boot = None
        for r in rows:
            if prev_boot is not None and r["boot"] != prev_boot:
                segs.append(pts); pts = []
            pts.append(f"{X(r['t']):.1f},{Y(r[key]):.1f}")
            prev_boot = r["boot"]
        segs.append(pts)
        for s in segs:
            if len(s) > 1:
                out.append(f"<polyline points='{' '.join(s)}' fill='none' stroke='{col}' stroke-width='1.6'/>")
        out.append(f"<rect x='{w-mr-220+series.index((key,label))*110}' y='28' width='10' height='10' fill='{col}'/>")
        out.append(f"<text x='{w-mr-206+series.index((key,label))*110}' y='37'>{label}</text>")
    out.append("</svg>")
    Path(path).write_text("\n".join(out), encoding="utf-8")


def main():
    src, outdir = Path(sys.argv[1]), Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)
    rows, resets = parse(src)
    name = src.stem
    for r in rows:
        r["in_dbfs"] = dbfs(r["in_peak"]); r["out_dbfs"] = dbfs(r["out_peak"])
    svg_chart(rows, resets, [("in_dbfs", "in_peak"), ("out_dbfs", "out_peak")], "peak (dBFS)", -40, 0,
              f"{name}: music peaks per 5 s window (phone PCM in, speaker PCM out), resets marked", outdir / f"{name}-peaks.svg",
              fmt=lambda v: f"{v:.0f} dB")
    svg_chart(rows, resets, [("limited", "limited frames / 5 s")], "frames", 0, 100000,
              f"{name}: frames held by the -6 dBFS limiter per 5 s window", outdir / f"{name}-limited.svg",
              fmt=lambda v: f"{v/1000:.0f}k")
    with open(outdir / f"{name}-table.csv", "w", encoding="utf-8") as f:
        f.write("t_s,boot,path,rate,pa,in_peak,in_dbfs,out_peak,out_dbfs,limited,profile\n")
        for r in rows:
            f.write(f"{r['t']:.1f},{r['boot']},{r['path']},{r['rate']},{r['pa']},{r['in_peak']},{r['in_dbfs']:.1f},{r['out_peak']},{r['out_dbfs']:.1f},{r['limited']},{r['profile']}\n")
    print(f"rows={len(rows)} resets={len(resets)} -> {outdir}")
    for t, nm in resets:
        print(f"  reset at ~{t:.0f}s ({nm})")


if __name__ == "__main__":
    main()
