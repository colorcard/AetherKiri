#!/usr/bin/env python3
"""Authorized, local-only visual compatibility runner for AetherKiri."""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
import os
import platform
import shutil
import statistics
import struct
import subprocess
import sys
import time
import zlib
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPAT = ROOT / "out" / "compat"
PROFILE_SCHEMA = "aetherkiri.visual-profile.v1"
REQUIRED = ("software", "gpu_bridge")
QUARANTINE = ("sdl3_gpu",)
LAYER_KEYS = ("parent", "order", "visible", "node_visible", "opacity",
              "type", "rect", "clip")


class CompatError(RuntimeError):
    pass


def load_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def ensure_ignored(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    probe = path / ".ignore-check"
    probe.touch()
    result = subprocess.run(["git", "check-ignore", "-q", str(probe)],
                            cwd=ROOT)
    probe.unlink()
    if result.returncode:
        raise CompatError(f"commercial artifact root is not Git-ignored: {path}")


def source_audit(root: Path) -> dict[str, tuple[int, int]]:
    audit = {}
    for path in root.rglob("*"):
        if path.is_file():
            stat = path.stat()
            audit[path.relative_to(root).as_posix()] = (stat.st_size, stat.st_mtime_ns)
    return audit


def resource_fingerprint(root: Path) -> tuple[str, list[dict]]:
    archives = []
    wanted = {"data", "patch", "data1080", "patch_data1080",
              "bgimage1080", "fgimage1080"}
    for path in sorted(root.iterdir(), key=lambda item: item.name.lower()):
        if not path.is_file() or path.suffix.lower() != ".xp3":
            continue
        if path.stem.lower() not in wanted and not any(
                token in path.stem.lower() for token in ("patch", "1080")):
            continue
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        archives.append({"name": path.name, "size": path.stat().st_size,
                         "sha256": digest.hexdigest()})
    if not archives:
        raise CompatError("no rendering-related XP3 archives found")
    packed = json.dumps(archives, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(packed).hexdigest(), archives


def make_shadow(source: Path, destination: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    writable = {"savedata", "save", "cache", "logs", "log"}
    for entry in source.iterdir():
        target = destination / entry.name
        if entry.name.lower() in writable:
            target.mkdir()
        elif entry.suffix.lower() in {".log", ".tmp"}:
            target.touch()
        else:
            target.symlink_to(entry.resolve(), target_is_directory=entry.is_dir())
    (destination / "savedata").mkdir(exist_ok=True)


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise CompatError(f"unsupported PPM: {path}")
    pos = 2
    tokens = []
    while len(tokens) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        tokens.append(int(data[pos:end]))
        pos = end
    while data[pos:pos + 1].isspace():
        pos += 1
    width, height, maximum = tokens
    if maximum != 255 or len(data) - pos != width * height * 3:
        raise CompatError(f"invalid PPM payload: {path}")
    return width, height, data[pos:]


def write_png(ppm: Path, png: Path) -> None:
    width, height, rgb = read_ppm(ppm)
    scanlines = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3]
                         for y in range(height))
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))
    png.write_bytes(b"\x89PNG\r\n\x1a\n" +
                    chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                                8, 2, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(scanlines, 6)) +
                    chunk(b"IEND", b""))


def luminance(rgb: bytes) -> list[float]:
    return [0.2126 * rgb[i] + 0.7152 * rgb[i + 1] + 0.0722 * rgb[i + 2]
            for i in range(0, len(rgb), 3)]


def edge_mask(values: list[float], width: int, height: int) -> set[int]:
    edges = set()
    for y in range(1, height - 1):
        row = y * width
        for x in range(1, width - 1):
            index = row + x
            gx = abs(values[index + 1] - values[index - 1])
            gy = abs(values[index + width] - values[index - width])
            if gx + gy >= 32:
                edges.add(index)
    return edges


def dilate(edges: set[int], width: int, height: int) -> set[int]:
    result = set()
    for index in edges:
        y, x = divmod(index, width)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    result.add(ny * width + nx)
    return result


