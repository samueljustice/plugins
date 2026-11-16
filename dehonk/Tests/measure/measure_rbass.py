#!/usr/bin/env python3
"""Measure what Waves RBass adds to audio, for De-Honk (dehonk/SPEC.md, section 7 and 8).

Steps, each a subcommand (``all`` runs them in order and stops if RBass does not load):

  step1    find the Waves shell, list the plug-ins in it, load RBass mono and stereo by
           name, write every parameter to results/rbass_parameters.json
  step2    measure RBass's latency with an impulse (and a noise burst as a cross-check)
           at each sample rate in use, and null-test the least-effect setting on pink noise
  render   generate the test signals, render every test signal and corpus file through
           the settings grid (plus named settings), write corpus/wet/ and results/manifest.csv
  analyse  1/12-octave band analysis, summary ranges, harmonics, plots

RBass is never imitated in code. If it cannot be loaded, the script stops and reports
the exact error.
"""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import glob
import json
import math
import os
import platform
import plistlib
import re
import sys
import traceback
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import signal

HERE = Path(__file__).resolve().parent
TESTS_DIR = HERE.parent

AUDIO_EXTS = {".wav", ".wave", ".aif", ".aiff", ".flac"}
TEST_SR_DEFAULT = 48000

# 1/12-octave bank, 40 Hz to 2.5 kHz
BAND_CENTRES = 40.0 * 2.0 ** (np.arange(0, 72) / 12.0)
BAND_CENTRES = BAND_CENTRES[BAND_CENTRES <= 2500.0 * 2 ** (1 / 24)]
BAND_SPLIT_HZ = 250.0  # bands below this run on the low-rate branch

SUMMARY_RANGES = [
    ("below_100", 0.0, 100.0),
    ("100_150", 100.0, 150.0),
    ("150_250", 150.0, 250.0),
    ("250_500", 250.0, 500.0),
    ("500_1000", 500.0, 1000.0),
]

SINE_FREQS = [30, 40, 50, 60, 80, 100]
TEST_LEVELS_DBFS = [-18, -6]
TEST_SECONDS = 5.0
N_FREQ = 6
N_INTENSITY = 5

ROLE_PATTERNS = {
    "frequency": r"freq",
    "intensity": r"intens|amount|drive",
    "original_bass": r"orig",
    "gain": r"gain|output|level",
}

# Chart colours (dataviz reference palette, light surface)
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
INTENSITY_RAMP = ["#86b6ef", "#5598e7", "#2a78d6", "#1c5cab", "#104281"]
DIVERGING = ["#184f95", "#6da7ec", "#f0efec", "#ec8a89", "#b52a2a"]


# --------------------------------------------------------------------------- helpers


def log(msg: str) -> None:
    print(msg, flush=True)


def db(x, floor=1e-20):
    return 10.0 * np.log10(np.maximum(x, floor))


def peak_dbfs(x: np.ndarray) -> float:
    p = float(np.max(np.abs(x))) if x.size else 0.0
    return 20.0 * math.log10(p) if p > 0 else -math.inf


def short(v) -> str:
    """Shorten numeric parameter text for labels ('55.855855' -> '55.86'); leave other text alone."""
    try:
        return f"{float(v):.4g}"
    except (TypeError, ValueError):
        return str(v)


def sanitise(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_") or "x"


def read_audio(path: Path):
    data, sr = sf.read(str(path), dtype="float32", always_2d=True)
    return np.ascontiguousarray(data.T), int(sr)  # (channels, frames)


def write_audio(path: Path, audio: np.ndarray, sr: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), np.ascontiguousarray(audio.T), sr, subtype="FLOAT")


def rel(path: Path) -> str:
    try:
        return str(Path(path).resolve().relative_to(TESTS_DIR))
    except ValueError:
        return str(path)


class Paths:
    def __init__(self, corpus: Path, results: Path):
        self.corpus = corpus
        self.dry = corpus / "dry"
        self.wet = corpus / "wet"
        self.generated = corpus / "generated"
        self.analysis = self.wet / "analysis"
        self.results = results
        self.plots = results / "plots"
        results.mkdir(parents=True, exist_ok=True)


def run_info() -> dict:
    import pedalboard

    info = {
        "date_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "os": platform.platform(),
        "python": sys.version.split()[0],
        "pedalboard": pedalboard.__version__,
        "numpy": np.__version__,
        "machine": platform.machine(),
    }
    if sys.platform == "darwin":
        info["macos"] = platform.mac_ver()[0]
    return info


# --------------------------------------------------------------------------- step 1


def bundle_version(bundle: Path) -> dict:
    plist = bundle / "Contents" / "Info.plist"
    out = {}
    try:
        with open(plist, "rb") as f:
            p = plistlib.load(f)
        for k in ("CFBundleShortVersionString", "CFBundleVersion", "CFBundleIdentifier"):
            if k in p:
                out[k] = p[k]
    except Exception as e:  # noqa: BLE001
        out["error"] = f"{type(e).__name__}: {e}"
    return out


def find_shells(args) -> dict:
    found = {"vst3": [], "au": []}
    if args.shell:
        for s in args.shell:
            p = Path(s).expanduser()
            fmt = "au" if p.suffix.lower() == ".component" else "vst3"
            found[fmt].append(p)
        return found
    home = Path.home()
    vst3_dirs = [Path("/Library/Audio/Plug-Ins/VST3"), home / "Library/Audio/Plug-Ins/VST3",
                 Path("C:/Program Files/Common Files/VST3")]
    au_dirs = [Path("/Library/Audio/Plug-Ins/Components"), home / "Library/Audio/Plug-Ins/Components"]
    for d in vst3_dirs:
        found["vst3"] += sorted(Path(p) for p in glob.glob(str(d / "WaveShell*.vst3")))
    for d in au_dirs:
        found["au"] += sorted(Path(p) for p in glob.glob(str(d / "WaveShell*.component")))
    return found


def _version_key(path: Path):
    """Shell version, from the bundle name ('WaveShell1-VST3 17.4.vst3' -> (17, 4)), else Info.plist."""
    m = re.search(r"(\d+(?:\.\d+)+)", path.stem.split(" ", 1)[-1])
    text = m.group(1) if m else str(bundle_version(path).get("CFBundleShortVersionString", "0"))
    return tuple(int(n) for n in re.findall(r"\d+", text)) or (0,)


def latest_shell(paths: list):
    return max(paths, key=_version_key) if paths else None


