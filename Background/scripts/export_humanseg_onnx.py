#!/usr/bin/env python3
"""Export pinned PP-HumanSegV2-Lite weights outside the source tree.

Requires PaddleSeg at the pinned commit, PaddlePaddle 2.6.2 and Paddle2ONNX
1.3.1 in the current Python environment. No dependencies are downloaded here.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile

REVISION = '3c4db66de1d9d59d0628ed87590b6308a2f4aa2a'
WEIGHTS = 'a3dc76ae522327aa83c6c898dc4cd297f738c6b51c90860c74ed5bdd36e0a159'
MODEL = '2047b95bea344af15575f4279d6fac44bb07a42e7d537cf8d3265f15fe95cc82'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--paddleseg', type=Path, required=True)
    parser.add_argument('--weights', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.paddleseg.resolve()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    if revision != REVISION or digest(args.weights) != WEIGHTS:
        raise SystemExit('Неверная ревизия PaddleSeg или контрольная сумма весов.')
    paddle2onnx = Path(sys.executable).with_name('paddle2onnx')
    if not paddle2onnx.is_file():
        raise SystemExit('В текущем окружении отсутствует Paddle2ONNX 1.3.1.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='humanseg-export-') as directory:
        exported = Path(directory)
        config = (root/'contrib/PP-HumanSeg/configs/portrait_pp_humansegv2_lite.yml').read_text(encoding='utf-8')
        # The explicit local checkpoint supplies every weight; suppress PaddleSeg's
        # implicit downloads of backbone and duplicate pretrained weights.
        config = re.sub(r'(?m)^(\s*)pretrained: https://[^\n]+$', r'\1pretrained: null', config)
        local_config = exported/'model.yml'
        local_config.write_text(config, encoding='utf-8')
        subprocess.run([sys.executable, str(root/'tools/export.py'), '--config',
                        str(local_config),
                        '--model_path', str(args.weights.resolve()), '--save_dir', str(exported),
                        '--input_shape', '1', '3', '144', '256', '--output_op', 'softmax'], cwd=root, check=True)
        subprocess.run([str(paddle2onnx), '--model_dir', str(exported), '--model_filename', 'model.pdmodel',
                        '--params_filename', 'model.pdiparams', '--save_file', str(args.output),
                        '--opset_version', '14'], check=True)
    if digest(args.output) != MODEL:
        args.output.unlink(missing_ok=True)
        raise SystemExit('ONNX отличается от закреплённой модели; проверьте версии PaddlePaddle и Paddle2ONNX.')
    print(f'ONNX готова: {args.output} ({MODEL})')


if __name__ == '__main__':
    main()