def largest_component(mask: set[int], width: int, height: int) -> int:
    largest = 0
    while mask:
        start = mask.pop()
        queue = deque([start])
        size = 0
        while queue:
            index = queue.popleft()
            size += 1
            y, x = divmod(index, width)
            for neighbor in (index - 1, index + 1, index - width, index + width):
                if neighbor in mask:
                    ny, nx = divmod(neighbor, width)
                    if abs(nx - x) + abs(ny - y) == 1:
                        mask.remove(neighbor)
                        queue.append(neighbor)
        largest = max(largest, size)
    return largest


def image_metrics(reference: Path, actual: Path) -> dict:
    rw, rh, ref = read_ppm(reference)
    aw, ah, got = read_ppm(actual)
    if (rw, rh) != (aw, ah):
        return {"pass": False, "reason": "dimension mismatch",
                "reference_size": [rw, rh], "actual_size": [aw, ah]}
    differences = [abs(a - b) for a, b in zip(ref, got)]
    sorted_diff = sorted(differences)
    mae = statistics.fmean(differences) if differences else 0
    p99 = sorted_diff[min(len(sorted_diff) - 1,
                          math.floor(len(sorted_diff) * .99))]
    ref_y, got_y = luminance(ref), luminance(got)
    mean_ref, mean_got = statistics.fmean(ref_y), statistics.fmean(got_y)
    var_ref = statistics.fmean((v - mean_ref) ** 2 for v in ref_y)
    var_got = statistics.fmean((v - mean_got) ** 2 for v in got_y)
    covariance = statistics.fmean((a - mean_ref) * (b - mean_got)
                                  for a, b in zip(ref_y, got_y))
    ssim = ((2 * mean_ref * mean_got + 6.5025) *
            (2 * covariance + 58.5225) /
            ((mean_ref ** 2 + mean_got ** 2 + 6.5025) *
             (var_ref + var_got + 58.5225)))
    ref_edges, got_edges = edge_mask(ref_y, rw, rh), edge_mask(got_y, rw, rh)
    if not ref_edges and not got_edges:
        edge_f1 = 1.0
    else:
        precision = len(got_edges & dilate(ref_edges, rw, rh)) / max(1, len(got_edges))
        recall = len(ref_edges & dilate(got_edges, rw, rh)) / max(1, len(ref_edges))
        edge_f1 = 2 * precision * recall / max(1e-12, precision + recall)
    changed = {i // 3 for i, value in enumerate(differences) if value > 64}
    component_ratio = largest_component(changed, rw, rh) / (rw * rh)
    return {"pass": mae <= 16 and p99 <= 64 and ssim >= .97 and
                    edge_f1 >= .985 and component_ratio <= .005,
            "rgb_mae": mae, "rgb_p99": p99, "luminance_ssim": ssim,
            "edge_f1": edge_f1,
            "largest_structural_difference_ratio": component_ratio,
            "absolute_error_pixels": sum(value != 0 for value in differences)}


def roi_metrics(reference: Path, actual: Path, roi: dict) -> dict:
    width, height, ref = read_ppm(reference)
    actual_width, actual_height, got = read_ppm(actual)
    if (width, height) != (actual_width, actual_height):
        return {"pass": False, "reason": "dimension mismatch"}
    x, y, roi_width, roi_height = (int(roi[key]) for key in ("x", "y", "width", "height"))
    if x < 0 or y < 0 or x + roi_width > width or y + roi_height > height:
        return {"pass": False, "reason": "ROI is outside the frame"}
    ref_values, got_values = [], []
    for row in range(y, y + roi_height):
        begin = (row * width + x) * 3
        end = begin + roi_width * 3
        ref_values.extend(ref[begin:end])
        got_values.extend(got[begin:end])
    mae = statistics.fmean(abs(a - b) for a, b in zip(ref_values, got_values))
    ref_luma, got_luma = luminance(bytes(ref_values)), luminance(bytes(got_values))
    ref_variance = statistics.pvariance(ref_luma) if len(ref_luma) > 1 else 0
    got_variance = statistics.pvariance(got_luma) if len(got_luma) > 1 else 0
    variance_ratio = min(ref_variance, got_variance) / max(1.0, ref_variance, got_variance)
    return {"pass": mae <= 16 and variance_ratio >= .5,
            "rgb_mae": mae, "luminance_variance_ratio": variance_ratio}


def normalized_layers(path: Path) -> dict[str, dict]:
    document = load_json(path)
    result = {}
    for layer in document.get("layers", []):
        image = layer.get("image", {})
        result[layer["path"]] = {key: layer.get(key) for key in LAYER_KEYS} | {
            "image": {key: image.get(key) for key in ("present", "width", "height")}}
    return result


def compare_checkpoint(reference: Path, actual: Path, exact: bool,
                       rois: list[dict] | None = None) -> dict:
    ppm_reference = reference.with_suffix(".ppm")
    ppm_actual = actual.with_suffix(".ppm")
    if not ppm_reference.exists() or not ppm_actual.exists():
        return {"pass": False, "reason": "checkpoint artifact missing"}
    layers_equal = normalized_layers(reference.with_suffix(".layers.json")) == \
                   normalized_layers(actual.with_suffix(".layers.json"))
    if exact:
        _, _, ref = read_ppm(ppm_reference)
        _, _, got = read_ppm(ppm_actual)
        metrics = {"pass": ref == got, "absolute_error_pixels":
                   sum(a != b for a, b in zip(ref, got))}
    else:
        metrics = image_metrics(ppm_reference, ppm_actual)
    metrics["layers_equal"] = layers_equal
    metrics["rois"] = {roi["name"]: roi_metrics(ppm_reference, ppm_actual, roi)
                       for roi in (rois or [])}
    metrics["pass"] = metrics["pass"] and layers_equal and all(
        result["pass"] for result in metrics["rois"].values())
    return metrics


def engine_path(configuration: str) -> Path:
    return ROOT / "out" / "linux" / configuration / "apps" / \
           "aetherkiri_engine" / "aetherkiri_engine"


def run_backend(profile_path: Path, shadow: Path, backend: str,
                configuration: str, run_root: Path, timeout: int,
                fast_exit: bool) -> dict:
    output = run_root / configuration / backend
    output.mkdir(parents=True, exist_ok=True)
    executable = engine_path(configuration)
    command = [str(executable), "--game", str(shadow.resolve()),
               "--render-backend", backend, "--fps", "60", "--scenario",
               str(profile_path), "--scenario-output", str(output)]
    if fast_exit:
        command.append("--scenario-fast-exit")
    environment = os.environ.copy()
    library = ROOT / "out" / "linux" / configuration / "vcpkg_installed" / \
              "x64-linux" / "lib"
    environment["LD_LIBRARY_PATH"] = str(library)
    started = time.monotonic()
    performance = None
    try:
        process = subprocess.run(command, cwd=ROOT, env=environment,
                                 text=True, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=timeout)
        log = process.stdout
        state = "passed" if process.returncode == 0 and \
                "scenario completed" in log else "failed"
        if backend != "software" and any(marker in log for marker in
                ("SDL_CreateGPUDevice failed", "SDL_CreateRenderer failed",
                 "GPU driver", "not supported")):
            state = "unavailable"
        report_path = output / "scenario-report.json"
        performance = load_json(report_path) if report_path.exists() else None
        if state == "passed" and configuration == "release":
            bad_log = any(marker.lower() in log.lower() for marker in
                          ("shader validation", "resource destruction error"))
            if (not performance or performance["average_fps"] < 55 or
                    performance["p99_frame_time_ms"] > 50 or
                    performance["max_frame_time_ms"] >= 3000 or bad_log):
                state = "failed"
    except subprocess.TimeoutExpired as exc:
        captured = exc.stdout or b""
        if isinstance(captured, bytes):
            captured = captured.decode("utf-8", errors="replace")
        log = captured + "\nrunner timeout"
        state = "timeout"
        process = None
    (output / "host.log").write_text(log, encoding="utf-8")
    return {"backend": backend, "configuration": configuration,
            "state": state, "elapsed_seconds": time.monotonic() - started,
            "returncode": None if process is None else process.returncode,
            "output": str(output), "performance": performance}


def reference_root(fingerprint: str, profile_name: str) -> Path:
    host = f"{sys.platform}-{platform.machine()}"
    return COMPAT / "references" / fingerprint / host / "1920x1080" / \
           PROFILE_SCHEMA / profile_name


def runtime_scenario(profile: dict, run_root: Path, quick: bool) -> Path:
    steps = profile.get("steps", [])
    if quick:
        steps = [step for step in steps
                 if step.get("action") != "performance_sample"]
    path = run_root / "scenario.runtime.json"
    path.write_text(json.dumps({"version": 1, "steps": steps},
                               ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")
    return path


def contact_sheet(source: Path, checkpoints: list[str], target: Path) -> None:
    rows = []
    for checkpoint in checkpoints:
        ppm = source / f"{checkpoint}.ppm"
        png = source / f"{checkpoint}.review.png"
        if ppm.exists():
            write_png(ppm, png)
            rows.append(f"<tr><th>{html.escape(checkpoint)}</th><td>"
                        f"<img src='{html.escape(png.name)}'></td></tr>")
    target.write_text("<!doctype html><meta charset=utf-8><title>Reference review</title>"
                      "<style>body{font:14px sans-serif;background:#202124;color:#eee}"
                      "table{border-collapse:collapse}td,th{border:1px solid #666;padding:8px}"
                      "img{max-width:960px;height:auto}</style><h1>Software reference review</h1>"
                      f"<table>{''.join(rows)}</table>", encoding="utf-8")


def write_summary(path: Path, summary: dict) -> None:
    path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")
    rows = "".join(f"<tr><td>{html.escape(run['configuration'])}</td>"
                   f"<td>{html.escape(run['backend'])}</td>"
                   f"<td>{html.escape(run['state'])}</td></tr>"
                   for run in summary["runs"])
    (path.parent / "summary.html").write_text(
        "<!doctype html><meta charset=utf-8><title>Visual compatibility</title>"
        "<style>body{font:14px sans-serif;max-width:1100px;margin:30px auto}"
        "td,th{padding:6px 12px;border:1px solid #aaa}table{border-collapse:collapse}"
        "pre{white-space:pre-wrap}</style><h1>Visual compatibility</h1>"
        f"<table><tr><th>Build</th><th>Backend</th><th>Status</th></tr>{rows}</table>"
        f"<pre>{html.escape(json.dumps(summary.get('comparisons', {}), indent=2))}</pre>",
        encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("profile")
    parser.add_argument("command", nargs="?", choices=("run", "approve-reference"),
                        default="run")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--confirm", action="store_true")
    parser.add_argument("--reviewed-run", type=Path)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    profile_path = ROOT / "tests" / "visual_profiles" / f"{args.profile}.json"
    profile = load_json(profile_path)
    if profile.get("schema") != PROFILE_SCHEMA:
        raise CompatError("unsupported visual profile schema")
    environment_name = profile.get("game_env")
    game_value = os.environ.get(environment_name, "")
    if not game_value:
        raise CompatError(f"set {environment_name} to the authorized game directory")
    source = Path(game_value).expanduser().resolve()
    if not source.is_dir() or not source.is_absolute():
        raise CompatError("game path must be an absolute directory")
    ensure_ignored(COMPAT)
    fingerprint, archives = resource_fingerprint(source)
    audit_before = source_audit(source)
    session = time.strftime("%Y%m%d-%H%M%S")
    run_root = COMPAT / "runs" / args.profile / fingerprint / session
    shadow = COMPAT / "worktrees" / args.profile / fingerprint
    make_shadow(source, shadow)
    (run_root / "resource-fingerprint.json").parent.mkdir(parents=True,
                                                           exist_ok=True)
    (run_root / "resource-fingerprint.json").write_text(
        json.dumps({"fingerprint": fingerprint, "archives": archives}, indent=2),
        encoding="utf-8")
    scenario_path = runtime_scenario(profile, run_root, args.quick)
    if not args.no_build:
        subprocess.run([str(ROOT / "tools" / "build_sdl_host.sh"), "linux", "debug"],
                       cwd=ROOT, check=True)
        if not args.quick:
            subprocess.run([str(ROOT / "tools" / "build_sdl_host.sh"), "linux", "release"],
                           cwd=ROOT, check=True)
    backends = list(REQUIRED) + ([] if args.quick else list(QUARANTINE))
    configurations = ["debug"] + ([] if args.quick else ["release"])
    runs = [run_backend(scenario_path, shadow, backend, configuration, run_root,
                        int(profile.get("timeout_seconds", 180)), args.quick)
            for configuration in configurations for backend in backends]
    fingerprint_after, _ = resource_fingerprint(source)
    if source_audit(source) != audit_before or fingerprint_after != fingerprint:
        raise CompatError("source game directory changed during compatibility run")
    checkpoints = profile.get("checkpoints", [])
    reference = reference_root(fingerprint, args.profile)
    software_output = run_root / "debug" / "software"
    if args.command == "approve-reference":
        review = run_root / "reference-review.html"
        contact_sheet(software_output, checkpoints, review)
        if not args.confirm:
            print(f"Review {review}, then approve this exact run with:\n"
                  f"  {sys.argv[0]} {args.profile} approve-reference --confirm "
                  f"--reviewed-run {run_root}")
            return 2
        if args.reviewed_run is None:
            raise CompatError("--confirm requires --reviewed-run from the review step")
        reviewed = args.reviewed_run.resolve()
        try:
            reviewed.relative_to(COMPAT.resolve())
        except ValueError as exc:
            raise CompatError("reviewed run must be under out/compat") from exc
        reviewed_fingerprint = load_json(reviewed / "resource-fingerprint.json")
        if reviewed_fingerprint.get("fingerprint") != fingerprint:
            raise CompatError("reviewed run resource fingerprint does not match")
        software_output = reviewed / "debug" / "software"
        reviewed_log = (software_output / "host.log").read_text(encoding="utf-8")
        if "scenario completed" not in reviewed_log:
            raise CompatError("reviewed software scenario did not pass")
        reference.mkdir(parents=True, exist_ok=True)
        for checkpoint in checkpoints:
            for suffix in (".ppm", ".layers.json", ".json"):
                shutil.copy2(software_output / f"{checkpoint}{suffix}", reference)
        if (software_output / "scenario-report.json").exists():
            shutil.copy2(software_output / "scenario-report.json", reference)
        print(f"Approved software reference: {reference}")
        return 0
    comparisons = {}
    for run in runs:
        if run["configuration"] != "debug" or run["backend"] == "software":
            continue
        actual = Path(run["output"])
        comparisons[f"candidate/debug/{run['backend']}"] = {
            checkpoint: compare_checkpoint(
                software_output / checkpoint, actual / checkpoint, False,
                profile.get("rois", {}).get(checkpoint, []))
            for checkpoint in checkpoints}
    if not reference.exists():
        comparisons["reference"] = {"pass": False, "reason": "missing approved reference"}
    else:
        for run in runs:
            key = f"{run['configuration']}/{run['backend']}"
            actual = Path(run["output"])
            comparisons[key] = {
                checkpoint: compare_checkpoint(reference / checkpoint,
                                               actual / checkpoint,
                                               run["backend"] == "software",
                                               profile.get("rois", {}).get(
                                                   checkpoint, []))
                for checkpoint in checkpoints}
    summary = {"profile": args.profile, "fingerprint": fingerprint,
               "required_backends": list(REQUIRED),
               "quarantine_backends": list(QUARANTINE), "runs": runs,
               "comparisons": comparisons}
    write_summary(run_root / "summary.json", summary)
    required_failed = any(run["state"] != "passed" for run in runs
                          if run["backend"] in REQUIRED)
    compare_failed = any(not result.get("pass", False)
                         for group in comparisons.values()
                         for result in (group.values() if "reason" not in group else [group]))
    print(run_root / "summary.html")
    return 1 if required_failed or compare_failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CompatError as error:
        print(f"visual compatibility: {error}", file=sys.stderr)
        raise SystemExit(2)
