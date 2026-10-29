#!/usr/bin/env python3
# MCP-Gesamttest für KiekAn! im Container (headless)
import json, os, subprocess, sys

T = "/root/t"
B = f"{T}/bilder"
import shutil
for d in (f"{T}/ki", "/root/cfg", "/root/data"):
    shutil.rmtree(d, ignore_errors=True)
os.makedirs(f"{T}/ki")
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME="/root/cfg",
           XDG_DATA_HOME="/root/data", KIEKAN_VIEWER_SOCKET="kt-test",
           KIEKAN_MCP_ROOTS=f"{B}:{T}/fremd", KIEKAN_MCP_WRITE_ROOTS=f"{T}/ki",
           KIEKAN_MCP_TRUSTED_ROOTS=B)
p = subprocess.Popen(["kiekan", "--mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, env=env, text=True)
nid = 0
results = []

def rpc(method, params=None):
    global nid
    nid += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid, "method": method, "params": params or {}}) + "\n")
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            raise RuntimeError("MCP-Prozess beendet")
        m = json.loads(line)
        if m.get("id") == nid:
            return m

def call(name, args):
    m = rpc("tools/call", {"name": name, "arguments": args})
    r = m.get("result", {})
    text = "\n".join(c.get("text", "") for c in r.get("content", []) if c.get("type") == "text")
    images = sum(1 for c in r.get("content", []) if c.get("type") == "image")
    return r.get("isError", False), text, images

def check(label, ok, info=""):
    results.append((label, ok))
    print(("OK    " if ok else "FEHLER") + "  " + label + ("" if ok else "  →  " + info[:400].replace("\n", " ")))

rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "kt", "version": "1"}})
tools = rpc("tools/list")["result"]["tools"]
check("tools/list liefert 19 Werkzeuge", len(tools) == 19, str(len(tools)))

err, txt, _ = call("list_media", {"directory": B})
check("list_media", not err and "foto.jpg" in txt and "video.mp4" in txt, txt)

for f, fmt in [("foto.jpg", "1067"), ("bild.png", "800"), ("bild.webp", "800"), ("bild.tif", "800"),
               ("bild.bmp", "800"), ("bild.jp2", "800"), ("anim.gif", "200"), ("bild.heic", "800"),
               ("bild.avif", "800"), ("bild.djvu", "800"), ("vektor.svg", "300"), ("textpdf.pdf", ""),
               ("video.mp4", "video")]:
    err, txt, _ = call("image_info", {"path": f"{B}/{f}"})
    check(f"image_info {f}", not err and fmt in txt, txt)
    if f.endswith(".mp4"):
        continue
    err, txt, n = call("view_image", {"path": f"{B}/{f}", "max_size": 400})
    check(f"view_image {f}", not err and n == 1, txt)

err, txt, _ = call("read_metadata", {"path": f"{B}/foto.jpg"})
check("read_metadata EXIF/GPS", not err and "Testkamera" in txt and "53" in txt, txt)
err, txt, _ = call("histogram", {"path": f"{B}/bild.png"})
check("histogram", not err and len(txt) > 50, txt)
err, txt, _ = call("list_filters", {})
check("list_filters", not err and "id" in txt, txt)
err, txt, n = call("view_image", {"path": f"{B}/foto.jpg", "region": [100, 100, 300, 200], "grid": True})
check("view_image Ausschnitt + Raster", not err and n == 1, txt)

err, txt, _ = call("ocr_languages", {})
check("ocr_languages (deu, eng)", not err and "deu" in txt and "eng" in txt, txt)
err, txt, _ = call("extract_text", {"path": f"{B}/scan.png"})
check("extract_text OCR (Grüßen)", not err and "Grüßen" in txt and "Unterlagen" in txt, txt)
err, txt, _ = call("extract_text", {"path": f"{B}/textpdf.pdf"})
check("extract_text PDF-Textschicht", not err and "Eingang" in txt, txt)
err, txt, _ = call("extract_text", {"path": f"{B}/mehrseitig.pdf", "pages": [2], "force_ocr": True})
check("extract_text PDF-OCR Seite 2", not err and "Unterlagen" in txt, txt)

