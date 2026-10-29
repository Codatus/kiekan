# Systemtest in Containern

Prüft ein installiertes KiekAn!-Paket ohne sichtbare Fenster, z. B. in einem
frischen incus-Container (Ubuntu 22.04/24.04, Debian 12/13):

1. `mkfiles.sh` legt unter `/root/t` Testdateien an (alle Formate, PDFs,
   Video, eine Datei mit eingeschleustem Text in den Metadaten).
   Braucht libheif-examples, djvulibre-bin, exiftool, ImageMagick, ffmpeg,
   Tesseract mit deutschen Sprachdaten.
2. `mcptest.py` ruft alle MCP-Werkzeuge auf und prüft Ergebnisse, Freigaben
   und die Erkennung von Prompt Injection (50 Prüfungen).
3. `guitest.sh` startet KiekAn! unter Xvfb, blättert durch alle Dateien,
   öffnet die Dialoge und legt Bildschirmfotos in `/root/gui` ab.

```bash
incus launch images:ubuntu/24.04 kt-noble
incus file push kiekan_*_ubuntu24.04_amd64.deb kt-noble/root/kiekan.deb
incus exec kt-noble -- apt-get install -y /root/kiekan.deb python3 xvfb xdotool imagemagick openbox
```
