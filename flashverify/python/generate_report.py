#!/usr/bin/env python3
"""
FlashVerify PDF Report Generator
Usage: python generate_report.py --input session.json --output report.pdf
"""

import argparse
import json
import os
import sys
from datetime import datetime

try:
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
    from reportlab.lib.units import cm
    from reportlab.lib import colors
    from reportlab.platypus import (
        SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle,
        HRFlowable, KeepTogether
    )
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
except ImportError:
    print("ERROR: reportlab not installed. Run: pip install reportlab", file=sys.stderr)
    sys.exit(1)


# ── Thai font registration ────────────────────────────────────────────────────

FONT_NAME = "NotoSans"
FONT_BOLD = "NotoSans-Bold"

def _register_fonts():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        # bundled in python/ dir (highest priority)
        os.path.join(script_dir, "NotoSansThai-Regular.ttf"),
        os.path.join(script_dir, "NotoSans-Regular.ttf"),
        # system paths (Linux)
        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
        # system paths (Windows via MSYS2 / WSL)
        r"C:\Windows\Fonts\tahoma.ttf",
        r"C:\Windows\Fonts\arial.ttf",
    ]
    candidates_bold = [
        os.path.join(script_dir, "NotoSansThai-Bold.ttf"),
        os.path.join(script_dir, "NotoSans-Bold.ttf"),
        "/usr/share/fonts/truetype/noto/NotoSansThai-Bold.ttf",
        r"C:\Windows\Fonts\tahomabd.ttf",
        r"C:\Windows\Fonts\arialbd.ttf",
    ]

    regular_path = next((p for p in candidates if os.path.exists(p)), None)
    bold_path    = next((p for p in candidates_bold if os.path.exists(p)), None)

    if regular_path:
        pdfmetrics.registerFont(TTFont(FONT_NAME, regular_path))
        if bold_path:
            pdfmetrics.registerFont(TTFont(FONT_BOLD, bold_path))
        else:
            pdfmetrics.registerFont(TTFont(FONT_BOLD, regular_path))
        return True
    return False


# ── Color palette ─────────────────────────────────────────────────────────────

C_GREEN  = colors.HexColor("#1a7a1a")
C_ORANGE = colors.HexColor("#c47000")
C_RED    = colors.HexColor("#b01010")
C_BLUE   = colors.HexColor("#1a5276")
C_LIGHT  = colors.HexColor("#eaf0fb")
C_GRAY   = colors.HexColor("#666666")
C_BLACK  = colors.black


def _verdict_color(verdict: str) -> colors.Color:
    if verdict == "GENUINE":     return C_GREEN
    if verdict == "WARNING":     return C_ORANGE
    if verdict == "CANCELLED":   return C_GRAY
    return C_RED


# ── Style helpers ─────────────────────────────────────────────────────────────

def _styles(has_thai_font: bool):
    base = FONT_NAME if has_thai_font else "Helvetica"
    bold = FONT_BOLD if has_thai_font else "Helvetica-Bold"
    return {
        "normal":   ParagraphStyle("Normal",   fontName=base, fontSize=10, leading=14),
        "small":    ParagraphStyle("Small",    fontName=base, fontSize=8,  leading=12, textColor=C_GRAY),
        "bold":     ParagraphStyle("Bold",     fontName=bold, fontSize=10, leading=14),
        "heading":  ParagraphStyle("Heading",  fontName=bold, fontSize=13, leading=18, textColor=C_BLUE),
        "title":    ParagraphStyle("Title",    fontName=bold, fontSize=22, leading=28, alignment=1),
        "subtitle": ParagraphStyle("Subtitle", fontName=base, fontSize=11, leading=16, alignment=1, textColor=C_GRAY),
        "verdict":  ParagraphStyle("Verdict",  fontName=bold, fontSize=28, leading=34, alignment=1),
    }