def rbass_bundle_info(args) -> dict:
    """Find the installed RBass bundle and the WaveShell version it was built for, by reading
    its Info.xml (no plug-in is loaded). Picks the newest RBass if several Waves versions have it."""
    roots = [Path(args.waves_dir)] if args.waves_dir else [
        Path("/Applications/Waves"), Path("C:/Program Files (x86)/Waves"), Path("C:/Program Files/Waves")]
    found = []
    for root in roots:
        for b in glob.glob(str(root / "Plug-Ins V*" / "RBass.bundle")):
            info = {"bundle": b}
            try:
                xml = (Path(b) / "Contents" / "Info.xml").read_text(errors="replace")
                for tag in ("PluginExternalVersion", "WavesLibExternalVersion", "WaveShellsBaseName"):
                    m = re.search(rf"<{tag}>([^<]+)</{tag}>", xml)
                    if m:
                        info[tag] = m.group(1).strip()
            except OSError as e:
                info["error"] = str(e)
            v = info.get("WavesLibExternalVersion") or info.get("PluginExternalVersion")
            if v:
                info["shell_version"] = tuple(int(n) for n in v.split(".")[:2])
                found.append(info)
    return max(found, key=lambda i: i["shell_version"]) if found else {}


def pick_shell(paths: list, rb: dict):
    """Exactly one shell per format is loaded: the one whose version matches the installed RBass
    (e.g. RBass 17.1.42 lives in 'WaveShell1-VST3 17.1'). Other WaveShells stay installed and are
    never touched; loading two versions into one process clashes their Objective-C classes.
    If the RBass bundle cannot be found, the newest shell is used."""
    if not paths:
        return None
    if rb.get("shell_version"):
        base = rb.get("WaveShellsBaseName", "WaveShell1")
        match = [p for p in paths if _version_key(p)[:2] == rb["shell_version"] and p.name.startswith(base)
                 and "ARA" not in p.name]
        if match:
            return max(match, key=_version_key)
    return latest_shell(paths)


def classify_rbass_names(names, args):
    rb = [n for n in names if "rbass" in n.lower()]
    # VST3 shells say "RBass Mono" / "RBass Stereo", AU shells "RBass (m)" / "RBass (s)"
    mono = args.mono_name or next((n for n in rb if re.search(r"\bmono\b|\(m\)", n, re.I)), None)
    stereo = args.stereo_name or next((n for n in rb if re.search(r"\bstereo\b|\(s\)", n, re.I)), None)
    return rb, mono, stereo


def describe_parameters(plugin) -> list:
    out = []
    for key, p in plugin.parameters.items():
        d = {"key": key}
        for attr in ("name", "index", "label", "num_steps", "is_discrete", "is_boolean",
                     "is_automatable", "is_meta_parameter", "raw_value", "default_raw_value",
                     "string_value"):
            try:
                v = getattr(p, attr)
                d[attr] = v if isinstance(v, (int, float, str, bool, type(None))) else str(v)
            except Exception as e:  # noqa: BLE001
                d[attr] = f"unavailable: {type(e).__name__}"
        d["units"] = d.pop("label")
        d["type"] = getattr(p.type, "__name__", str(p.type))
        d["min"] = p.min_value
        d["max"] = p.max_value
        d["step_size"] = p.step_size
        d["approximate_step_size"] = p.approximate_step_size
        vv = list(p.valid_values)
        d["n_distinct_values"] = len(vv)
        if d["type"] != "float" or len(vv) <= 64:
            d["valid_values"] = vv
        try:
            d["default_value_text"] = p.get_text_for_raw_value(p.default_raw_value)
        except Exception:  # noqa: BLE001
            pass
        d["value_at_load"] = d.pop("string_value")
        d["raw_value_at_load"] = d.pop("raw_value")
        out.append(d)
    return out


def plugin_identity(plugin) -> dict:
    out = {}
    for attr in ("name", "descriptive_name", "manufacturer_name", "version", "category",
                 "identifier", "reported_latency_samples"):
        try:
            out[attr] = getattr(plugin, attr)
        except Exception as e:  # noqa: BLE001
            out[attr] = f"unavailable: {type(e).__name__}"
    return out


def step1(args, paths: Paths, quiet_ok=False):
    """Returns dict(component -> RBassComponent) or None."""
    import pedalboard

    report = {"run": run_info(), "attempts": [], "result": None}
    shells = find_shells(args)
    report["shells_found"] = {k: [str(p) for p in v] for k, v in shells.items()}
    rb_info = {} if args.shell else rbass_bundle_info(args)
    report["rbass_bundle"] = rb_info
    if rb_info:
        log(f"RBass {rb_info.get('PluginExternalVersion')} found at {rb_info['bundle']}; "
            f"it lives in the {'.'.join(map(str, rb_info['shell_version']))} shell")
    formats = ["vst3", "au"] if args.format == "auto" else [args.format]
    loaded = None

    for fmt in formats:
        cls = pedalboard.VST3Plugin if fmt == "vst3" else getattr(pedalboard, "AudioUnitPlugin", None)
        if cls is None:
            report["attempts"].append({"format": fmt, "error": "pedalboard has no AudioUnitPlugin on this OS"})
            continue
        if not shells[fmt]:
            report["attempts"].append({"format": fmt, "error": "no WaveShell bundle found"})
            continue
        chosen = shells[fmt][0] if args.shell else pick_shell(shells[fmt], rb_info)
        report.setdefault("shells_ignored", {})[fmt] = [str(p) for p in shells[fmt] if p != chosen]
        for shell in [chosen]:
            att = {"format": fmt, "shell": str(shell), "shell_version": bundle_version(shell)}
            report["attempts"].append(att)
            try:
                names = list(cls.get_plugin_names_for_file(str(shell)))
            except Exception as e:  # noqa: BLE001
                att["error"] = f"listing plug-ins failed: {type(e).__name__}: {e}"
                att["traceback"] = traceback.format_exc()
                continue
            att["n_plugins_in_shell"] = len(names)
            att["plugins_in_shell"] = names
            rb, mono, stereo = classify_rbass_names(names, args)
            att["rbass_names"] = rb
            if not rb:
                att["error"] = "no plug-in with 'RBass' in its name in this shell"
                continue
            if not mono or not stereo:
                att["error"] = (f"could not tell the mono and stereo components apart in {rb}; "
                                "pass --mono-name and --stereo-name")
                continue
            comps, errors = {}, {}
            for comp, pname in (("mono", mono), ("stereo", stereo)):
                try:
                    plug = pedalboard.load_plugin(str(shell), plugin_name=pname,
                                                  initialization_timeout=args.init_timeout)
                    comps[comp] = (pname, plug)
                except Exception as e:  # noqa: BLE001
                    errors[comp] = {"plugin_name": pname, "error": f"{type(e).__name__}: {e}",
                                    "traceback": traceback.format_exc()}
            att["load_errors"] = errors
            att["loaded"] = sorted(comps)
            if len(comps) == 2:
                loaded = (fmt, shell, comps)
                break
        if loaded:
            break

    load_report_path = paths.results / "rbass_load_report.json"
    if not loaded:
        report["result"] = "FAILED: RBass did not load. Nothing else was run."
        load_report_path.write_text(json.dumps(report, indent=2, default=str))
        log(report["result"])
        log(f"Details: {rel(load_report_path)}")
        for a in report["attempts"]:
            log(f"  [{a.get('format')}] {a.get('shell', '')}: {a.get('error') or a.get('load_errors')}")
        return None

    fmt, shell, comps = loaded
    report["result"] = "OK"
    load_report_path.write_text(json.dumps(report, indent=2, default=str))

    params_doc = {
        "measured_with": report["run"],
        "format": fmt,
        "shell": str(shell),
        "shell_version": bundle_version(shell),
        "components": {},
    }
    out = {}
    for comp, (pname, plug) in comps.items():
        params_doc["components"][comp] = {
            "plugin_name_in_shell": pname,
            "identity": plugin_identity(plug),
            "parameters": describe_parameters(plug),
        }
        out[comp] = RBassComponent(comp, pname, plug, args)
    params_doc["roles"] = {c: out[c].roles for c in out}
    (paths.results / "rbass_parameters.json").write_text(json.dumps(params_doc, indent=2, default=str))
    log(f"RBass loaded ({fmt}) from {shell}")
    for comp in ("mono", "stereo"):
        log(f"  {comp}: '{out[comp].plugin_name}', parameters: {list(out[comp].plugin.parameters)}")
        log(f"    roles: {out[comp].roles}")
    log(f"Wrote {rel(paths.results / 'rbass_parameters.json')}")
    return out


