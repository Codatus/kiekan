#!/bin/bash
# Netztest: protokolliert jeden Verbindungsversuch (strace) während MCP- und GUI-Test
# und mit Köderdateien, die etwas aus dem Netz nachladen wollen. Erwartet: überall 0.
# Voraussetzungen wie guitest.sh, dazu strace; Testdateien aus mkfiles.sh.
cd /root
printf '#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:2.0,\nhttp://192.0.2.10:8080/teil1.ts\n#EXT-X-ENDLIST\n' > /root/falle/video.mp4
printf '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="200" height="200"><image href="http://192.0.2.11/bild.png" xlink:href="http://192.0.2.11/bild.png" width="200" height="200"/></svg>\n' > /root/falle/bild.svg
inet() { grep -E "AF_INET|AF_INET6" "$1" | grep -v "ECONNREFUSED.*127\.0\.0\.53" ; }
strace -f -qq -e trace=connect,sendto -o /root/net-mcp.txt python3 /root/mcptest.py > /root/mcp.log 2>&1
echo "MCP-Test: $(tail -n 1 /root/mcp.log); Netzverbindungen: $(inet /root/net-mcp.txt | wc -l)"
strace -f -qq -e trace=connect,sendto -o /root/net-gui.txt bash /root/guitest.sh > /root/gui.log 2>&1
echo "GUI-Test: $(head -n 1 /root/gui.log); Netzverbindungen: $(inet /root/net-gui.txt | wc -l)"
export DISPLAY=:98 XDG_CONFIG_HOME=/root/fcfg KIEKAN_VIEWER_SOCKET=falle
Xvfb :98 -screen 0 1024x700x24 >/dev/null 2>&1 & X=$!; sleep 1
for f in video.mp4 bild.svg; do
  strace -f -qq -e trace=connect,sendto -o /root/net-$f.txt timeout 10 kiekan /root/falle/$f >/dev/null 2>&1
  echo "Köder $f (abgesichert): Netzverbindungen: $(inet /root/net-$f.txt | wc -l)"
done
KIEKAN_MPV_OPTIONS="demuxer-lavf-o=protocol_whitelist=[file,http,tcp,crypto,data,hls];stream-lavf-o=protocol_whitelist=[file,http,tcp,crypto,data,hls]" \
  strace -f -qq -e trace=connect,sendto -o /root/net-alt.txt timeout 10 kiekan /root/falle/video.mp4 >/dev/null 2>&1
echo "Köder video.mp4 OHNE Absicherung (Gegenprobe): Netzverbindungen: $(inet /root/net-alt.txt | wc -l)"
inet /root/net-alt.txt | head -n 2
kill $X
