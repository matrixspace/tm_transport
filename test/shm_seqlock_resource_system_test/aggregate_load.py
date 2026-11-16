#!/usr/bin/env python3
"""Aggregate the saturation batch and the 50%/90% load runs into one JSON.

Usage:
  aggregate_load.py OUT.json --saturation DIR --load LABEL=DIR [--load LABEL=DIR ...]

The saturation DIR is a run_tests.py --profile=full --rate=0 output. Each load
DIR is a run_tests.py run whose perf options carry the calibrated rate.
Percentiles are per-round means, not pooled samples; sample counts are kept in
the raw summary.json files and are not re-aggregated here.
"""
import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path


def load(path):
    with open(path) as f:
        return json.load(f)


def mean(values):
    return sum(values) / len(values)


def perf_rows(summary):
    for result in summary['results']:
        options = result['options']
        if options.get('mode') != 'perf':
            continue
        yield options, result['rows'][-1]


def summarize(rows):
    rates = [r['offered_per_second'] for r in rows]
    offered = sum(r['offered'] for r in rows)
    metrics = []
    for i in range(len(rows[0]['readers'])):
        latency = [r['readers'][i]['latency'] for r in rows]
        freshness = [r['readers'][i]['freshness'] for r in rows]
        lost = sum(r['readers'][i]['lost'] for r in rows)
        metrics.append({
            'received_total': sum(r['readers'][i]['received'] for r in rows),
            'lost_total': lost,
            'bad_total': sum(r['readers'][i]['bad'] for r in rows),
            'order_errors_total': sum(r['readers'][i]['order_errors'] for r in rows),
            'loss_rate': lost / max(1, offered),
            'latency_p50_ns_avg': mean([x['p50_ns'] for x in latency]),
            'latency_p99_ns_avg': mean([x['p99_ns'] for x in latency]),
            'latency_p99_ns_max': max(x['p99_ns'] for x in latency),
            'latency_max_ns_max': max(x['max_ns'] for x in latency),
            'freshness_p99_ns_avg': mean([x['p99_ns'] for x in freshness]),
        })
    return {
        'rounds': len(rows),
        'status_counts': dict(Counter(r['status'] for r in rows)),
        'offered_per_second_min': min(rates),
        'offered_per_second_avg': mean(rates),
        'offered_per_second_max': max(rates),
        'offered_total': offered,
        'accepted_total': sum(r['accepted'] for r in rows),
        'rejected_total': sum(r['rejected'] for r in rows),
        'process_cpu_percent_avg': mean([r['process_cpu_percent'] for r in rows]),
        'reader_metrics': metrics,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('out', type=Path)
    parser.add_argument('--saturation', type=Path, required=True)
    parser.add_argument('--load', action='append', default=[],
                        help='LABEL=DIR, one per 50%%/90%% run')
    args = parser.parse_args()

    sat_summary = load(args.saturation / 'summary.json')
    environment = load(args.saturation / 'environment.json')
    groups = defaultdict(list)
    for options, row in perf_rows(sat_summary):
        groups[(options['readers'], options['wait'])].append(row)
    saturation = []
    for key in sorted(groups):
        entry = {'readers': key[0], 'wait': key[1], 'rate': 0}
        entry.update(summarize(groups[key]))
        saturation.append(entry)

    loads = []
    for item in args.load:
        label, _, directory = item.partition('=')
        if not directory:
            parser.error(f'invalid --load (expected LABEL=DIR): {item}')
        path = Path(directory)
        summary = load(path / 'summary.json')
        rows = [row for _, row in perf_rows(summary)]
        options = next(options for options, _ in perf_rows(summary))
        entry = {'label': label, 'readers': options['readers'], 'wait': options['wait'],
                 'rate_requested': options['rate']}
        entry.update(summarize(rows))
        loads.append(entry)

    out = {
        'date': '2026-11-13',
        'scope': 'saturation and 50%/90% no-loss load measurements',
        'environment': {
            'uname': environment['uname'], 'cpu': environment['cpu'],
            'commit': environment['commit'], 'dirty_status': environment['dirty_status'],
            'binary_sha256': environment['binary_sha256'],
            'binary_environment': environment['binary_environment'],
        },
        'saturation': saturation,
        'load_50_90': loads,
        'method': 'Saturation is --rate=0. The 50%/90% rates are percentages of the minimum '
                  'offered/s observed across 5 saturation rounds for the same config; each load '
                  'run is 5 rounds x (5s warmup + 30s measurement).',
        'limits': [
            'The writer reaches its own throughput ceiling before readers lose messages in these configs, so saturation itself has no loss.',
            '10k/s staged baseline and these load runs use mean-of-per-round percentiles, not pooled samples.',
            'No p99.9; 300,000 samples per reader per round.',
            'The host is shared; affinity was pinned to NUMA0 physical cores but not reserved.',
        ],
    }
    with open(args.out, 'w') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print(f'wrote {args.out}: saturation {len(saturation)} configs, loads {len(loads)}')


if __name__ == '__main__':
    main()
