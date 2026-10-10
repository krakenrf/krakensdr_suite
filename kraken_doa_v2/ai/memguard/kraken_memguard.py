"""Memory ceiling for the AI Signal Lab's Python processes.

A Python process that needs more memory than the Pi has free does not just
fail: Linux first pushes the receiver (heimdall, kraken_doa) and everything
else into swap, the whole Pi crawls or freezes, and the out-of-memory killer
may take minutes to step in. With this ceiling such a process gets a
MemoryError instead and the receiver keeps running.

apply() caps the process's data memory (RLIMIT_DATA: heap and anonymous
mappings - every numpy array) at what is available now minus a reserve for
the receiver. sigtool.py calls it itself; kraken_ai.py puts this folder on
PYTHONPATH for the agent, so sitecustomize.py applies it to every Python
script the agent runs.
"""
import os

RESERVE_MB = 400        # left for the receiver's own growth and the system
MIN_MB = 256            # never cap a process below this


def _meminfo_kb(key):
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith(key + ":"):
                return int(line.split()[1])
    return None


def _vmdata_kb():
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("VmData:"):
                return int(line.split()[1])
    return 0


def apply():
    """Returns the limit in MB, or None when none was set."""
    if os.environ.get("KRAKEN_AI_MEM_LIMIT", "1") == "0":
        return None
    try:
        import resource
        avail = _meminfo_kb("MemAvailable")
        if avail is None:
            return None
        total = _meminfo_kb("MemTotal") or 0
        reserve = max(RESERVE_MB * 1024, total // 10)
        allow_kb = max(MIN_MB * 1024, avail - reserve)
        limit = (allow_kb + _vmdata_kb()) * 1024
        soft, hard = resource.getrlimit(resource.RLIMIT_DATA)
        if hard != resource.RLIM_INFINITY:
            limit = min(limit, hard)
        if soft != resource.RLIM_INFINITY and soft <= limit:
            return soft // (1 << 20)        # already tighter (a parent's ceiling)
        resource.setrlimit(resource.RLIMIT_DATA, (limit, hard))
        return limit // (1 << 20)
    except (OSError, ValueError, ImportError):
        return None


def limit_mb():
    """The ceiling in force (MB), or None."""
    try:
        import resource
        soft = resource.getrlimit(resource.RLIMIT_DATA)[0]
        return None if soft == resource.RLIM_INFINITY else soft // (1 << 20)
    except (OSError, ImportError):
        return None
