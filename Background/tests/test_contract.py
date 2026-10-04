#!/usr/bin/env python3
"""Small contract guard: generated files and the public v1 surface stay aligned."""
from pathlib import Path
import json
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(ROOT / "scripts/generate_ui.py"), "--check"], check=True)

contract = json.loads((ROOT / "kdenlive/studio_background.json").read_text(encoding="utf-8"))
assert contract["version"] == 2
assert contract["effect"] == "studio_background"
assert contract["service"] == "studio.background"
assert len(contract["controls"]) == 8
assert len(contract["presets"]) == 6
for preset in contract["presets"]:
    for suffix in ("png", "gif"):
        assert (ROOT / f'{preset["preview"]}.{suffix}').is_file()
assert contract["analysis_sizes"] == [512, 768, 1280]

keys = [item["key"] for item in contract["parameters"]]
assert keys == [
    "method", "output", "key_r", "key_g", "key_b", "tolerance", "transition",
    "despill", "feather", "edge_shift", "refine", "blur", "fill_r", "fill_g", "fill_b",
]
assert not {"invert", "strokes", "mask_output"}.intersection(keys)
assert contract["hidden_project_properties"] == [
    "mask_asset", "source_sha256", "recipe_sha256", "sample_offset", "analysis_size", "static_image",
]

xml = ET.parse(ROOT / "kdenlive/studio_background.xml").getroot()
assert xml.attrib == {"tag": "studio.background", "id": "studio_background", "type": "hidden"}
assert [node.attrib["name"] for node in xml.findall("parameter")] == keys + contract["hidden_project_properties"]
print("Background contract OK")