# --------------------------------------------------------------------------- RBass wrapper


class RBassComponent:
    def __init__(self, comp: str, plugin_name: str, plugin, args):
        self.comp = comp
        self.plugin_name = plugin_name
        self.plugin = plugin
        self.buffer_size = args.buffer_size
        self.keys = list(plugin.parameters)
        self.defaults = {k: plugin.parameters[k].raw_value for k in self.keys}
        self.roles = self._find_roles(args)

    def _find_roles(self, args) -> dict:
        overrides = {"frequency": args.param_frequency, "intensity": args.param_intensity,
                     "original_bass": args.param_original_bass, "gain": args.param_gain}
        roles = {}
        for role, pat in ROLE_PATTERNS.items():
            if overrides[role]:
                if overrides[role] not in self.keys:
                    raise SystemExit(f"--param-{role.replace('_', '-')} '{overrides[role]}' is not one of {self.keys}")
                roles[role] = overrides[role]
                continue
            hits = [k for k in self.keys if re.search(pat, self.plugin.parameters[k].name, re.I)]
            if role == "gain":
                hits = [k for k in hits if k not in roles.values()]
            roles[role] = hits[0] if len(hits) == 1 else (None if not hits else hits)
        return roles

    def check_roles(self):
        bad = {r: v for r, v in self.roles.items() if r != "gain" and not isinstance(v, str)}
        if bad:
            raise SystemExit(
                f"[{self.comp}] could not identify parameters {bad} from {self.keys}. "
                "Pass --param-frequency / --param-intensity / --param-original-bass with the key "
                "from results/rbass_parameters.json.")

    # -- parameter values

    def reset_params(self):
        for k, v in self.defaults.items():
            self.plugin.parameters[k].raw_value = v

    def snapshot(self) -> dict:
        snap = {}
        for k in self.keys:
            p = self.plugin.parameters[k]
            snap[k] = p.string_value
            snap[k + "_raw"] = round(float(p.raw_value), 6)
        return snap

    def grid_raws(self, key: str, n: int) -> list:
        """n raw values spread across the parameter's range. Discrete parameters use
        evenly spaced steps (range midpoints), continuous ones evenly spaced raw values."""
        p = self.plugin.parameters[key]
        ranges = sorted(p.ranges.keys())
        if len(ranges) <= 64:
            idx = np.unique(np.round(np.linspace(0, len(ranges) - 1, n)).astype(int))
            return [(ranges[i][0] + ranges[i][1]) / 2.0 for i in idx]
        return list(np.linspace(0.0, 1.0, n))

    def set_raw(self, key: str, raw: float):
        self.plugin.parameters[key].raw_value = float(raw)

    def set_original_bass(self, on: bool):
        key = self.roles["original_bass"]
        p = self.plugin.parameters[key]
        if p.type is bool:
            setattr(self.plugin, key, bool(on))
            return
        on_words = {"in", "on", "1", "yes", "enabled", "true"}
        off_words = {"out", "off", "0", "no", "disabled", "false"}
        want = on_words if on else off_words
        for rng, val in sorted(p.ranges.items()):
            if str(val).strip().lower() in want:
                self.set_raw(key, (rng[0] + rng[1]) / 2.0)
                return
        self.set_raw(key, 1.0 if on else 0.0)

    def set_named(self, values: dict):
        for name, val in values.items():
            key = name if name in self.keys else next(
                (k for k in self.keys if self.plugin.parameters[k].name.lower() == str(name).lower()), None)
            if key is None:
                raise SystemExit(f"settings.json: parameter '{name}' not found; keys are {self.keys}")
            if isinstance(val, dict) and "raw" in val:
                self.set_raw(key, val["raw"])
            else:
                setattr(self.plugin, key, val)

    def least_effect(self):
        self.reset_params()
        self.set_raw(self.roles["intensity"], self.grid_raws(self.roles["intensity"], N_INTENSITY)[0])
        self.set_original_bass(True)

    # -- processing

    def process(self, audio: np.ndarray, sr: int, lag: int = 0) -> np.ndarray:
        """Render and remove a residual lag (samples) so the output lines up with the input."""
        ch, n = audio.shape
        pad = max(lag, 0)
        x = np.concatenate([audio, np.zeros((ch, pad), np.float32)], axis=1) if pad else audio
        y = self.plugin.process(x, float(sr), buffer_size=self.buffer_size, reset=True)
        y = np.asarray(y, dtype=np.float32)
        if y.ndim == 1:
            y = y[None, :]
        if lag >= 0:
            y = y[:, lag:lag + n]
        else:
            y = np.concatenate([np.zeros((y.shape[0], -lag), np.float32), y], axis=1)[:, :n]
        if y.shape[1] < n:
            y = np.concatenate([y, np.zeros((y.shape[0], n - y.shape[1]), np.float32)], axis=1)
        return y


# --------------------------------------------------------------------------- signals


def fade(x: np.ndarray, sr: int, ms: float = 20.0) -> np.ndarray:
    n = int(sr * ms / 1000)
    w = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))
    x[..., :n] *= w
    x[..., -n:] *= w[::-1]
    return x


