#!/usr/bin/env python3
# Rauchtest für fertige Pakete: KiekAn! über MCP jede angegebene Datei
# öffnen lassen (view_image). Aufruf: mcpsmoke.py <kiekan> <datei> …
import json, os, subprocess, sys

exe, files = sys.argv[1], [os.path.abspath(f) for f in sys.argv[2:]]
roots = os.pathsep.join(sorted({os.path.dirname(f) for f in files}))
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", KIEKAN_MCP_ROOTS=roots, KIEKAN_VIEWER_SOCKET="kiekan-smoke")
msgs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "1"}}}]
for i, f in enumerate(files):
    msgs.append({"jsonrpc": "2.0", "id": 10 + i, "method": "tools/call",
                 "params": {"name": "view_image", "arguments": {"path": f, "max_size": 200}}})
out = subprocess.run([exe, "--mcp"], input="".join(json.dumps(m) + "\n" for m in msgs),
                     capture_output=True, text=True, env=env, timeout=120).stdout
answers = {m.get("id"): m for m in map(json.loads, filter(None, out.splitlines()))}
failed = 0
for i, f in enumerate(files):
    r = answers.get(10 + i, {}).get("result", {})
    ok = r and not r.get("isError") and any(c.get("type") == "image" for c in r.get("content", []))
    print(("OK    " if ok else "FEHLER") + "  " + os.path.basename(f)
          + ("" if ok else "  " + json.dumps(r)[:300]))
    failed += not ok
sys.exit(1 if failed or 1 not in answers else 0)
