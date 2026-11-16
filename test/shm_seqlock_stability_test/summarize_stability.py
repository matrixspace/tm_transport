#!/usr/bin/env python3
"""Summarize a run_stability.py summary.json into a compact result file.

Usage:
  summarize_stability.py SUMMARY.json OUT.json [--resources MONITOR_DIR]

--resources points at a resource_monitor.py output directory. The digest keeps
first-to-last growth for PIDs with at least --min-samples samples (long-lived
workers); short-lived restart-lane instances remain in resource_summary.json.
"""
import argparse
import json
from collections import defaultdict


def lane_summary(lane):
    writers = lane.get('writer_instances', [])
    readers = lane.get('reader_instances', [])

    def total(items, key):
        return sum((item.get('stats') or {}).get(key, 0) for item in items)

    sent = total(writers, 'sent')
    duration = lane.get('elapsed_seconds') or 0
    return {
        'name': lane['name'],
        'passed': lane['passed'],
        'violations': lane['violations'],
        'rate': lane['rate'],
        'payload': lane['payload'],
        'props': lane['props'],
        'writer_instances': len(writers),
        'reader_instances': len(readers),
        'sent': sent,
        'rejected': total(writers, 'rejected'),
        'received': total(readers, 'received'),
        'lost': total(readers, 'lost'),
        'corrupt': total(readers, 'corrupt'),
        'bad': total(readers, 'bad'),
        'dup': total(readers, 'dup'),
        'ooo': total(readers, 'ooo'),
        'runs': total(readers, 'runs'),
        'sent_per_second': sent / duration if duration else 0,
        'reader_p99_ns_max': max(((r.get('stats') or {}).get('p99ns', 0) for r in readers), default=0),
        'reader_max_ns_max': max(((r.get('stats') or {}).get('maxns', 0) for r in readers), default=0),
        'restarts': lane['restarts'],
        'resource_max': lane.get('resource_max'),
    }


def resource_digest(monitor_dir, min_samples):
    by_pid = defaultdict(list)
    with open(monitor_dir + '/resources.jsonl') as f:
        for line in f:
            record = json.loads(line)
            by_pid[record['pid']].append(record)
    long_lived = []
    for pid, rows in by_pid.items():
        if len(rows) < min_samples:
            continue
        rows.sort(key=lambda x: x['mono'])
        first, last = rows[0], rows[-1]
        long_lived.append({
            'pid': pid, 'lane': first['lane'], 'role': first['role'],
            'reader_index': first['reader_index'], 'samples': len(rows),
            'first_exp': first['exp'], 'last_exp': last['exp'],
            'delta_rss_kib': last['rss_kib'] - first['rss_kib'],
            'delta_pss_kib': last.get('pss_kib', 0) - first.get('pss_kib', 0),
            'delta_fds': last['fds'] - first['fds'],
            'delta_threads': last['threads'] - first['threads'],
            'delta_maps': last['maps'] - first['maps'],
            'rss_kib_first': first['rss_kib'], 'rss_kib_last': last['rss_kib'],
            'pss_kib_first': first.get('pss_kib'), 'pss_kib_last': last.get('pss_kib'),
            'cpu_percent_max': max((r.get('cpu_percent') or 0) for r in rows),
            'minflt_last': last.get('minflt'), 'majflt_last': last.get('majflt'),
            'rchar_last': last.get('rchar'), 'wchar_last': last.get('wchar'),
        })
    return {
        'samples_total': sum(len(rows) for rows in by_pid.values()),
        'pids_total': len(by_pid),
        'long_lived_pids': len(long_lived),
        'min_samples': min_samples,
        'max_delta_rss_kib': max((x['delta_rss_kib'] for x in long_lived), default=0),
        'max_delta_pss_kib': max((x['delta_pss_kib'] for x in long_lived), default=0),
        'max_delta_fds': max((x['delta_fds'] for x in long_lived), default=0),
        'max_delta_threads': max((x['delta_threads'] for x in long_lived), default=0),
        'max_delta_maps': max((x['delta_maps'] for x in long_lived), default=0),
        'instances': long_lived,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('summary')
    parser.add_argument('out')
    parser.add_argument('--resources')
    parser.add_argument('--min-samples', type=int, default=1000)
    args = parser.parse_args()
    with open(args.summary) as f:
        summary = json.load(f)
    lanes = [lane_summary(lane) for lane in summary['lanes']]
    totals = defaultdict(int)
    for lane in lanes:
        for key in ('sent', 'rejected', 'received', 'lost', 'corrupt', 'bad', 'dup', 'ooo', 'runs',
                    'writer_instances', 'reader_instances'):
            totals[key] += lane[key]
    out = {
        'duration_seconds': summary['duration_seconds'],
        'duration_hours': summary['duration_seconds'] / 3600,
        'passed': summary['passed'],
        'totals': dict(totals),
        'lanes': lanes,
        'environment': summary.get('environment', {}),
    }
    if args.resources:
        out['resources'] = resource_digest(args.resources, args.min_samples)
    with open(args.out, 'w') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print(f"wrote {args.out}: {len(lanes)} lanes, passed={summary['passed']}")


if __name__ == '__main__':
    main()
