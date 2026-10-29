import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('recording', Path(__file__).parents[1] / 'tools/analyze_fg_recording.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RecordingTests(unittest.TestCase):
    def test_gap_outside_present_and_missing_shutdown_remain_visible(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'session.jsonl'
            rows = [dict(kind='session', version='0.4.10', qpc_frequency=1000),
                    dict(kind='present.begin', qpc=1000, tid=1, generation=1),
                    dict(kind='present.end', qpc=1001, tid=1, generation=1, a=1),
                    dict(kind='scope.begin', qpc=1002, tid=1, message='native.framework'),
                    dict(kind='scope.end', qpc=1120, tid=1, message='native.framework', a=118),
                    dict(kind='present.begin', qpc=1121, tid=1, generation=1),
                    dict(kind='present.end', qpc=1122, tid=1, generation=1, a=1),
                    dict(kind='scope.begin', qpc=1123, tid=1, message='shutdown.output')]
            path.write_text('\n'.join(map(json.dumps, rows)), encoding='utf-8')
            result = module.analyze(path)
            self.assertEqual(result['generations'][1]['presents']['maximum_ms'], 1)
            self.assertEqual(result['generations'][1]['intervals']['maximum_ms'], 121)
            self.assertEqual(result['phases']['native.framework']['maximum_ms'], 118)
            self.assertEqual(result['unfinished_calls'][0]['message'], 'shutdown.output')
            self.assertFalse(result['clean_shutdown'])
            self.assertTrue(result['shutdown_marker_supported'])
            rows += [dict(kind='scope.end', qpc=1125, tid=1, message='shutdown.output', a=2),
                     dict(kind='session.end')]
            path.write_text('\n'.join(map(json.dumps, rows)), encoding='utf-8')
            result = module.analyze(path)
            self.assertEqual(result['unfinished_calls'], [])
            self.assertTrue(result['clean_shutdown'])

    def test_vendor_warning_survives_normal_status_and_rotation(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'session.jsonl'
            before = [dict(kind='session', qpc_frequency=1000),
                      dict(kind='sdk.streamline', seq=10, qpc=1000, tid=1, a=1,
                           message='Frame rate over 100.00ms, reseting frame timer', generation=2, provider=1),
                      dict(kind='present.begin', seq=11, qpc=1001, tid=1, generation=2, provider=1)]
            after = [dict(kind='status', seq=12, qpc=1002, message='healthy', generation=2, provider=1),
                     dict(kind='present.end', seq=13, qpc=1101, tid=1, a=100, d=0, generation=2, provider=1)]
            Path(str(path) + '.1').write_text('\n'.join(map(json.dumps, before)), encoding='utf-8')
            path.write_text('\n'.join(map(json.dumps, after)), encoding='utf-8')
            result = module.analyze(path)
            self.assertEqual(len(result['warnings']), 1)
            self.assertEqual(result['generations'][2]['presents']['over_100_ms'], 1)
            self.assertEqual(result['unfinished_calls'], [])
            self.assertTrue(result['earlier_records_rotated_out'])

    def test_sparse_phase_is_labelled_as_slow_samples_only(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'session.jsonl'
            path.write_text(json.dumps(dict(kind='phase.slow', tid=1, a=62,
                                           message='latency.render_begin', generation=1, provider=1)), encoding='utf-8')
            result = module.analyze(path)
            self.assertEqual(result['phases']['slow_only.latency.render_begin']['maximum_ms'], 62)
            self.assertEqual(result['unfinished_calls'], [])

    def test_incomplete_corrupt_and_dropped_records_are_not_clean(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'session.jsonl'
            path.write_bytes(b'{"kind":"records_dropped","count":7}\n\xff\n'
                             b'{"kind":"sleep.begin","tid":9,"seq":2}\n'
                             b'{"kind":"handoff.begin","tid":8,"seq":3}\n')
            result = module.analyze(path)
            self.assertEqual(result['dropped_records'], 7)
            self.assertEqual(len(result['parse_errors']), 1)
            self.assertEqual(len(result['unfinished_calls']), 2)


if __name__ == '__main__':
    unittest.main()
