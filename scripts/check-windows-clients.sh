#!/usr/bin/env bash
# Run in an existing MSYS2 MINGW64 shell. No packages or physical devices.
set -euo pipefail
build="${1:-build-win-native}"
sdl="${2:-ON}"
case "$sdl" in ON|OFF) ;; *) echo 'SDL must be ON or OFF' >&2; exit 2;; esac
cmake -S . -B "$build" -G Ninja -DCMAKE_CXX_COMPILER="${CUE_CXX:-g++.exe}" \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG -g0' \
  -DCUE_ENABLE_SDL2="$sdl" -DCUE_SANITIZE=none
names=(wire frame_guard reassembly_budget upstream_reassembly capture_process probe_network
       capture probe_render_headless audio_playback audio_drift audio_decoder audio_receiver h264_decoder h264_dynamic avc_level)
if [ "$sdl" = ON ]; then names+=(probe_render_sdl); fi
targets=(cue-probe cue-capture)
for name in "${names[@]}"; do targets+=("test_$name"); done
cmake --build "$build" --target "${targets[@]}" -j1
pattern=$(IFS='|'; echo "${names[*]}")
ctest --test-dir "$build" --show-only=json-v1 > "$build/windows-client-tests.json"
test_exit=0
ctest --test-dir "$build" -j1 -R "^($pattern)$" --no-tests=error --output-on-failure \
  --output-junit windows-client-results.xml || test_exit=$?
python3 scripts/ci-regression.py --audit-native "$build" --sdl "$sdl"
exit "$test_exit"