def _table_style_base():
    return TableStyle([
        ("FONTNAME",    (0, 0), (-1, -1), FONT_NAME),
        ("FONTSIZE",    (0, 0), (-1, -1), 9),
        ("FONTNAME",    (0, 0), (0, -1),  FONT_BOLD),   # first column bold
        ("BACKGROUND",  (0, 0), (-1, 0),  C_LIGHT),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f7f7f7")]),
        ("GRID",        (0, 0), (-1, -1), 0.4, colors.HexColor("#cccccc")),
        ("TOPPADDING",  (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING",(0, 0), (-1, -1), 4),
        ("LEFTPADDING", (0, 0), (-1, -1), 8),
        ("RIGHTPADDING",(0, 0), (-1, -1), 8),
    ])


# ── Chunk map ─────────────────────────────────────────────────────────────────

def _draw_chunk_map(canvas, x, y, width, height, chunks):
    """Draw a colour-coded chunk grid directly onto the canvas."""
    n = len(chunks)
    if n == 0:
        return

    cols = 40
    rows = (n + cols - 1) // cols
    cw = width  / cols
    ch = max(6.0, height / rows)

    color_map = {
        "PENDING":    (0.82, 0.82, 0.78),
        "WRITTEN":    (0.28, 0.56, 0.76),
        "OK":         (0.14, 0.63, 0.14),
        "MISMATCH":   (0.80, 0.18, 0.18),
        "UNREADABLE": (0.60, 0.35, 0.05),
    }

    for i, chunk in enumerate(chunks):
        col = i % cols
        row = i // cols
        cx  = x + col * cw + 1
        cy  = y - row * ch - 1
        r, g, b = color_map.get(chunk.get("status", "PENDING"), (0.5, 0.5, 0.5))
        canvas.setFillColorRGB(r, g, b)
        canvas.rect(cx, cy - ch + 2, cw - 2, ch - 2, fill=1, stroke=0)


# ── Main generator ────────────────────────────────────────────────────────────

def generate(data: dict, out_path: str):
    has_font = _register_fonts()
    st = _styles(has_font)

    doc = SimpleDocTemplate(
        out_path,
        pagesize=A4,
        leftMargin=2*cm, rightMargin=2*cm,
        topMargin=2*cm,  bottomMargin=2*cm,
        title="FlashVerify Report",
        author="FlashVerify",
    )

    story = []
    W = A4[0] - 4*cm   # usable width

    # ── Title ──────────────────────────────────────────────────────────────
    story.append(Paragraph("FlashVerify", st["title"]))
    story.append(Paragraph("USB / SSD Authenticity Test Report", st["subtitle"]))
    story.append(Spacer(1, 0.3*cm))
    story.append(HRFlowable(width="100%", thickness=1.5, color=C_BLUE))
    story.append(Spacer(1, 0.4*cm))

    # ── Verdict banner ─────────────────────────────────────────────────────
    verdict      = data.get("verdict", "UNKNOWN")
    verdict_col  = _verdict_color(verdict)
    ok_chunks    = data.get("ok_chunks",   0)
    total_chunks = data.get("total_chunks",1)
    fail_chunks  = data.get("fail_chunks", 0)
    pct_ok       = ok_chunks / total_chunks * 100 if total_chunks else 0

    banner_style = ParagraphStyle(
        "Banner",
        fontName=FONT_BOLD if has_font else "Helvetica-Bold",
        fontSize=28, leading=34, alignment=1,
        textColor=verdict_col,
    )
    story.append(Paragraph(verdict, banner_style))
    sub = f"{ok_chunks} / {total_chunks} chunks OK ({pct_ok:.1f}%)   |   {fail_chunks} failed"
    story.append(Paragraph(sub, st["subtitle"]))
    story.append(Spacer(1, 0.5*cm))
    story.append(HRFlowable(width="100%", thickness=0.5, color=C_GRAY))
    story.append(Spacer(1, 0.4*cm))

    # ── Report metadata ────────────────────────────────────────────────────
    story.append(Paragraph("Report Information", st["heading"]))
    story.append(Spacer(1, 0.2*cm))
    meta_data = [
        ["Report ID",   data.get("report_id",    "—")],
        ["Date",        data.get("report_date",   "—")],
        ["Time",        data.get("report_time",   "—")],
        ["Software",    data.get("software_ver",  "—")],
    ]
    meta_table = Table(meta_data, colWidths=[4*cm, W - 4*cm])
    meta_table.setStyle(_table_style_base())
    story.append(meta_table)
    story.append(Spacer(1, 0.5*cm))

    # ── Device info ────────────────────────────────────────────────────────
    story.append(Paragraph("Device Information", st["heading"]))
    story.append(Spacer(1, 0.2*cm))
    dev_data = [
        ["Device Path",   data.get("device_path",   "—")],
        ["Device Name",   data.get("device_name",   "—")],
        ["Serial",        data.get("device_serial", "—")],
        ["Vendor ID",     data.get("vendor_id",     "—")],
    ]
    dev_table = Table(dev_data, colWidths=[4*cm, W - 4*cm])
    dev_table.setStyle(_table_style_base())
    story.append(dev_table)
    story.append(Spacer(1, 0.5*cm))

    # ── Capacity ───────────────────────────────────────────────────────────
    story.append(Paragraph("Capacity Analysis", st["heading"]))
    story.append(Spacer(1, 0.2*cm))

    claimed_gb = data.get("claimed_gb", 0)
    actual_gb  = data.get("actual_gb",  0)
    cap_diff   = actual_gb - claimed_gb

    cap_data = [
        ["Claimed capacity", f"{claimed_gb:.2f} GB  ({data.get('claimed_bytes',0):,} bytes)"],
        ["Actual capacity",  f"{actual_gb:.2f} GB  ({data.get('actual_bytes',0):,} bytes)"],
        ["Difference",       f"{cap_diff:+.2f} GB"],
    ]
    cap_table = Table(cap_data, colWidths=[5*cm, W - 5*cm])
    ts = _table_style_base()
    # highlight negative difference in red
    if cap_diff < 0:
        ts.add("TEXTCOLOR", (1, 2), (1, 2), C_RED)
        ts.add("FONTNAME",  (1, 2), (1, 2), FONT_BOLD if has_font else "Helvetica-Bold")
    cap_table.setStyle(ts)
    story.append(cap_table)
    story.append(Spacer(1, 0.5*cm))

    # ── Test results ───────────────────────────────────────────────────────
    story.append(Paragraph("Test Results", st["heading"]))
    story.append(Spacer(1, 0.2*cm))
    res_data = [
        ["Hash algorithm",  data.get("hash_algo",        "SHA-256")],
        ["Chunk size",      f"{data.get('chunk_size_mb',64)} MB"],
        ["Total chunks",    str(total_chunks)],
        ["OK chunks",       str(ok_chunks)],
        ["Failed chunks",   str(fail_chunks)],
        ["Write speed",     f"{data.get('write_speed_mbps',0):.1f} MB/s"],
        ["Read speed",      f"{data.get('read_speed_mbps',0):.1f} MB/s"],
    ]
    res_table = Table(res_data, colWidths=[5*cm, W - 5*cm])
    rts = _table_style_base()
    if fail_chunks > 0:
        rts.add("TEXTCOLOR", (1, 4), (1, 4), C_RED)
        rts.add("FONTNAME",  (1, 4), (1, 4), FONT_BOLD if has_font else "Helvetica-Bold")
    res_table.setStyle(rts)
    story.append(res_table)
    story.append(Spacer(1, 0.5*cm))

    # ── Order info ─────────────────────────────────────────────────────────
    order_id = data.get("order_id", "")
    if order_id:
        story.append(Paragraph("Purchase Information", st["heading"]))
        story.append(Spacer(1, 0.2*cm))
        ord_data = [
            ["Order ID",    data.get("order_id",        "—")],
            ["Shop",        data.get("shop_name",        "—")],
            ["Platform",    data.get("platform",         "—")],
            ["Price",       data.get("purchase_price",   "—")],
        ]
        ord_table = Table(ord_data, colWidths=[4*cm, W - 4*cm])
        ord_table.setStyle(_table_style_base())
        story.append(ord_table)
        story.append(Spacer(1, 0.5*cm))

    # ── Fail samples ───────────────────────────────────────────────────────
    samples = data.get("fail_chunk_samples", [])
    if samples:
        story.append(Paragraph("Failed Chunk Samples", st["heading"]))
        story.append(Spacer(1, 0.2*cm))
        hdr = [["#", "Index", "Offset (bytes)", "Expected hash (prefix)"]]
        rows = [
            [str(i+1),
             str(s.get("index", "?")),
             f"{s.get('offset', 0):,}",
             s.get("expected", "—")]
            for i, s in enumerate(samples)
        ]
        s_table = Table(hdr + rows,
                        colWidths=[1*cm, 2*cm, 5*cm, W - 8*cm])
        sts2 = _table_style_base()
        sts2.add("BACKGROUND", (0, 0), (-1, 0), C_BLUE)
        sts2.add("TEXTCOLOR",  (0, 0), (-1, 0), colors.white)
        sts2.add("FONTNAME",   (0, 0), (-1, 0), FONT_BOLD if has_font else "Helvetica-Bold")
        s_table.setStyle(sts2)
        story.append(s_table)
        story.append(Spacer(1, 0.5*cm))

    # ── Chunk map placeholder (drawn on canvas via later pass) ─────────────
    # We embed chunk map as a note since platypus doesn't support canvas drawing mid-story.
    # For full chunk map, a custom Flowable is used below.
    chunks_raw = data.get("chunks", [])
    if chunks_raw:
        from reportlab.platypus import Flowable

        class ChunkMapFlowable(Flowable):
            def __init__(self, chunks, width, height=100):
                super().__init__()
                self.chunks  = chunks
                self.width   = width
                self._height = height

            def wrap(self, availWidth, availHeight):
                return self.width, self._height

            def draw(self):
                _draw_chunk_map(self.canv, 0, self._height,
                                self.width, self._height, self.chunks)

        story.append(Paragraph("Chunk Map", st["heading"]))
        story.append(Spacer(1, 0.2*cm))
        story.append(ChunkMapFlowable(chunks_raw, W, height=120))

        # legend
        legend_style = ParagraphStyle(
            "Legend",
            fontName=FONT_NAME if has_font else "Helvetica",
            fontSize=8, leading=12, textColor=C_GRAY,
        )
        story.append(Paragraph(
            "■ OK &nbsp;&nbsp; ■ Mismatch &nbsp;&nbsp; ■ Unreadable &nbsp;&nbsp; ■ Written",
            legend_style))
        story.append(Spacer(1, 0.5*cm))

    # ── Footer note ────────────────────────────────────────────────────────
    story.append(HRFlowable(width="100%", thickness=0.5, color=C_GRAY))
    story.append(Spacer(1, 0.2*cm))
    story.append(Paragraph(
        f"Generated by FlashVerify {data.get('software_ver','1.0.0')} · "
        f"{data.get('report_date','')} {data.get('report_time','')}",
        st["small"]))

    doc.build(story)
    print(f"PDF saved: {out_path}")


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="FlashVerify PDF Report Generator")
    parser.add_argument("--input",  required=True,  help="Path to session JSON")
    parser.add_argument("--output", required=True,  help="Output PDF path")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: input file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    with open(args.input, "r", encoding="utf-8", errors="replace") as f:
        data = json.load(f)

    generate(data, args.output)


if __name__ == "__main__":
    main()
