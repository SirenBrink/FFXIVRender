"""Validate moving-bar 3X/4X readbacks, not just SDK success or Present counts."""
import argparse
import hashlib
import json
from pathlib import Path


def validate(directory):
    evidence = []
    for frame, factor in ((25, 3), (45, 4), (85, 4)):
        positions, hashes = [], []
        for index in range(factor):
            path = directory / f'{frame}-{index}-1280x720-28.raw'
            data = path.read_bytes()
            assert len(data) == 1280 * 720 * 4, f'Incomplete image: {path}'
            weights = [max(data[x * 4] - 26, 0) for x in range(1280)]
            mass = sum(weights)
            assert mass > 0, f'Unwritten image: {path}'
            positions.append(sum(x * v for x, v in enumerate(weights)) / mass)
            hashes.append(hashlib.sha256(data).hexdigest())
        assert len(set(hashes)) == factor, f'{factor}X repeated an image at {frame}'
        assert all(a < b for a, b in zip(positions, positions[1:])), 'Interpolation order reversed'
        for i, position in enumerate(positions):
            expected = positions[-1] - 2 * (factor - 1 - i) / factor
            assert abs(position - expected) < .06, f'Incorrect interpolation phase: {positions}'
        evidence.append(dict(frame=frame, factor=factor, centroids=positions, sha256=hashes))
    return dict(passed=True, scope='SDR moving-bar pixel interpolation; not gameplay acceptance', images=evidence)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = json.dumps(validate(args.directory), indent=2)
    if args.output:
        args.output.write_text(result + '\n', encoding='utf-8')
    print(result)
