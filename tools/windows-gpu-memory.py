"""Sample Windows driver memory counters; maxima are bounded by the sample interval.

Adapter counters include other processes. Process counters are supplementary:
Microsoft documents inaccurate GPU Process Memory values (KB4490156).
https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw
https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/gpu-process-memory-counters-report-wrong-value
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import platform
import time


class Value(c.Union):
    _fields_ = [('large', c.c_int64), ('double', c.c_double)]


class Formatted(c.Structure):
    _fields_ = [('status', w.DWORD), ('value', Value)]


class Item(c.Structure):
    _fields_ = [('name', w.LPWSTR), ('formatted', Formatted)]


class DriverMemory:
    def __init__(self):
        self.api = c.WinDLL('pdh')
        self.api.PdhOpenQueryW.argtypes = [w.LPCWSTR, c.c_size_t, c.POINTER(w.HANDLE)]
        self.api.PdhAddEnglishCounterW.argtypes = [w.HANDLE, w.LPCWSTR, c.c_size_t, c.POINTER(w.HANDLE)]
        self.api.PdhCollectQueryData.argtypes = [w.HANDLE]
        self.api.PdhGetFormattedCounterArrayW.argtypes = [w.HANDLE, w.DWORD, c.POINTER(w.DWORD), c.POINTER(w.DWORD), c.c_void_p]
        self.api.PdhCloseQuery.argtypes = [w.HANDLE]
        for name in ('PdhOpenQueryW', 'PdhAddEnglishCounterW', 'PdhCollectQueryData', 'PdhGetFormattedCounterArrayW', 'PdhCloseQuery'):
            getattr(self.api, name).restype = w.DWORD
        self.query = w.HANDLE()
        self.check(self.api.PdhOpenQueryW(None, 0, c.byref(self.query)))
        self.counters = {}
        try:
            for scope in ('Adapter', 'Process'):
                for kind in ('Dedicated', 'Shared', 'Total Committed'):
                    label = kind + (' Usage' if kind != 'Total Committed' else '')
                    handle = w.HANDLE()
                    self.check(self.api.PdhAddEnglishCounterW(self.query, f'\\GPU {scope} Memory(*)\\{label}', 0, c.byref(handle)))
                    self.counters[f'{scope}/{kind}'] = handle
            self.check(self.api.PdhCollectQueryData(self.query))
        except Exception:
            self.close()
            raise

    @staticmethod
    def check(status):
        if status:
            raise OSError(f'PDH status 0x{status:08x}')

    def sample(self, pid):
        self.check(self.api.PdhCollectQueryData(self.query))
        result = {}
        for key, handle in self.counters.items():
            size, count = w.DWORD(), w.DWORD()
            status = self.api.PdhGetFormattedCounterArrayW(handle, 0x400, c.byref(size), c.byref(count), None)
            if not size.value:
                if status not in (0, 0x800007d5):
                    self.check(status)
                result[key] = {}
                continue
            if status not in (0, 0x800007d2):
                self.check(status)
            # Wildcard instances may change between the two API calls. Retry
            # from a fresh size query rather than using an invalid returned size.
            for _ in range(3):
                buffer = c.create_string_buffer(size.value)
                status = self.api.PdhGetFormattedCounterArrayW(handle, 0x400, c.byref(size), c.byref(count), buffer)
                if status != 0x800007d2:
                    break
                size = w.DWORD()
                self.api.PdhGetFormattedCounterArrayW(handle, 0x400, c.byref(size), c.byref(count), None)
            self.check(status)
            items = c.cast(buffer, c.POINTER(Item))
            result[key] = {items[i].name: int(items[i].formatted.value.large)
                           for i in range(count.value)
                           if items[i].formatted.status in (0, 1)
                           and (key.startswith('Adapter/') or items[i].name.startswith(f'pid_{pid}_'))}
        return result

    def close(self):
        if self.query:
            self.api.PdhCloseQuery(self.query)
            self.query = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interval-ms', type=int, default=20)
    parser.add_argument('--seconds', type=float, default=600)
    args = parser.parse_args()
    if args.output.exists() or not 10 <= args.interval_ms <= 1000 or args.pid <= 0:
        parser.error('Use a fresh output, positive PID and a 10..1000 ms interval.')
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    kernel.WaitForSingleObject.restype = w.DWORD
    kernel.CloseHandle.argtypes = [w.HANDLE]
    process = kernel.OpenProcess(0x100000, False, args.pid)
    if not process:
        raise c.WinError(c.get_last_error())
    memory, samples, peaks = None, [], {}
    report = {'passed': False, 'pid': args.pid, 'windows': platform.version(),
              'requestedIntervalMs': args.interval_ms,
              'peakScope': 'maximum observed at sample times; adapter totals include other processes; process values are supplementary',
              'instantaneousPeakProven': False,
              'processCounterKnownIssue': 'Microsoft KB4490156',
              'monitorSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    start = time.perf_counter()
    try:
        memory = DriverMemory()
        while time.perf_counter() - start < args.seconds:
            values = memory.sample(args.pid)
            samples.append({'elapsedMs': (time.perf_counter() - start) * 1000, 'wallUnixNs': time.time_ns(), 'bytes': values})
            for key, instances in values.items():
                for name, value in instances.items():
                    peaks.setdefault(key, {})[name] = max(peaks.get(key, {}).get(name, 0), value)
            status = kernel.WaitForSingleObject(process, args.interval_ms)
            if status == 0:
                report['processExited'] = True
                break
            if status != 0x102:
                raise OSError(f'WaitForSingleObject: 0x{status:x}')
        report['passed'] = bool(samples and peaks.get('Adapter/Dedicated') and report.get('processExited'))
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        if memory:
            memory.close()
        kernel.CloseHandle(process)
        intervals = [b['elapsedMs'] - a['elapsedMs'] for a, b in zip(samples, samples[1:])]
        report.update(samples=samples, observedPeaksBytes=peaks, sampleCount=len(samples),
                      maximumObservedIntervalMs=max(intervals, default=0))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    if not report['passed']:
        raise RuntimeError('Driver memory measurement did not complete.')
    print(json.dumps({key: value for key, value in report.items() if key != 'samples'}))


if __name__ == '__main__':
    main()
