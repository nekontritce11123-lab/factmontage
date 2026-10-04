#!/bin/sh
set -eu
output=""
vad=0
vad_model=""
while [ "$#" -gt 0 ]; do
    if [ "$1" = "-of" ]; then
        shift
        output="$1"
    elif [ "$1" = "--vad" ]; then
        vad=1
    elif [ "$1" = "--vad-model" ]; then
        shift
        vad_model="$1"
    fi
    shift
done
test -n "$output"
if [ "${STUDIO_FAKE_REQUIRE_VAD:-1}" = 1 ]; then
    test "$vad" -eq 1
    test -f "$vad_model"
fi
printf 'whisper_print_progress_callback: progress =  50%%\n' >&2
if [ "${STUDIO_FAKE_NO_SPEECH:-0}" = 1 ]; then
    printf '{"params":{"token_timestamps":"original"},"vad_segments":[],"transcription":[]}\n' > "${output}.json"
    exit 0
fi
if [ "${STUDIO_FAKE_PAUSE:-0}" = 1 ]; then
    cat > "${output}.json" <<'EOF'
{"params":{"token_timestamps":"original"},"vad_segments":[{"offsets":{"from":3000,"to":6000}},{"offsets":{"from":11000,"to":15000}}],"transcription":[{"offsets":{"from":3000,"to":15000},"text":" Привет я здесь","tokens":[{"text":" Привет","offsets":{"from":5500,"to":5900},"p":0.98},{"text":" я","offsets":{"from":5900,"to":11300},"p":0.97},{"text":" здесь","offsets":{"from":11300,"to":12000},"p":0.99}]}]}
EOF
    exit 0
fi
token_timestamps="${STUDIO_FAKE_TOKEN_TIMESTAMPS:-original}"
vad_segments=',"vad_segments":[{"offsets":{"from":400,"to":2200}}]'
if [ "${STUDIO_FAKE_OMIT_VAD_SPANS:-0}" = 1 ]; then vad_segments=''; fi
cat > "${output}.json" <<EOF
{"params":{"token_timestamps":"$token_timestamps"}$vad_segments,"transcription":[{"timestamps":{"from":"00:00:00,400","to":"00:00:02,200"},"text":" Привет мир","tokens":[{"text":" Привет","offsets":{"from":400,"to":1100},"p":0.98},{"text":" мир","offsets":{"from":1100,"to":1800},"p":0.97}]}]}
EOF
