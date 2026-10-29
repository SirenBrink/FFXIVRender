"""Read always-on JSONL recordings, including rotations; never treats test success as symptom proof."""
import argparse
import collections
import json
import statistics
from pathlib import Path


def analyze(path):
    files = [Path(str(path) + f'.{n}') for n in range(7, 0, -1)] + [path]
    frequency = None
    pending = {}
    generations = {}
    warnings = []
    events = collections.Counter()
    lost = 0
    invalid = []
    seconds = {}
    origin = None
    first_sequence = None
    last_sequence = None
    last_present = {}
    scope_pending = collections.defaultdict(list)
    phase_times = collections.defaultdict(list)
    version = None
    for file in files:
        if not file.exists():
            continue
        for line_number, raw in enumerate(file.read_bytes().splitlines(), 1):
            try:
                row = json.loads(raw.decode('utf-8-sig'))
            except (ValueError, UnicodeError) as error:
                invalid.append(f'{file.name}:{line_number}: {error}')
                continue
            kind = row.get('kind', '')
            version = row.get('version', version)
            events[kind] += 1
            if 'seq' in row:
                if first_sequence is None:
                    first_sequence = row['seq']
                last_sequence = row['seq']
            frequency = row.get('qpc_frequency', frequency)
            if frequency and 'qpc' in row:
                if origin is None:
                    origin = row['qpc']
                second = max(0, int((row['qpc'] - origin) / frequency))
                sample = seconds.setdefault(second, dict(presents=0, displayed=0, max_present_ms=0,
                                                          max_sleep_ms=0, generation=row.get('generation')))
                if kind == 'present.end':
                    sample['presents'] += 1
                    sample['max_present_ms'] = max(sample['max_present_ms'], row['a'])
                elif kind == 'sleep.end':
                    sample['max_sleep_ms'] = max(sample['max_sleep_ms'], row['a'])
                elif kind == 'retirement':
                    sample['displayed'] += row.get('c', 0)
            if kind == 'records_dropped':
                lost += row['count']
            generation = row.get('generation', 0)
            if generation not in generations:
                generations[generation] = dict(provider=row.get('provider'), presents=[], sleeps=[], warnings=0,
                                               intervals=[], ungenerated=0, captures=0, handoffs=[])
            data = generations[generation]
            if kind == 'present.begin' and frequency and 'qpc' in row:
                previous = last_present.get(generation)
                if previous is not None:
                    data['intervals'].append((row['qpc'] - previous) * 1000 / frequency)
                last_present[generation] = row['qpc']
            if kind == 'scope.begin':
                scope_pending[(row['tid'], row['message'])].append(row)
            elif kind == 'scope.end':
                stack = scope_pending[(row['tid'], row['message'])]
                if stack:
                    stack.pop()
                phase_times[row['message']].append(row['a'])
            elif kind == 'phase.slow':
                phase_times['slow_only.' + row['message']].append(row['a'])
            if kind in ('handoff.begin', 'module.load.begin', 'streamline.shutdown.begin'):
                pending[(kind.rsplit('.', 1)[0], row['tid'])] = row
            elif kind in ('handoff.committed', 'handoff.close_failed', 'module.load.end', 'streamline.shutdown.end'):
                pending.pop((kind.rsplit('.', 1)[0], row['tid']), None)
            if kind in ('addon.shutdown.begin', 'reshade.effects.begin', 'streamline.bind.begin'):
                pending[(kind.rsplit('.', 1)[0], row['tid'])] = row
            elif kind in ('addon.shutdown.end', 'reshade.effects.end', 'streamline.bind.end'):
                previous = pending.pop((kind.rsplit('.', 1)[0], row['tid']), None)
                if previous and frequency and 'qpc' in previous and 'qpc' in row:
                    phase_times[kind.rsplit('.', 1)[0]].append((row['qpc'] - previous['qpc']) * 1000 / frequency)
            if kind in ('present.begin', 'sleep.begin'):
                pending[(kind.split('.')[0], row['tid'])] = row
            elif kind in ('present.end', 'sleep.end'):
                phase = kind.split('.')[0]
                pending.pop((phase, row['tid']), None)
                data['presents' if phase == 'present' else 'sleeps'].append(row['a'])
                if phase == 'present' and row.get('d') == 0:
                    data['ungenerated'] += 1
            elif kind == 'capture':
                data['captures'] += 1
            elif kind.startswith('handoff.') or kind.endswith('.destroy') or 'shutdown' in kind:
                data['handoffs'].append(dict(kind=kind, seq=row.get('seq'), result=row.get('a')))
            warning = (kind == 'sdk.streamline' and row.get('a', 0) >= 1 or
                       kind in ('sdk.xefg', 'sdk.xell') and row.get('a', 0) >= 2 or
                       kind == 'sdk.ngx' and any(word in row.get('message', '').lower() for word in ('warn', 'error', 'fail')) or
                       kind == 'sdk.fsr' or 'error' in kind or 'failure' in kind or 'failed' in kind)
            if warning:
                data['warnings'] += 1
                warnings.append({k: row.get(k) for k in ('seq', 'qpc', 'generation', 'provider', 'kind', 'a', 'message')})
    def distribution(samples):
        values = sorted(samples)
        return dict(count=len(values), mean_ms=statistics.fmean(values) if values else 0,
                    p99_ms=values[min(len(values)-1, int(len(values)*.99))] if values else 0,
                    maximum_ms=max(values, default=0), over_25_ms=sum(x >= 25 for x in values),
                    over_100_ms=sum(x >= 100 for x in values))
    for data in generations.values():
        for key in ('presents', 'sleeps', 'intervals'):
            data[key] = distribution(data[key])
    return dict(recording=str(path), qpc_frequency=frequency, events=events, dropped_records=lost,
                first_sequence=first_sequence, last_sequence=last_sequence,
                earlier_records_rotated_out=first_sequence is not None and first_sequence > 1,
                parse_errors=invalid, generations=generations, warnings=warnings,
                seconds=seconds,
                version=version, clean_shutdown=bool(events['session.end']),
                shutdown_marker_supported=version not in (None, '0.4.9'),
                phases={name: distribution(values) for name, values in phase_times.items()},
                unfinished_calls=list(pending.values()) + [row for stack in scope_pending.values() for row in stack],
                note='Long Present is not by itself a root cause. Compare warnings, input gaps, settings and handoff generations.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('recording', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(args.recording)
    encoded = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(encoded + '\n', encoding='utf-8')
        print(args.output)
    else:
        print(encoded)
    if (result['parse_errors'] or result['dropped_records'] or result['unfinished_calls'] or
        result['shutdown_marker_supported'] and not result['clean_shutdown']):
        raise SystemExit(2)
