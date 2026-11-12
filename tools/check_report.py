#!/usr/bin/env python3
"""Independently check graphics-xps JSON accounting; standard library only."""

import argparse
import json
import math
from pathlib import Path


TYPES = {
    "fp16": (2, 2), "fp32": (4, 4), "fp64": (8, 8),
    "int8": (1, 4), "int16": (2, 4), "int32": (4, 4), "int64": (8, 8),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(report):
    require(report["schema_version"] == 1, "Unsupported schema")
    require(report["status"] == "completed", f"Run status: {report['status']}")
    config, device = report["config"], report["device"]
    require(config["precision_selection"] in ("auto", "explicit"), "Unknown precision selection")
    budget = config["memory_budget_bytes"]
    require(budget > 0 and report["definitions"]["serial"], "Invalid budget/scheduling mode")
    requested = [name for index, name in enumerate(TYPES) if config["precision_mask"] & (1 << index)]
    require([r["precision"] for r in report["results"]] == requested, "Missing/duplicate/out-of-order precision")
    passed = 0
    for result in report["results"]:
        name = result["precision"]
        require(result["declared_supported"] == device["capabilities"][name]["declared"], f"{name}: declaration mismatch")
        if result["status"] == "unsupported":
            require(not result["declared_supported"] and result["operations"] == 0, f"{name}: incorrect skip")
            require(result["peak_gpu_bytes"] == 0 and not result["compiled"], f"{name}: unsupported type executed")
            continue
        require(result["status"] == "passed", f"{name}: {result['status']}: {result['reason']}")
        require(result["compiled"] and result["self_test_passed"] and result["verified"], f"{name}: verification missing")
        require(0 < result["self_test_n"] <= 32, f"{name}: invalid self-test size")
        require(result["self_test_cells"] == result["self_test_n"] ** 2, f"{name}: partial self-test")
        n = result["matrix_n"]
        input_bytes, output_bytes = TYPES[name]
        require(result["a_bytes"] == n * n * input_bytes, f"{name}: A bytes incorrect")
        require(result["b_bytes"] == result["a_bytes"], f"{name}: B bytes incorrect")
        require(result["c_bytes"] == n * n * output_bytes, f"{name}: C bytes incorrect")
        require(result["gpu_bytes"] == result["a_bytes"] + result["b_bytes"] + result["c_bytes"], f"{name}: total bytes incorrect")
        require(result["gpu_bytes"] <= result["peak_gpu_bytes"] <= budget, f"{name}: memory budget exceeded")
        if "effective_budget_bytes" in result:
            require(result["gpu_bytes"] <= result["effective_budget_bytes"] <= budget, f"{name}: invalid effective budget")
        if "max_allocation_bytes" in device:
            require(max(result["a_bytes"], result["b_bytes"], result["c_bytes"]) <= device["max_allocation_bytes"], f"{name}: device allocation limit exceeded")
        require(result["requested_seconds"] == config["seconds"][name], f"{name}: duration override mismatch")
        if config["mode"] == "bench":
            require(result["wall_seconds"] >= result["requested_seconds"], f"{name}: test stopped too early")
            require(0 < result["gpu_seconds"] <= result["wall_seconds"] * 1.01, f"{name}: GPU time invalid")
            require(result["cpu_seconds"] >= 0, f"{name}: CPU time invalid")
            require(result["batches"] > 0 and 0 < result["batch_rows"] <= n, f"{name}: no valid batches")
            stride = 2 * n * n
            require(result["operations"] > 0 and result["operations"] % stride == 0, f"{name}: invalid operation count")
            rows = result["operations"] // stride
            require(result["touched_rows"] == min(n, rows), f"{name}: touched rows mismatch")
            require(result["verified_cells"] == (result["touched_rows"] * n if n <= 64 else 64), f"{name}: verification count mismatch")
            for metric, denominator in (("gpu_gops", "gpu_seconds"), ("wall_gops", "wall_seconds")):
                expected = result["operations"] / result[denominator] / 1e9
                require(math.isfinite(result[metric]) and math.isclose(result[metric], expected, rel_tol=1e-8), f"{name}: {metric} incorrect")
        else:
            require(result["operations"] == 0 and result["verified_cells"] == n * n, f"{name}: probe accounting incorrect")
            require(result["gpu_gops"] is None and result["wall_gops"] is None, f"{name}: probe presented as performance test")
        passed += 1
    require(passed > 0, "No precision passed")
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", type=Path, nargs="+")
    args = parser.parse_args()
    for path in args.reports:
        report = json.loads(path.read_text())
        passed = validate(report)
        print(f"PASS {path}: {passed} usable precisions, byte/time/operation/verification accounting consistent")


if __name__ == "__main__":
    main()
