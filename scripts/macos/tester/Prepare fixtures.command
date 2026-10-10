#!/bin/bash
set -euo pipefail
test_root="$(cd -- "$(dirname -- "$0")" && pwd)"
playlist="$test_root/fixtures/local.m3u"
cat > "$playlist" <<EOF
#EXTM3U
#EXTINF:-1 tvg-id="local.fixture" group-title="Local tests",Local video fixture
$test_root/fixtures/H264-AAC.mp4
EOF
epg_start="$(/bin/date -u -v-1H '+%Y%m%d%H%M%S +0000')"
epg_stop="$(/bin/date -u -v+2H '+%Y%m%d%H%M%S +0000')"
cat > "$test_root/fixtures/local.xml" <<EOF
<?xml version="1.0"?>
<tv><channel id="local.fixture"><display-name>Local video fixture</display-name></channel>
<programme start="$epg_start" stop="$epg_stop" channel="local.fixture"><title>Local playback test</title></programme></tv>
EOF
echo "Fixture playlist: $playlist"
echo "Add it in Settings > Sources as a local M3U file."
echo "Use fixtures/local.xml as the XMLTV file, and sample subtitles.srt to test subtitle uploads."