err, txt, n = call("edit_image", {"input": f"{B}/foto.jpg", "output": f"{T}/ki/foto-bearbeitet.jpg",
                                  "operations": [{"op": "rotate", "degrees": 90}, {"op": "resize", "max_size": 500},
                                                 {"op": "grayscale"}, {"op": "auto_levels"}]})
check("edit_image (drehen, verkleinern, grau)", not err and os.path.exists(f"{T}/ki/foto-bearbeitet.jpg"), txt)
err, txt, _ = call("read_metadata", {"path": f"{T}/ki/foto-bearbeitet.jpg"})
check("EXIF bleibt bei edit_image erhalten", not err and "Testkamera" in txt, txt)
err, txt, _ = call("edit_image", {"input": f"{B}/foto.jpg", "output": f"{T}/ki/foto-bearbeitet.jpg", "operations": [{"op": "invert"}]})
check("Überschreiben ohne Erlaubnis abgelehnt", err, txt)
err, txt, _ = call("edit_image", {"input": f"{B}/foto.jpg", "output": f"{B}/neu.jpg", "operations": [{"op": "invert"}]})
check("Schreiben in Leseordner abgelehnt", err and not os.path.exists(f"{B}/neu.jpg"), txt)
err, txt, _ = call("image_info", {"path": "/etc/passwd"})
check("Lesen außerhalb der Freigabe abgelehnt", err, txt)
err, txt, _ = call("image_info", {"path": f"{B}/../../../etc/hostname"})
check("Pfad mit .. außerhalb abgelehnt", err, txt)

err, txt, _ = call("convert_batch", {"inputs": [f"{B}/bild.png", f"{B}/bild.heic", f"{B}/bild.jp2"],
                                     "output_dir": f"{T}/ki/konv", "format": "webp", "max_size": 300})
check("convert_batch → webp", not err and len(os.listdir(f"{T}/ki/konv")) == 3, txt)
err, txt, n = call("contact_sheet", {"directory": B})
check("contact_sheet", not err and n >= 1, txt)

err, txt, _ = call("split_pdf", {"path": f"{B}/mehrseitig.pdf", "output_dir": f"{T}/ki/teile", "pages_per_part": 2})
parts = [f for f in os.listdir(f"{T}/ki/teile") if f.endswith(".pdf")] if os.path.isdir(f"{T}/ki/teile") else []
check("split_pdf in 3 Teile", not err and len(parts) == 3, txt)
err, txt, _ = call("pdf_parts", {"path": f"{T}/ki/teile", "page": 5, "verify": True})
check("pdf_parts Seite 5 → Teil 3", not err and "3" in txt, txt)

err, txt, _ = call("write_metadata", {"path": f"{T}/ki/foto-bearbeitet.jpg", "title": "Testtitel", "keywords": ["küste", "fisch"], "rating": 4})
check("write_metadata ohne Überschreib-Erlaubnis abgelehnt", err, txt)

# Prompt Injection
err, txt, _ = call("read_metadata", {"path": f"{T}/fremd/boese.png"})
check("Injection in Metadaten erkannt (SECURITY NOTICE)", not err and "SECURITY NOTICE" in txt and "FILE-CONTENT" in txt, txt)
err, txt, _ = call("read_metadata", {"path": f"{B}/foto.jpg"})
check("Kein Fehlalarm bei normaler Datei", not err and "SECURITY NOTICE" not in txt, txt)

err, txt, _ = call("viewer_state", {})
check("viewer_state ohne Fenster: sauberer Fehler", err and len(txt) > 10, txt)

log = "/root/data/kiekan/kiekan/ki-protokoll.jsonl"
check("KI-Protokoll geschrieben", os.path.exists(log) and sum(1 for _ in open(log)) >= 30, log)

p.stdin.close()
p.wait(timeout=10)
fails = [l for l, ok in results if not ok]
print(f"\n{len(results) - len(fails)}/{len(results)} bestanden")
sys.exit(1 if fails else 0)