def make_test_signals(sr: int) -> list:
    t = np.arange(int(TEST_SECONDS * sr)) / sr
    out = []
    for lvl in TEST_LEVELS_DBFS:
        a = 10 ** (lvl / 20)
        for f in SINE_FREQS:
            x = (a * np.sin(2 * np.pi * f * t)).astype(np.float32)[None, :]
            out.append((f"sine_{f:03d}Hz_{lvl}dBFS", fade(x, sr), {"kind": "sine", "f0": f, "level_dbfs": lvl}))
        x = (a * signal.chirp(t, 20, TEST_SECONDS, 200, method="logarithmic", phi=-90)).astype(np.float32)[None, :]
        out.append((f"sweep_20-200Hz_{lvl}dBFS", fade(x, sr), {"kind": "sweep", "level_dbfs": lvl}))
    return out


def pink_noise(sr: int, seconds: float, rms_dbfs: float = -18.0, seed: int = 1) -> np.ndarray:
    n = int(sr * seconds)
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / sr)
    spec[1:] /= np.sqrt(f[1:])
    spec[0] = 0
    x = np.fft.irfft(spec, n)
    x *= 10 ** (rms_dbfs / 20) / np.sqrt(np.mean(x ** 2))
    return fade(x.astype(np.float32)[None, :], sr)


# --------------------------------------------------------------------------- step 2


def measure_lag(comp: RBassComponent, sr: int, ch: int) -> dict:
    """Residual delay of the rendered output after pedalboard's own compensation for the
    latency the plug-in reports. Positive = output late."""
    n = int(2.0 * sr)
    pos = int(0.5 * sr)
    imp = np.zeros((ch, n), np.float32)
    imp[:, pos] = 0.5
    burst = np.zeros((ch, n), np.float32)
    burst[:, pos:pos + int(0.3 * sr)] = pink_noise(sr, 0.3, -12, seed=7)[0]
    res = {"sample_rate": sr, "component": comp.comp,
           "reported_latency_samples": int(comp.plugin.reported_latency_samples)}
    maxlag = int(0.45 * sr)
    for label, x in (("impulse", imp), ("noise_burst", burst)):
        for setting in ("least_effect", "max_intensity_original_out"):
            comp.least_effect()
            if setting != "least_effect":
                comp.set_raw(comp.roles["intensity"], comp.grid_raws(comp.roles["intensity"], N_INTENSITY)[-1])
                comp.set_original_bass(False)
            y = comp.plugin.process(np.concatenate([x, np.zeros((ch, maxlag), np.float32)], axis=1),
                                    float(sr), buffer_size=comp.buffer_size, reset=True)
            y = np.asarray(y, np.float32)
            xc = signal.correlate(y[0], x[0], mode="full", method="fft")
            centre = x.shape[1] - 1
            window = np.abs(xc[centre - maxlag:centre + maxlag + 1])
            res[f"{label}_{setting}_lag"] = int(np.argmax(window)) - maxlag
    comp.reset_params()
    res["lag_used"] = res["impulse_least_effect_lag"]
    res["impulse_and_noise_agree"] = res["impulse_least_effect_lag"] == res["noise_burst_least_effect_lag"]
    res["lag_same_at_max_setting"] = res["impulse_least_effect_lag"] == res["impulse_max_intensity_original_out_lag"]
    return res


def null_stats(dry: np.ndarray, wet: np.ndarray, sr: int) -> dict:
    d = dry.astype(np.float64)
    w = wet.astype(np.float64)
    rd = float(np.sqrt(np.mean(d ** 2)))
    diff = w - d
    g = float(np.sum(w * d) / max(np.sum(d * d), 1e-30))
    out = {
        "residual_db_re_dry": 20 * math.log10(max(float(np.sqrt(np.mean(diff ** 2))), 1e-15) / max(rd, 1e-15)),
        "residual_peak_dbfs": peak_dbfs(diff),
        "level_change_db": 20 * math.log10(max(float(np.sqrt(np.mean(w ** 2))), 1e-15) / max(rd, 1e-15)),
        "best_gain_db": 20 * math.log10(max(abs(g), 1e-15)),
        "residual_after_best_gain_db": 20 * math.log10(
            max(float(np.sqrt(np.mean((w - g * d) ** 2))), 1e-15) / max(rd, 1e-15)),
    }
    lt_d = range_levels(dry, sr)
    lt_w = range_levels(wet, sr)
    for name, _, _ in SUMMARY_RANGES:
        out[f"change_{name}_db"] = lt_w[name] - lt_d[name]
    return out


class LagTable:
    def __init__(self, paths: Paths):
        self.path = paths.results / "host_check.json"
        self.data = json.loads(self.path.read_text()) if self.path.exists() else {"latency": [], "null_pink_noise": []}

    def get(self, comp, sr, ch):
        for r in self.data["latency"]:
            if r["component"] == comp.comp and r["sample_rate"] == sr:
                return r["lag_used"]
        r = measure_lag(comp, sr, ch)
        self.data["latency"].append(r)
        log(f"  latency [{comp.comp} @ {sr}]: reported {r['reported_latency_samples']}, "
            f"residual after compensation {r['lag_used']} samples")
        self.save()
        return r["lag_used"]

    def save(self):
        self.path.write_text(json.dumps(self.data, indent=2, default=str))


def step2(args, comps, paths: Paths, rates=None):
    for c in comps.values():
        c.check_roles()
    lt = LagTable(paths)
    lt.data["run"] = run_info()
    rates = sorted(set(rates or []) | {args.test_sr} | set(corpus_rates(paths)))
    lt.data["null_pink_noise"] = []
    for sr in rates:
        for name, ch in (("mono", 1), ("stereo", 2)):
            comp = comps[name]
            lag = lt.get(comp, sr, ch)
            x = np.repeat(pink_noise(sr, 5.0), ch, axis=0)
            comp.least_effect()
            settings = comp.snapshot()
            y = comp.process(x, sr, lag)
            row = {"component": name, "sample_rate": sr, "lag_used": lag, "signal": "pink noise -18 dBFS RMS, 5 s",
                   "settings": settings, **null_stats(x, y, sr)}
            lt.data["null_pink_noise"].append(row)
            log(f"  null [{name} @ {sr}] least-effect vs dry: residual {row['residual_db_re_dry']:.1f} dB, "
                f"level change {row['level_change_db']:+.2f} dB")
    lt.save()
    log(f"Wrote {rel(lt.path)}")
    return lt


# --------------------------------------------------------------------------- corpus


def list_audio(d: Path) -> list:
    if not d.is_dir():
        return []
    return sorted(p for p in d.rglob("*") if p.is_file() and p.suffix.lower() in AUDIO_EXTS
                  and not p.name.startswith("."))


