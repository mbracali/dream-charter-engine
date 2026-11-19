#!/usr/bin/env bash
# Linux check of gd-tool-02 in a Fedora container (podman or docker):
#   builds the app and the C++ tests, runs ctest, the Python tests (without torch), and
#   a headless smoke run of the app (offscreen video, dummy audio) that saves a screenshot.
#
#   gd-tool-02/scripts/linux_test.sh [OUT_DIR]   (OUT_DIR gets linux_screen0-3.bmp)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # gd-tool-02
REPO="$(cd "$HERE/.." && pwd)"                    # dream-charter-engine
OUT="${1:-$HERE/build/linux}"
mkdir -p "$OUT"
ENGINE="$(command -v podman || command -v docker)"
"$ENGINE" run --rm -v "$REPO":/src:ro -v "$OUT":/out fedora:41 bash -euo pipefail -c '
dnf -q -y install gcc-c++ cmake make SDL3-devel python3 python3-pip mesa-dri-drivers bc >/dev/null
cp -r /src /work && cd /work/gd-tool-02 && rm -rf build .venv
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" 2>&1 | grep -E "error|warning" || true
test -x build/gd-tool-02
ctest --test-dir build --output-on-failure
python3 -m venv .venv && .venv/bin/pip -q install numpy scipy soundfile mido pyyaml pytest pyguitarpro==0.11
PYTHONPATH=/work .venv/bin/python -m pytest -q python/tests
# Headless smoke run: a project with a chart, every screen drawn once.
mkdir -p /tmp/p/Song/forge/A && cd /tmp/p/Song
printf "project:\n  created: \"2026-01-01T00:00:00\"\n  audio_file: \"song.wav\"\nsong:\n  title: \"Linux Song\"\n  artist: \"Tester\"\n" > project.yaml
/work/gd-tool-02/.venv/bin/python -c "import numpy as n, soundfile as s; s.write(\"song.wav\", n.zeros((44100, 2)), 44100)"
{ echo "SONG 8"; for i in $(seq 0 15); do echo "BEAT $(echo "$i*0.5" | bc) $((i % 4 == 0))"; done
  echo "NOTE guitar 3 0.5 0 1 -"; echo "NOTE drums 3 1 0 2 c"; echo "VOCAL 1 0.4 60 hey"; } > forge/chart.txt
printf "settings:\n  projects_dir: \"/tmp/p\"\n" > /tmp/settings.yaml
cd /work/gd-tool-02
for screen in 0 1 2 3; do
  SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy ./build/gd-tool-02 --settings /tmp/settings.yaml --screen $screen \
    --screenshot /out/linux_screen$screen.bmp
  test -s /out/linux_screen$screen.bmp
done
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy ./build/gd-tool-02 --settings /tmp/settings.yaml --screen 2 --selftest
echo "LINUX OK"
'
