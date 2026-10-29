#!/bin/bash
# GUI-Test unter Xvfb (unsichtbar): alle Formate durchblättern, Dialoge, Video
export DEBIAN_FRONTEND=noninteractive
command -v openbox >/dev/null || apt-get install -y -q --no-install-recommends openbox >/dev/null
O=/root/gui; mkdir -p $O; cd $O
export DISPLAY=:99 XDG_CONFIG_HOME=/root/guicfg XDG_DATA_HOME=/root/guidata KIEKAN_VIEWER_SOCKET=kt-gui
mkdir -p $XDG_CONFIG_HOME
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 & XV=$!
sleep 1
openbox >/dev/null 2>&1 &
sleep 1
shot() { import -window root "$O/$1.png"; }
key() { xdotool key --clearmodifiers "$@"; sleep "${WAIT:-1.2}"; }

kiekan /root/t/bilder/anim.gif > $O/stderr.log 2>&1 & KP=$!
sleep 4
xdotool search --sync --onlyvisible --class kiekan windowactivate >/dev/null 2>&1
i=0
for f in $(ls /root/t/bilder); do
    i=$((i+1)); n=$(printf "%02d" $i)
    case $f in video.mp4) sleep 3;; *.pdf|*.djvu|*.heic|*.avif) sleep 1.5;; esac
    shot "$n-$f"
    [ "$f" = video.mp4 ] && break
    key Next
done
key Home
key p;  shot "dlg-einstellungen"; key Escape
key i;  shot "dlg-info"; key Escape
key e;  shot "dlg-exif"; key Escape
key F1; shot "dlg-tasten"; key Escape
key ctrl+t; sleep 6; shot "dlg-ocr"; key Escape
key r;  shot "bearb-gedreht"; key ctrl+z
alive=$(kill -0 $KP 2>/dev/null && echo ja || echo nein)
echo "läuft noch: $alive"
xdotool key ctrl+q; sleep 2
kill -0 $KP 2>/dev/null && { echo "beendet sich nicht"; kill $KP; }
wait $KP 2>/dev/null; echo "exit: $?"
# Deutsche Oberfläche
LANG=de_DE.UTF-8 LANGUAGE=de kiekan /root/t/bilder/foto.jpg >> $O/stderr.log 2>&1 & KP=$!
sleep 4; key p; shot "de-einstellungen"; key Escape; xdotool key ctrl+q; sleep 2; kill $KP 2>/dev/null
kill $XV
echo "--- stderr"; grep -v "XDG_RUNTIME_DIR\|^$" $O/stderr.log | sort | uniq -c | sort -rn | head -20