def discover_corpus(paths: Paths, keep_names: bool) -> tuple:
    items, skipped = [], []
    prefix = {"sources": "source", "stems": "stem", "mixes": "mix", "clean": "clean"}
    for cat in ("sources", "stems", "clean"):
        for i, p in enumerate(list_audio(paths.dry / cat), 1):
            name = sanitise(str(p.relative_to(paths.dry / cat).with_suffix(""))) if keep_names else f"{prefix[cat]}{i:02d}"
            items.append({"category": cat, "item": name, "files": {"main": p}})
    mixes_dir = paths.dry / "mixes"
    if mixes_dir.is_dir():
        i = 0
        for d in sorted(x for x in mixes_dir.iterdir() if x.is_dir()):
            bass = next((p for p in list_audio(d) if p.stem.lower() == "bass"), None)
            rest = next((p for p in list_audio(d) if p.stem.lower() == "rest"), None)
            if not bass or not rest:
                skipped.append({"path": str(d), "reason": "mix folder needs bass.wav and rest.wav"})
                continue
            i += 1
            name = sanitise(d.name) if keep_names else f"mix{i:02d}"
            items.append({"category": "mixes", "item": name, "files": {"bass": bass, "rest": rest}})
    for it in items:
        infos = {k: sf.info(str(v)) for k, v in it["files"].items()}
        it["sample_rate"] = int(next(iter(infos.values())).samplerate)
        it["channels"] = {k: v.channels for k, v in infos.items()}
        if len({int(v.samplerate) for v in infos.values()}) > 1:
            it["skip"] = "bass.wav and rest.wav have different sample rates"
        elif any(v.channels > 2 for v in infos.values()):
            it["skip"] = "more than two channels"
    return items, skipped


def corpus_rates(paths: Paths) -> list:
    rates = set()
    for p in list_audio(paths.dry):
        try:
            rates.add(int(sf.info(str(p)).samplerate))
        except Exception:  # noqa: BLE001
            pass
    return sorted(rates)


def load_settings_json(paths: Paths) -> list:
    p = paths.corpus / "settings.json"
    if not p.exists():
        return []
    doc = json.loads(p.read_text())
    if isinstance(doc, dict) and "settings" in doc:
        doc = doc["settings"]
    if isinstance(doc, dict):
        doc = [{"name": k, "params": v} for k, v in doc.items()]
    return [{"name": sanitise(s["name"]), "label": s["name"], "params": s["params"]} for s in doc]


def match_channels(a: np.ndarray, b: np.ndarray):
    if a.shape[0] == b.shape[0]:
        return a, b
    if a.shape[0] == 1:
        return np.repeat(a, b.shape[0], axis=0), b
    return a, np.repeat(b, a.shape[0], axis=0)


def pad_to(x: np.ndarray, n: int) -> np.ndarray:
    return x if x.shape[1] >= n else np.concatenate([x, np.zeros((x.shape[0], n - x.shape[1]), x.dtype)], axis=1)


# --------------------------------------------------------------------------- render


def settings_list(comp: RBassComponent, named: list) -> list:
    out = [{"kind": "least_effect", "id": "least_effect"}]
    f_raws = comp.grid_raws(comp.roles["frequency"], N_FREQ)
    i_raws = comp.grid_raws(comp.roles["intensity"], N_INTENSITY)
    for fi, fr in enumerate(f_raws):
        for ii, ir in enumerate(i_raws):
            for orig in (True, False):
                out.append({"kind": "grid", "id": f"f{fi}_i{ii}_orig-{'in' if orig else 'out'}",
                            "fi": fi, "ii": ii, "f_raw": fr, "i_raw": ir, "orig": orig})
    for s in named:
        out.append({"kind": "named", "id": f"named_{s['name']}", "label": s["label"], "params": s["params"]})
    return out


def apply_setting(comp: RBassComponent, s: dict):
    if s["kind"] == "least_effect":
        comp.least_effect()
        return
    comp.reset_params()
    if s["kind"] == "grid":
        comp.set_raw(comp.roles["frequency"], s["f_raw"])
        comp.set_raw(comp.roles["intensity"], s["i_raw"])
        comp.set_original_bass(s["orig"])
    else:
        comp.set_named(s["params"])


