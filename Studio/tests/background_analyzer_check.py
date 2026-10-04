#!/usr/bin/env python3
"""Real installed human segmentation and failure atomicity; not a visual quality test."""
import hashlib
from pathlib import Path
import selectors
import subprocess
import tempfile

MODEL = Path('/app/share/studio-background/models/pp_humansegv2_lite_portrait_static.onnx')
ANALYZER = '/app/bin/studio-background-analyzer'


def main():
    frame = bytes((70, 130, 190, 255)) * (64 * 64)
    with tempfile.TemporaryDirectory(prefix='studio фон ') as folder:
        root = Path(folder)
        output = root/'маска тест.sbg'
        args = [ANALYZER, '--model', str(MODEL), '--model-sha256', hashlib.sha256(MODEL.read_bytes()).hexdigest(),
                '--output', str(output), '--width', '64', '--height', '64', '--frames', '2',
                '--fps-num', '60', '--fps-den', '1', '--source-sha256', hashlib.sha256(frame).hexdigest(),
                '--recipe-sha256', 'a' * 64, '--threads', '2']
        complete = subprocess.run(args, input=frame*2, capture_output=True, timeout=90)
        assert complete.returncode == 0, complete.stderr
        original = output.read_bytes()
        assert original[:8] == b'SBG0001\0' and b'"published":true' in complete.stdout
        repeated = subprocess.run(args, input=frame*2, capture_output=True, timeout=90)
        assert repeated.returncode != 0 and output.read_bytes() == original
        args[args.index('--output') + 1] = str(root/'incomplete.sbg')
        truncated = subprocess.run(args, input=frame, capture_output=True, timeout=90)
        assert truncated.returncode != 0 and not (root/'incomplete.sbg').exists()
        process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            process.stdin.write(frame)
            process.stdin.flush()
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                assert selector.select(90), 'Human segmentation did not report a processed frame'
                assert b'"done":1' in process.stdout.readline()
            process.terminate()
            process.stdin.close()
            process.wait(timeout=15)
            assert process.returncode != 0 and not (root/'incomplete.sbg').exists()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        assert output.read_bytes() == original
        assert not list(root.glob('*.part.*'))
    print('Real human segmentation: inference, no overwrite, truncated input, cancellation: PASS')


if __name__ == '__main__':
    main()
