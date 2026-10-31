#!/bin/bash
# Erzeugt das Manifest für den Flathub-Antrag: wie das lokale Manifest, aber
# KiekAn! selbst aus dem Git-Tag (Flathub baut nur aus festen Quellen).
# Aufruf: packaging/flatpak/flathub-manifest.sh v0.13.0 > io.github.codatus.kiekan.yml
set -euo pipefail
tag="$1"
commit=$(git rev-parse "$tag^{commit}")
here=$(dirname "$0")
python3 - "$here/io.github.codatus.kiekan.yml" "$tag" "$commit" <<'PY'
import sys
path, tag, commit = sys.argv[1:4]
text = open(path, encoding='utf-8').read()
old = """      - type: dir
        path: ../.."""
new = f"""      - type: git
        url: https://github.com/Codatus/kiekan.git
        tag: {tag}
        commit: {commit}"""
assert old in text, 'Quelle von kiekan nicht gefunden'
print(text.replace(old, new), end='')
PY
