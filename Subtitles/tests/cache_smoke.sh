#!/bin/sh
set -eu
worker="$1"
export STUDIO_FAKE_SCRIPT="$2"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
python3 - "$work/audio.wav" "$work/silent.wav" <<'PY'
import math
import struct
import sys
import wave

with wave.open(sys.argv[1], 'wb') as out:
    out.setnchannels(1)
    out.setsampwidth(2)
    out.setframerate(16000)
    out.writeframes(b''.join(struct.pack('<h', round(4000 * math.sin(2 * math.pi * 220 * i / 16000))) for i in range(16000)))
with wave.open(sys.argv[2], 'wb') as out:
    out.setnchannels(1)
    out.setsampwidth(2)
    out.setframerate(16000)
    out.writeframes(b'\0\0' * 16000)
PY
printf 'model one' > "$work/model.bin"
export STUDIO_FAKE_LOG="$work/calls"
cat > "$work/whisper.sh" <<'EOF'
#!/bin/sh
printf 'run\n' >> "$STUDIO_FAKE_LOG"
exec /bin/sh "$STUDIO_FAKE_SCRIPT" "$@"
EOF
chmod +x "$work/whisper.sh"
export STUDIO_FAKE_REQUIRE_VAD=0
if "$worker" --audio "$work/audio.wav" --model "$work/model.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/without-vad" > "$work/missing-vad.json" 2> "$work/missing-vad.err"; then
    echo 'Recognition without a VAD model unexpectedly produced subtitles' >&2
    exit 1
fi
test ! -e "$work/calls"
export STUDIO_FAKE_REQUIRE_VAD=1
printf 'vad one' > "$work/vad.bin"
mkdir "$work/out"
python3 - "$work" <<'PY'
import hashlib
import pathlib
import sys
work = pathlib.Path(sys.argv[1])
source = hashlib.sha256((work/'audio.wav').read_bytes()).hexdigest()[:16]
model = hashlib.sha256((work/'model.bin').read_bytes()).hexdigest()[:16]
(work/'out'/f'{source}-{model}-whisper.json').write_text('{"transcription":[{"timestamps":{"from":"00:00:00,400","to":"00:00:02,200"},"text":" Устаревший кэш"}]}')
vad = hashlib.sha256((work/'vad.bin').read_bytes()).hexdigest()[:16]
(work/'out'/f'{source}-{model}-asr-vad-v1-{vad}-whisper.json').write_text('{"params":{"token_timestamps":"original"},"transcription":[{"offsets":{"from":400,"to":2200},"text":" Слова через паузу"}]}')
PY
"$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/out" > "$work/first.json" 2> "$work/first.log"
grep -Fq 'Распознавание речи: 50%' "$work/first.log"
python3 - "$work/first.json" <<'PY'
import json
import pathlib
import sys
words = json.loads(pathlib.Path(json.loads(pathlib.Path(sys.argv[1]).read_text())['words']).read_text())['words']
assert [word['text'] for word in words] == ['Привет', 'мир']
assert words[0]['start_ms'] == 400 and words[1]['end_ms'] == 1800
PY
"$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/out" \
    --max-width 59 --font 'DejaVu Sans' --font-size 60 --bold 0 --text-color '#80FF0000' \
    --outline-color '#FF00FF00' --background-color '#660000FF' --outline 4 --shadow 2 \
    --background 2 --position 2 --safe-margin 12 --animation pop > "$work/second.json"
grep -Fq '"cache_hit":false' "$work/second.json"
python3 - "$work/second.json" <<'PY'
import json
import pathlib
import sys
ass = pathlib.Path(json.loads(pathlib.Path(sys.argv[1]).read_text())['ass']).read_text()
assert 'DejaVu Sans,60,&H7F0000FF' in ass
assert '&H0000FF00,&H99FF0000,0,0,0,0,100,100,0,0,1,4,2,8,80,80,132,1' in ass
assert '&H99FF0000,&H99FF0000,0,0,0,0,100,100,0,0,3,4,0,8,80,80,132,1' in ass
assert r'\fad(70,90)' in ass and r'\fscx108' in ass
PY
test "$(wc -l < "$work/calls")" -eq 1
"$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/out" > "$work/third.json"
grep -Fq '"cache_hit":true' "$work/third.json"
printf 'model two' > "$work/model.bin"
"$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/out" > "$work/fourth.json"
test "$(wc -l < "$work/calls")" -eq 2
if "$worker" --audio "$work/silent.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/out" > "$work/silent.json" 2> "$work/silent.err"; then
    echo 'Silent WAV unexpectedly produced subtitles' >&2
    exit 1
