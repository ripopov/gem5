#!/usr/bin/env python3
"""Validate completed gem5-kmd study runs and report active-time metrics."""
import configparser
import json
import sys
from pathlib import Path


def summarize(path):
    path = Path(path)
    status = dict(
        line.split("=", 1)
        for line in (path / "exit-status.txt").read_text().splitlines()
    )
    assert status == {
        "application": "0",
        "gem5": "0",
        "timed_out": "false",
    }, status
    output = (path / "app.stdout").read_text()
    assert "PASS" in output, output
    log = (path / "gem5.log").read_text()
    assert "rocjitsu KMD state drained" in log
    assert not any(
        error in log
        for error in (
            "fatal:",
            "panic:",
            "KMD reactor failed",
            "dropped stale doorbell",
        )
    )
    stats = {}
    for line in (path / "stats.txt").read_text().splitlines():
        fields = line.split()
        if len(fields) > 1 and not line.startswith(("-", "#")):
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                pass  # Histogram tables also contain non-numeric rows.
    hsa = "system.cpu1.gpu_cmd_proc.hsapp."
    submitted = stats[hsa + "aqlPacketsSubmitted"]
    assert submitted > 0 and submitted == stats[hsa + "aqlPacketsRetired"]
    config = configparser.ConfigParser()
    config.read(path / "config.ini")
    cus = sum(config[s].get("type") == "ComputeUnit" for s in config)
    clock = config[config["system.Shader"]["clk_domain"]]["clock"]
    ticks = stats["system.cpu1.shaderActiveTicks"]
    seconds = ticks / stats["simFreq"]
    result = {
        "run": path.name,
        "modeled_cus": cus,
        "host_seconds": stats["hostSeconds"],
        "shader_active_ticks": ticks,
        "gpu_active_cycles": ticks / int(clock),
        "gpu_active_ms": seconds * 1000,
        "kernels": stats[
            "system.cpu1.gpu_cmd_proc.dispatcher.numKernelLaunched"
        ],
        "aql_submitted_and_retired": submitted,
        "output": output.strip().splitlines(),
        "completed_wavefronts_per_cu": [
            stats[f"system.cpu1.CUs{i}.completedWfs"] for i in range(cus)
        ],
    }
    if "input_bytes=" in output:
        size = int(output.split("input_bytes=", 1)[1].split()[0])
        result["useful_input_GB_per_second"] = size / seconds / 1e9
    # DRAM counters cover the whole run, including setup and copies. They must
    # not be divided by GPU-active time and called kernel-only HBM bandwidth.
    result["whole_run_dram"] = {
        key: value
        for key, value in stats.items()
        if key.startswith("system.gpu_mem_ctrls")
        and key.endswith(("dramBytesRead", "dramBytesWritten", "peakBW"))
    }
    return result


if __name__ == "__main__":
    print(json.dumps([summarize(path) for path in sys.argv[1:]], indent=2))