def render(args, comps, paths: Paths):
    for c in comps.values():
        c.check_roles()
    named = load_settings_json(paths)
    if named:
        log(f"Named settings from corpus/settings.json: {[s['label'] for s in named]}")
    lags = LagTable(paths)

    # test signals (generated, 32-bit float, mono)
    work = []
    if not args.skip_test_signals:
        for name, x, meta in make_test_signals(args.test_sr):
            p = paths.generated / "test_signals" / f"{name}.wav"
            write_audio(p, x, args.test_sr)
            work.append({"category": "test_signals", "item": name, "files": {"main": p},
                         "sample_rate": args.test_sr, "channels": {"main": 1}, "meta": meta})
    items, skipped = discover_corpus(paths, args.keep_names)
    work += [it for it in items if it["category"] != "clean" and "skip" not in it]
    skipped += [{"path": str(it["files"]), "reason": it["skip"]} for it in items if "skip" in it]
    if args.only:
        work = [w for w in work if any(o in w["item"] for o in args.only)]

    name_map = [{"category": it["category"], "item": it["item"],
                 **{k: str(v.relative_to(paths.dry)) for k, v in it["files"].items()}} for it in items]
    if name_map:
        with open(paths.corpus / "name_map.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=sorted({k for r in name_map for k in r}))
            w.writeheader()
            w.writerows(name_map)

    rows, rows_private, null_rows = [], [], []
    total = sum(len(settings_list(comps["mono"], named)) for _ in work)
    done = 0
    for it in work:
        sr = it["sample_rate"]
        if it["category"] == "mixes":
            bass, _ = read_audio(it["files"]["bass"])
            rest, _ = read_audio(it["files"]["rest"])
            n = max(bass.shape[1], rest.shape[1])
            bass, rest = pad_to(bass, n), pad_to(rest, n)
            part = bass
            b2, r2 = match_channels(bass, rest)
            dry = b2 + r2
            dry_path = paths.wet / it["category"] / it["item"] / "dry_sum.wav"
            write_audio(dry_path, dry, sr)
            dry_rel_private = f"{rel(it['files']['bass'])} + {rel(it['files']['rest'])}"
        else:
            dry, _ = read_audio(it["files"]["main"])
            part = dry
            dry_path = it["files"]["main"]
            dry_rel_private = rel(dry_path)
        ch = part.shape[0]
        comp = comps["mono" if ch == 1 else "stereo"]
        lag = lags.get(comp, sr, ch)
        it["dry_path"] = dry_path
        public_dry = rel(dry_path) if (args.keep_names or it["category"] == "test_signals") \
            else f"{it['category']}/{it['item']} (real path in corpus/name_map.csv)"

        for s in settings_list(comp, named):
            done += 1
            wet_path = paths.wet / it["category"] / it["item"] / f"{s['id']}.wav"
            apply_setting(comp, s)
            snap = comp.snapshot()
            if wet_path.exists() and not args.force:
                wet, _ = read_audio(wet_path)
                part_peak = None
            else:
                y = comp.process(part, sr, lag)
                part_peak = peak_dbfs(y)
                if it["category"] == "mixes":
                    y2, r2 = match_channels(y, rest)
                    wet = y2 + r2
                else:
                    wet = y
                write_audio(wet_path, wet, sr)
            pk = peak_dbfs(wet)
            row = {
                "category": it["category"], "item": it["item"],
                "dry_path": public_dry, "wet_path": rel(wet_path),
                "setting_kind": s["kind"], "setting_id": s["id"],
                "named_setting": s.get("label", ""),
                "component": comp.comp, "plugin_name": comp.plugin_name,
                "frequency_index": s.get("fi", ""), "intensity_index": s.get("ii", ""),
                "original_bass": "" if s["kind"] == "named" else ("in" if s.get("orig", True) else "out"),
                "frequency": snap[comp.roles["frequency"]], "intensity": snap[comp.roles["intensity"]],
                "sample_rate": sr, "channels": wet.shape[0], "frames": wet.shape[1],
                "latency_comp_samples": lag,
                "peak_dbfs": round(pk, 3), "over_0dbfs": pk > 0.0,
                "peak_processed_part_dbfs": "" if part_peak is None else round(part_peak, 3),
                **{f"p_{k}": v for k, v in snap.items()},
            }
            rows.append(row)
            rows_private.append({**row, "dry_path": dry_rel_private})
            if s["kind"] == "least_effect":
                null_rows.append({"category": it["category"], "item": it["item"], "component": comp.comp,
                                  "sample_rate": sr, "latency_comp_samples": lag,
                                  **{k: round(v, 3) for k, v in null_stats(dry, wet, sr).items()},
                                  "intensity": row["intensity"], "original_bass": "in"})
            if pk > 0:
                log(f"  OVER 0 dBFS: {it['item']} {s['id']} peak {pk:+.2f} dBFS")
            if done % 50 == 0 or done == total:
                log(f"  rendered {done}/{total}")

    write_csv(paths.results / "manifest.csv", rows)
    write_csv(paths.corpus / "manifest_private.csv", rows_private)
    write_csv(paths.results / "null_test.csv", null_rows)
    write_csv(paths.results / "skipped.csv", skipped, fields=["path", "reason"])
    missing = [r["wet_path"] for r in rows if not (TESTS_DIR / r["wet_path"]).exists()]
    overs = sum(1 for r in rows if r["over_0dbfs"])
    log(f"Wrote {rel(paths.results / 'manifest.csv')}: {len(rows)} renders, {overs} over 0 dBFS, "
        f"{len(missing)} listed wet files missing, {len(skipped)} skipped")
    return rows


def write_csv(path: Path, rows: list, fields=None):
    fields = fields or (list(dict.fromkeys(k for r in rows for k in r)) if rows else [])
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow({k: (f"{v:.3f}" if isinstance(v, float) and math.isfinite(v) else v) for k, v in r.items()})


# --------------------------------------------------------------------------- analysis


class BandBank:
    """1/12-octave bank, 40 Hz to 2.5 kHz. Bands below 250 Hz run on a ~1.5 kHz copy of
    the signal, the rest on a ~8 kHz copy. 6th-order Butterworth band-pass per band.
    Levels are mean power, reported in dB re a full-scale sine (a 0 dBFS-peak sine reads 0 dB)."""

    def __init__(self, sr: int):
        self.sr = sr
        self.q_low = max(1, sr // 1500)
        self.q_high = max(1, sr // 8000)
        self.filters = []
        for fc in BAND_CENTRES:
            q = self.q_low if fc < BAND_SPLIT_HZ else self.q_high
            fs = sr / q
            lo, hi = fc * 2 ** (-1 / 24), fc * 2 ** (1 / 24)
            sos = signal.butter(3, [lo, hi], btype="bandpass", fs=fs, output="sos")
            self.filters.append((q, sos))

    def run(self, x: np.ndarray, frame_s: float = 0.01):
        """x (channels, frames) -> (long-term dB per band, per-frame dB (bands, frames), frame times)"""
        x = x.astype(np.float64)
        n_frames = int(math.ceil(x.shape[1] / self.sr / frame_s))
        dec = {}
        for q in {self.q_low, self.q_high}:
            dec[q] = signal.resample_poly(x, 1, q, axis=1) if q > 1 else x
        lt = np.zeros(len(self.filters))
        fr = np.zeros((len(self.filters), n_frames))
        for b, (q, sos) in enumerate(self.filters):
            y = signal.sosfilt(sos, dec[q], axis=1)
            pw = np.mean(y ** 2, axis=0)
            lt[b] = np.mean(pw)
            fs = self.sr / q
            idx = np.minimum((np.arange(pw.size) / fs / frame_s).astype(int), n_frames - 1)
            s = np.bincount(idx, weights=pw, minlength=n_frames)
            c = np.bincount(idx, minlength=n_frames)
            fr[b] = s / np.maximum(c, 1)
        return db(2 * lt), db(2 * fr).astype(np.float32), np.arange(n_frames) * frame_s


def range_levels(x: np.ndarray, sr: int) -> dict:
    """Mean power in each summary range from one FFT of the whole signal, dB re full-scale sine.
    Uses brick-wall bins, so 'below_100' includes everything from just above DC to 100 Hz."""
    x = x.astype(np.float64)
    n = x.shape[1]
    X = np.fft.rfft(x, axis=1)
    p = np.mean(np.abs(X) ** 2, axis=0) * 2.0 / (n * n)
    p[0] = 0.0
    f = np.fft.rfftfreq(n, 1 / sr)
    out = {}
    for name, lo, hi in SUMMARY_RANGES:
        sel = (f > lo) & (f <= hi)
        out[name] = float(db(2 * np.sum(p[sel])))
    return out


def harmonic_levels(x: np.ndarray, sr: int, f0: float, n_h: int = 10) -> list:
    """Peak amplitude (dBFS) at k*f0 over the steady middle of the signal, Hann-windowed."""
    a, b = int(0.5 * sr), x.shape[1] - int(0.5 * sr)
    seg = x[:, a:b].astype(np.float64)
    w = np.hanning(seg.shape[1])
    t = np.arange(seg.shape[1]) / sr
    out = []
    for k in range(1, n_h + 1):
        e = np.exp(-2j * np.pi * k * f0 * t)
        amp = np.mean([2 * abs(np.sum(ch * w * e)) / np.sum(w) for ch in seg])
        out.append(20 * math.log10(max(amp, 1e-12)))
    return out


def analyse(args, paths: Paths):
    man_path = paths.results / "manifest.csv"
    rows = list(csv.DictReader(open(man_path))) if man_path.exists() else []
    private = paths.corpus / "manifest_private.csv"
    priv_rows = list(csv.DictReader(open(private))) if private.exists() else []
    dry_lookup = {}
    for r in priv_rows:
        if r["category"] == "mixes":
            dry_lookup[(r["category"], r["item"])] = paths.wet / "mixes" / r["item"] / "dry_sum.wav"
        else:
            dry_lookup[(r["category"], r["item"])] = TESTS_DIR / r["dry_path"]
    paths.analysis.mkdir(parents=True, exist_ok=True)
    banks = {}
    bands_cols = [f"{fc:.1f}" for fc in BAND_CENTRES]
    lt_rows, sum_rows, harm_rows = [], [], []
    dry_cache = {}

    def bank(sr):
        if sr not in banks:
            banks[sr] = BandBank(sr)
        return banks[sr]

    for i, r in enumerate(rows, 1):
        key = (r["category"], r["item"])
        if key not in dry_lookup:
            log(f"  no dry file for {key}; skipping")
            continue
        sr = int(r["sample_rate"])
        if key not in dry_cache:
            dry, _ = read_audio(dry_lookup[key])
            lt_d, fr_d, times = bank(sr).run(dry)
            dry_cache.clear()
            dry_cache[key] = (dry, lt_d, fr_d, times, range_levels(dry, sr))
        dry, lt_d, fr_d, times, rl_d = dry_cache[key]
        wet, _ = read_audio(TESTS_DIR / r["wet_path"])
        lt_w, fr_w, _ = bank(sr).run(wet)
        np.savez_compressed(paths.analysis / f"{r['category']}__{r['item']}__{r['setting_id']}.npz",
                            band_hz=BAND_CENTRES, frame_s=times, dry_db=fr_d.astype(np.float16),
                            wet_db=fr_w.astype(np.float16))
        meta = {k: r[k] for k in ("category", "item", "setting_kind", "setting_id", "named_setting", "component",
                                  "frequency_index", "intensity_index", "frequency", "intensity", "original_bass")}
        lt_rows.append({**meta,
                        **{f"dry_{c}": round(float(v), 2) for c, v in zip(bands_cols, lt_d)},
                        **{f"wet_{c}": round(float(v), 2) for c, v in zip(bands_cols, lt_w)},
                        **{f"diff_{c}": round(float(w - d), 2) for c, d, w in zip(bands_cols, lt_d, lt_w)}})
        rl_w = range_levels(wet, sr)
        sr_row = dict(meta)
        for name, _, _ in SUMMARY_RANGES:
            sr_row[f"dry_{name}_db"] = round(rl_d[name], 2)
            sr_row[f"wet_{name}_db"] = round(rl_w[name], 2)
            sr_row[f"change_{name}_db"] = round(rl_w[name] - rl_d[name], 2)
        sum_rows.append(sr_row)
        m = re.match(r"sine_(\d+)Hz_(-?\d+)dBFS", r["item"])
        if r["category"] == "test_signals" and m:
            f0 = float(m.group(1))
            hd = harmonic_levels(dry, sr, f0)
            hw = harmonic_levels(wet, sr, f0)
            hr = {**meta, "f0_hz": f0, "input_dbfs": int(m.group(2))}
            for k in range(10):
                hr[f"h{k + 1}_hz"] = f0 * (k + 1)
                hr[f"h{k + 1}_dry_dbfs"] = round(hd[k], 2)
                hr[f"h{k + 1}_wet_dbfs"] = round(hw[k], 2)
                hr[f"h{k + 1}_change_db"] = round(hw[k] - hd[k], 2)
            harm_rows.append(hr)
        if i % 50 == 0 or i == len(rows):
            log(f"  analysed {i}/{len(rows)}")

    # clean material: dry only
    items, _ = discover_corpus(paths, args.keep_names)
    for it in items:
        if it["category"] != "clean" or "skip" in it:
            continue
        x, sr = read_audio(it["files"]["main"])
        lt_d, fr_d, times = bank(sr).run(x)
        np.savez_compressed(paths.analysis / f"clean__{it['item']}__dry.npz",
                            band_hz=BAND_CENTRES, frame_s=times, dry_db=fr_d.astype(np.float16))
        lt_rows.append({"category": "clean", "item": it["item"], "setting_kind": "dry_only",
                        **{f"dry_{c}": round(float(v), 2) for c, v in zip(bands_cols, lt_d)}})

    write_csv(paths.results / "bands_longterm.csv", lt_rows)
    write_csv(paths.results / "summary.csv", sum_rows)
    write_csv(paths.results / "harmonics.csv", harm_rows)
    log(f"Wrote bands_longterm.csv ({len(lt_rows)} rows), summary.csv ({len(sum_rows)}), "
        f"harmonics.csv ({len(harm_rows)})")
    plots(paths, lt_rows)


# --------------------------------------------------------------------------- plots


def _style(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(True, which="major", color=GRID, linewidth=0.6)
    ax.grid(True, which="minor", axis="x", color=GRID, linewidth=0.3)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(AXIS)
    ax.tick_params(colors=INK2, labelsize=8)
    ax.axhline(0, color=AXIS, linewidth=1.0, zorder=1)


def plots(paths: Paths, lt_rows: list):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import LinearSegmentedColormap
    from matplotlib.ticker import FixedLocator, FuncFormatter

    paths.plots.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.family": "sans-serif", "font.size": 9, "text.color": INK,
                         "axes.labelcolor": INK2, "figure.facecolor": SURFACE})
    cols = [f"{fc:.1f}" for fc in BAND_CENTRES]
    xticks = [50, 100, 200, 500, 1000, 2000]
    fmt = FuncFormatter(lambda v, _: f"{v / 1000:g}k" if v >= 1000 else f"{v:g}")
    grid = [r for r in lt_rows if r.get("setting_kind") == "grid"]

    by_item = {}
    for r in grid:
        by_item.setdefault((r["category"], r["item"]), []).append(r)
    for (cat, item), rs in by_item.items():
        diffs = np.array([[float(r[f"diff_{c}"]) for c in cols] for r in rs])
        lim = max(3.0, float(np.nanmax(np.abs(diffs))) * 1.05)
        for orig in ("in", "out"):
            sub = [r for r in rs if r["original_bass"] == orig]
            if not sub:
                continue
            fis = sorted({int(r["frequency_index"]) for r in sub})
            fig, axes = plt.subplots(2, 3, figsize=(12, 6.2), sharex=True, sharey=True)
            for ax, fi in zip(axes.flat, fis):
                _style(ax)
                pan = sorted((r for r in sub if int(r["frequency_index"]) == fi), key=lambda r: int(r["intensity_index"]))
                for r in pan:
                    ii = int(r["intensity_index"])
                    ax.semilogx(BAND_CENTRES, [float(r[f"diff_{c}"]) for c in cols],
                                color=INTENSITY_RAMP[ii % len(INTENSITY_RAMP)], linewidth=2,
                                label=f"Intensity {short(r['intensity'])}")
                ax.set_title(f"Frequency {short(pan[0]['frequency'])}", color=INK, fontsize=9, loc="left")
                ax.xaxis.set_major_locator(FixedLocator(xticks))
                ax.xaxis.set_major_formatter(fmt)
                ax.set_xlim(BAND_CENTRES[0], BAND_CENTRES[-1])
                ax.set_ylim(-lim, lim)
            for ax in axes.flat[len(fis):]:
                ax.set_visible(False)
            for ax in axes[-1]:
                ax.set_xlabel("Band centre (Hz)")
            for ax in axes[:, 0]:
                ax.set_ylabel("Wet − dry (dB)")
            h, lab = axes.flat[0].get_legend_handles_labels()
            fig.suptitle(f"{cat}/{item}: RBass wet minus dry per 1/12-octave band, Original bass {orig}",
                         x=0.01, ha="left", color=INK, fontsize=10)
            fig.tight_layout(rect=(0, 0.05, 1, 0.95))
            fig.legend(h, lab, loc="lower center", frameon=False, ncol=len(lab), fontsize=8, labelcolor=INK2)
            fig.savefig(paths.plots / f"{cat}__{item}__orig-{orig}.png", dpi=100)
            plt.close(fig)

    cmap = LinearSegmentedColormap.from_list("div", DIVERGING)
    groups = {"corpus": [r for r in lt_rows if r["category"] in ("sources", "stems", "mixes")
                         and r.get("setting_kind") in ("grid", "named")],
              "test_signals": [r for r in lt_rows if r["category"] == "test_signals"
                               and r.get("setting_kind") in ("grid", "named")]}
    for gname, rs in groups.items():
        if not rs:
            continue
        settings = {}
        for r in rs:
            settings.setdefault(r["setting_id"], []).append([float(r[f"diff_{c}"]) for c in cols])
        def sort_key(sid):
            m = re.match(r"f(\d+)_i(\d+)_orig-(in|out)", sid)
            return (1, 0, 0, 0, sid) if not m else (0, m.group(3) == "out", int(m.group(1)), int(m.group(2)), sid)
        order = sorted(settings, key=sort_key)
        label = {}
        for r in rs:
            if r["setting_kind"] == "named":
                label[r["setting_id"]] = f"{r['named_setting']} (named)"
            else:
                label[r["setting_id"]] = (f"Orig {r['original_bass']} · F {short(r['frequency'])} · "
                                          f"I {short(r['intensity'])}")
        mat = np.array([np.mean(settings[s], axis=0) for s in order])
        n_src = {s: len(settings[s]) for s in order}
        vmax = max(1.0, float(np.nanpercentile(np.abs(mat), 99)))
        fig, ax = plt.subplots(figsize=(12, 1.6 + 0.16 * len(order)))
        im = ax.pcolormesh(np.arange(len(cols) + 1), np.arange(len(order) + 1), mat, cmap=cmap,
                           vmin=-vmax, vmax=vmax, shading="flat")
        ax.set_yticks(np.arange(len(order)) + 0.5)
        ax.set_yticklabels([label[s] for s in order], fontsize=6, color=INK2)
        ax.invert_yaxis()
        tick_idx = [int(np.argmin(np.abs(BAND_CENTRES - t))) for t in xticks]
        ax.set_xticks(np.array(tick_idx) + 0.5)
        ax.set_xticklabels([fmt(t, None) for t in xticks], color=INK2)
        ax.set_xlabel("Band centre (Hz)")
        for s in ax.spines.values():
            s.set_visible(False)
        cb = fig.colorbar(im, ax=ax, fraction=0.025, pad=0.01)
        cb.set_label("Added level, wet − dry (dB)", color=INK2)
        cb.ax.tick_params(colors=INK2, labelsize=7)
        cb.outline.set_visible(False)
        n = sorted(set(n_src.values()))
        ax.set_title(f"Added level per band by RBass setting, mean over {'/'.join(map(str, n))} "
                     f"{'corpus files' if gname == 'corpus' else 'test signals'}",
                     loc="left", color=INK, fontsize=10)
        fig.tight_layout()
        fig.savefig(paths.plots.parent / f"heatmap_added_level{'' if gname == 'corpus' else '_test_signals'}.png",
                    dpi=110)
        plt.close(fig)
    log(f"Wrote plots to {rel(paths.plots)}")


# --------------------------------------------------------------------------- main


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=["step1", "step2", "render", "analyse", "all"])
    ap.add_argument("--shell", action="append", help="path to a WaveShell .vst3 or .component (repeatable)")
    ap.add_argument("--format", choices=["auto", "vst3", "au"], default="auto")
    ap.add_argument("--waves-dir", help="Waves install folder holding 'Plug-Ins V*' (default /Applications/Waves)")
    ap.add_argument("--mono-name", help="name of the mono component in the shell, if not found automatically")
    ap.add_argument("--stereo-name", help="name of the stereo component in the shell")
    ap.add_argument("--param-frequency")
    ap.add_argument("--param-intensity")
    ap.add_argument("--param-original-bass")
    ap.add_argument("--param-gain")
    ap.add_argument("--init-timeout", type=float, default=10.0)
    ap.add_argument("--buffer-size", type=int, default=512)
    ap.add_argument("--test-sr", type=int, default=TEST_SR_DEFAULT)
    ap.add_argument("--corpus", type=Path, default=TESTS_DIR / "corpus")
    ap.add_argument("--results", type=Path, default=HERE / "results")
    ap.add_argument("--keep-names", action="store_true",
                    help="use real corpus file names in committed results (default: anonymised ids)")
    ap.add_argument("--force", action="store_true", help="re-render wet files that already exist")
    ap.add_argument("--skip-test-signals", action="store_true")
    ap.add_argument("--only", action="append", help="only items whose id contains this text (repeatable)")
    args = ap.parse_args(argv)
    paths = Paths(args.corpus.resolve(), args.results.resolve())
    (paths.results / "run_info.json").write_text(json.dumps(run_info(), indent=2))

    if args.step == "analyse":
        analyse(args, paths)
        return 0
    comps = step1(args, paths)
    if comps is None:
        return 2
    if args.step == "step1":
        return 0
    if args.step in ("step2", "all"):
        step2(args, comps, paths)
    if args.step in ("render", "all"):
        render(args, comps, paths)
    if args.step == "all":
        analyse(args, paths)
    return 0


if __name__ == "__main__":
    sys.exit(main())
