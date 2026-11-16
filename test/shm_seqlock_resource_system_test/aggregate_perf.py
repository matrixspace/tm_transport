#!/usr/bin/env python3
"""Aggregate several run_tests.py --profile=full batches into one JSON baseline.

Usage:
  aggregate_perf.py OUT.json label=DIR [label=DIR ...]

Each DIR must contain summary.json and environment.json from
run_tests.py --profile=full. Percentiles are aggregated as the mean of the
per-round percentiles (not a pooled-sample percentile); per-reader sample
counts are kept in the raw summary.json files.
"""
import json
import statistics
import sys
from collections import Counter
from pathlib import Path


def load(path):
    with open(path) as f:
        return json.load(f)


def mean(values):
    return sum(values) / len(values)


def aggregate(rows):
    pub_mean = [r['publish']['mean_ns'] for r in rows]
    pub_p50 = [r['publish']['p50_ns'] for r in rows]
    pub_p99 = [r['publish']['p99_ns'] for r in rows]
    pub_max = [r['publish']['max_ns'] for r in rows]
    cpu = [r['process_cpu_percent'] for r in rows]
    readers = []
    for i in range(len(rows[0]['readers'])):
        latency = [r['readers'][i]['latency'] for r in rows]
        freshness = [r['readers'][i]['freshness'] for r in rows]
        lost = sum(r['readers'][i]['lost'] for r in rows)
        offered = sum(r['offered'] for r in rows)
        readers.append({
            'received_total': sum(r['readers'][i]['received'] for r in rows),
            'lost_total': lost,
            'bad_total': sum(r['readers'][i]['bad'] for r in rows),
            'order_errors_total': sum(r['readers'][i]['order_errors'] for r in rows),
            'loss_rate': lost / max(1, offered),
            'latency_p50_ns_avg': mean([x['p50_ns'] for x in latency]),
            'latency_p90_ns_avg': mean([x['p90_ns'] for x in latency]),
            'latency_p99_ns_avg': mean([x['p99_ns'] for x in latency]),
            'latency_p99_ns_max': max(x['p99_ns'] for x in latency),
            'latency_max_ns_max': max(x['max_ns'] for x in latency),
            'freshness_p99_ns_avg': mean([x['p99_ns'] for x in freshness]),
            'freshness_max_ns_max': max(x['max_ns'] for x in freshness),
        })
    before = rows[0]['resource_before']
    after = rows[-1]['resource_after']
    return {
        'rounds': len(rows),
        'status_counts': dict(Counter(r['status'] for r in rows)),
        'publish_mean_ns_avg': mean(pub_mean),
        'publish_mean_ns_stdev': statistics.pstdev(pub_mean),
        'publish_mean_ns_min': min(pub_mean),
        'publish_mean_ns_max': max(pub_mean),
        'publish_p50_ns_avg': mean(pub_p50),
        'publish_p99_ns_avg': mean(pub_p99),
        'publish_p99_ns_max': max(pub_p99),
        'publish_max_ns_max': max(pub_max),
        'process_cpu_percent_avg': mean(cpu),
        'offered_total': sum(r['offered'] for r in rows),
        'accepted_total': sum(r['accepted'] for r in rows),
        'rejected_total': sum(r['rejected'] for r in rows),
        'resource': {
            key: [before[name], after[name]]
            for key, name in (
                ('rss_kib', 'rss_kib'), ('pss_kib', 'pss_kib'), ('fd', 'fd'),
                ('threads', 'threads'), ('maps', 'maps'), ('segment_maps', 'segment_maps'))
        },
        'reader_metrics': readers,
    }


def main(out_path, batches):
    configs = {}
    environments = []
    for label, path in batches:
        directory = Path(path)
        summary = load(directory / 'summary.json')
        environment = load(directory / 'environment.json')
        environments.append((label, directory, summary, environment))
        for result in summary['results']:
            options = result['options']
            if options.get('mode') != 'perf':
                continue
            key = (options['readers'], options['payload'], options['layout'],
                   options['read'], options['wait'])
            configs.setdefault(key, []).append(result['rows'][-1])
    rows = []
    for key in sorted(configs):
        entry = {'readers': key[0], 'payload': key[1], 'layout': key[2],
                 'read': key[3], 'wait': key[4]}
        entry.update(aggregate(configs[key]))
        rows.append(entry)
    first = environments[0][3]
    out = {
        'date': '2026-11-13',
        'scope': 'staged release performance baseline',
        'full_plan_matrix': False,
        'note': 'Covers the requested staged dimensions only; not the full 1200-config '
                'default matrix, no calibrated 50%/90% load and no soak.',
        'environment': {
            'uname': first['uname'],
            'cpu': first['cpu'],
            'commit': first['commit'],
            'dirty_status': first['dirty_status'],
            'binary': first['binary'],
            'binary_sha256': first['binary_sha256'],
            'binary_environment': first['binary_environment'],
            'batches': [
                {'label': label, 'path': str(path), 'shortened': summary['shortened'],
                 'full_dimensions': summary['full_dimensions']}
                for label, path, summary, _ in environments
            ],
        },
        'configs': rows,
        'limits': [
            'No calibrated 50%/90% no-loss load; the offered rate is a selected rate.',
            'Percentiles are the mean of per-round percentiles, not pooled samples.',
            'p99.9 is not reported; 300,000 samples per reader per round.',
            'Affinity was pinned, but the host was not reserved exclusively.',
            'Frequency governor/current frequency sysfs entries are unavailable.',
            'Slow-reader writer cost and futex idle CPU come from shm_seqlock_test, not this file.',
            '2h/24h soak, saturation and graph/MultiTransport performance are separate runs.',
        ],
    }
    with open(out_path, 'w') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print(f'wrote {out_path} with {len(rows)} configs')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit('usage: aggregate_perf.py OUT.json label=DIR [label=DIR ...]')
    batches = []
    for item in sys.argv[2:]:
        label, _, path = item.partition('=')
        if not path:
            sys.exit(f'invalid batch (expected label=DIR): {item}')
        batches.append((label, path))
    main(sys.argv[1], batches)