fi
grep -Fq 'почти неслышный' "$work/silent.err"
test "$(wc -l < "$work/calls")" -eq 2
printf 'vad two' > "$work/vad.bin"
"$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/out" > "$work/vad-changed.json"
grep -Fq '"cache_hit":false' "$work/vad-changed.json"
test "$(wc -l < "$work/calls")" -eq 3
export STUDIO_FAKE_NO_SPEECH=1
if "$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/no-speech" > "$work/no-speech.json" 2> "$work/no-speech.err"; then
    echo 'No speech response unexpectedly produced subtitles' >&2
    exit 1
fi
test -z "$(find "$work/no-speech" -name '*.ass' -o -name '*.blocks.json' -o -name '*.words.json')"
test "$(wc -l < "$work/calls")" -eq 4
unset STUDIO_FAKE_NO_SPEECH
export STUDIO_FAKE_TOKEN_TIMESTAMPS=processed
if "$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/unmapped" > "$work/unmapped.json" 2> "$work/unmapped.err"; then
    echo 'Processed token timestamps unexpectedly produced subtitles' >&2
    exit 1
fi
test -z "$(find "$work/unmapped" -name '*.ass' -o -name '*.blocks.json' -o -name '*.words.json')"
unset STUDIO_FAKE_TOKEN_TIMESTAMPS
export STUDIO_FAKE_PAUSE=1
"$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/pause" > "$work/pause.json"
python3 - "$work/pause.json" <<'PY'
import json
import pathlib
import sys
result = json.loads(pathlib.Path(sys.argv[1]).read_text())
words = json.loads(pathlib.Path(result['words']).read_text())['words']
blocks = json.loads(pathlib.Path(result['blocks']).read_text())['blocks']
assert [word['text'] for word in words] == ['Привет', 'я', 'здесь']
assert words[1]['start_ms'] == 11000 and words[1]['end_ms'] == 11300
for item in words + blocks:
    assert item['start_ms'] >= 3000 and item['end_ms'] <= 15000
    assert not (item['start_ms'] < 10500 and item['end_ms'] > 6500)
    assert any(item['start_ms'] >= start and item['end_ms'] <= end for start, end in [(3000, 6000), (11000, 15000)])
def milliseconds(value):
    hours, minutes, seconds = value.split(':')
    return round((int(hours)*3600 + int(minutes)*60 + float(seconds))*1000)
for line in pathlib.Path(result['ass']).read_text().splitlines():
    if line.startswith('Dialogue:'):
        fields = line.split(',', 3)
        start, end = milliseconds(fields[1]), milliseconds(fields[2])
        assert start >= 3000 and end <= 15000
        assert not (start < 10500 and end > 6500)
        assert any(start >= a and end <= b for a, b in [(3000, 6000), (11000, 15000)])
PY
unset STUDIO_FAKE_PAUSE
export STUDIO_FAKE_OMIT_VAD_SPANS=1
if "$worker" --audio "$work/audio.wav" --model "$work/model.bin" --vad-model "$work/vad.bin" --whisper-cli "$work/whisper.sh" --out-dir "$work/missing-spans" > "$work/missing-spans.json" 2> "$work/missing-spans.err"; then
    echo 'Missing native VAD speech intervals unexpectedly produced subtitles' >&2
    exit 1
fi
test -z "$(find "$work/missing-spans" -name '*.ass' -o -name '*.blocks.json' -o -name '*.words.json')"
unset STUDIO_FAKE_OMIT_VAD_SPANS
"$worker" --preview-text 'Пример субтитров' --max-width 59 --font 'DejaVu Sans' --font-size 60 --bold 0 \
    --text-color '#80FF0000' --outline-color '#FF00FF00' --background-color '#660000FF' \
    --outline 4 --shadow 2 --background 2 --position 2 --safe-margin 12 --animation pop > "$work/preview.ass"
python3 - "$work/second.json" "$work/preview.ass" <<'PY'
import json
import pathlib
import sys
exported = pathlib.Path(json.loads(pathlib.Path(sys.argv[1]).read_text())['ass']).read_text()
preview = pathlib.Path(sys.argv[2]).read_text()
assert [line for line in exported.splitlines() if line.startswith('Style:')] == [line for line in preview.splitlines() if line.startswith('Style:')]
assert 'Пример субтитров' in preview and preview.count('Dialogue:') == 2
PY
cat > "$work/restyle-blocks.json" <<'EOF'
{"schema":3,"blocks":[{"id":"42","start_ms":400,"end_ms":2200,"text":"Исправлено вручную","words":[]}]}
EOF
"$worker" --render-blocks-json "$work/restyle-blocks.json" > "$work/restyled.ass"
grep -Fq 'Исправлено вручную' "$work/restyled.ass"
