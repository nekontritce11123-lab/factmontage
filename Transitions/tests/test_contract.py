#!/usr/bin/env python3
"""Small contract check for transition assets."""
from pathlib import Path
import json
import array
import hashlib
import math
import subprocess
import sys
import wave
import xml.etree.ElementTree as E

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(ROOT / "scripts/generate_ui.py"), "--check"], check=True)
xml = E.parse(ROOT / "kdenlive/studio_transition.xml").getroot()
panel = json.loads((ROOT / "kdenlive/studio_transition.json").read_text(encoding="utf-8"))
previews = json.loads((ROOT / "kdenlive/transition_previews.json").read_text(encoding="utf-8"))
assert xml.attrib == {"tag": "frei0r.sunimo_transition", "id": "studio_transition",
                      "type": "videotransition", "LC_NUMERIC": "C"}
assert [node.attrib["name"] for node in xml.findall("parameter")] == [str(index) for index in range(7)] + ["studio:audio_dip", "studio:sfx_enabled", "studio:sfx_level", "studio:sfx_source", "studio:sfx_id"]
source = next(item for item in panel["controls"] if item["key"] == "sfx_source")
assert [option["value"] for option in source["options"]] == [""] + [sound.name for sound in sorted((ROOT / "sfx").glob("transition_*.wav"))]
assert panel["asset"] == "studio_transition" and panel["default_duration"] == .65
assert next(control for control in panel["controls"] if control["key"] == "audio_dip")["value"] == 25
assert len(panel["controls"][0]["options"]) == 15 and len(previews["items"]) == 15
assert all(abs(float(code) - i / 11) < 1e-11 for i, code in enumerate(xml.find("parameter[@name='1']").attrib["paramlist"].split(";")[:12]))
assert all(item["png"].startswith("transition_") and item["gif"].startswith("transition_")
           for item in previews["items"])
sounds = sorted((ROOT / "sfx").glob("transition_*.wav"))
assert len(sounds) == 16
assert len({hashlib.sha256(sound.read_bytes()).hexdigest() for sound in sounds}) == 16
for sound in sounds:
    with wave.open(str(sound)) as stream:
        assert stream.getnchannels() == 2 and stream.getframerate() == 48000 and stream.getsampwidth() == 2
        assert .25 <= stream.getnframes() / stream.getframerate() <= .65
        values = array.array("h", stream.readframes(stream.getnframes()))
        rms = math.sqrt(sum(value * value for value in values) / len(values)) / 32768
        peak = max(abs(value) for value in values) / 32768
        assert .048 <= rms <= .052 and peak <= .401, (sound, rms, peak)
print("PASS transition contract: 15 styles, 16 sourced and levelled sounds")
