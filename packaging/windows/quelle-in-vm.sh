#!/bin/bash
# Kopiert den aktuellen Arbeitsstand (alle von git verfolgten Dateien) in den
# gemeinsamen Ordner der Test-VM; dort baut msys2-offline/bauen.cmd daraus.
set -euo pipefail
ZIEL=${1:-$HOME/VM-Austausch/kiekan-quelle}
cd "$(dirname "$0")/../.."
mkdir -p "$ZIEL"
git ls-files -z | rsync -a --from0 --files-from=- ./ "$ZIEL/"
echo "Quelle aktualisiert: $ZIEL"
