"""Exercise the real worker's complete and truncated MLT input streams."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

worker, model, model_sha = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source = root / 'source.bin'
    source.write_bytes(b'frame-count-fixture')
    output = root / 'mask.sbg'
    command = [worker, '--model', model, '--model-sha256', model_sha,
               '--source-file', str(source), '--output', str(output),
               '--width', '16', '--height', '16', '--frames', '3',
               '--fps-num', '30000', '--fps-den', '1001',
               '--reference-w', '1920', '--reference-h', '1080',
               '--recipe-sha256', hashlib.sha256(b'count-test').hexdigest()]
    frame = bytes((80, 110, 140, 255)) * (16 * 16)
    complete = subprocess.run(command, input=frame * 3, capture_output=True, timeout=40)
    assert complete.returncode == 0, complete.stderr.decode()
    assert b'"published":true' in complete.stdout
    assert struct.unpack_from('<III', output.read_bytes(), 20) == (3, 30000, 1001)
    previous = output.read_bytes()
    failed_output = root / 'failed.sbg'
    command[command.index('--output') + 1] = str(failed_output)
    truncated = subprocess.run(command, input=frame * 2 + b'partial', capture_output=True, timeout=40)
    assert truncated.returncode != 0
    assert 'Получено 2 из 3 кадров' in truncated.stderr.decode(), truncated.stderr.decode()
    assert b'"published":true' not in truncated.stdout
    assert output.read_bytes() == previous, 'A failed analysis replaced the previous mask'
    assert not failed_output.exists()
    assert not list(root.glob('*.part.*'))
print('Analyzer frame count and failure preservation: PASS')
