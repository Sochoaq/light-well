#!/usr/bin/env python3
"""Offline Photometric Reconstruction and Circadian Spectral Regime ML Pipeline.

Replicates the ESP32-C3 edge photometry physics engine (Gaussian SPD reconstruction,
CIE 1931 2-deg tristimulus integration, McCamy CCT estimation, and CIE S 026 mEDI)
and evaluates supervised regime classification on the AS7262 + BH1750 dataset.
"""

from __future__ import annotations

import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

LAMBDA_START = 400
LAMBDA_END = 700
LAMBDA_STEP = 1
WAVELENGTHS_SPD = list(range(LAMBDA_START, LAMBDA_END + 1, LAMBDA_STEP))

AS7262_WL = (450.0, 500.0, 550.0, 570.0, 600.0, 650.0)
SIGMA_DEFAULT = 17.0
SIGMA_VIOLET = 0.65 * SIGMA_DEFAULT
AS7262_SIGMAS = (
    SIGMA_VIOLET,
    SIGMA_DEFAULT,
    SIGMA_DEFAULT,
    SIGMA_DEFAULT,
    SIGMA_DEFAULT,
    SIGMA_DEFAULT,
)
AS7262_GAINS = (1.24, 4.3, 0.95, 0.95, 0.415, 1.2)

EXTRA_WL = (525.0, 626.0)
EXTRA_GAINS = (0.7, 0.35)
EXTRA_SIGMAS = (SIGMA_DEFAULT, SIGMA_DEFAULT)

CCT_MIN = 2000.0
CCT_MAX = 10000.0

EDI_PINK_MIN = 0.0
EDI_PINK_MAX = 10.0
EDI_RED_MIN = 10.0
EDI_RED_MAX = 20.0

WAVELENGTHS_CIE = tuple(range(380, 785, 5))

X_BAR = (
    0.001368, 0.002236, 0.004243, 0.00765, 0.01431, 0.02319, 0.04351, 0.07763, 0.13438, 0.21477,
    0.2839, 0.3285, 0.34828, 0.34806, 0.3362, 0.3187, 0.2908, 0.2511, 0.19536, 0.1421,
    0.09564, 0.05795001, 0.03201, 0.0147, 0.0049, 0.0024, 0.0093, 0.0291, 0.06327, 0.1096,
    0.1655, 0.2257499, 0.2904, 0.3597, 0.4334499, 0.5120501, 0.5945, 0.6784, 0.7621, 0.8425,
    0.9163, 0.9786, 1.0263, 1.0567, 1.0622, 1.0456, 1.0026, 0.9384, 0.8544499, 0.7514,
    0.6424, 0.5419, 0.4479, 0.3608, 0.2835, 0.2187, 0.1649, 0.1212, 0.0874, 0.0636,
    0.04677, 0.0329, 0.0227, 0.01584, 0.01135916, 0.008110916, 0.005790346, 0.004109457, 0.002899327, 0.00204919,
    0.001439971, 0.000999949, 0.000690079, 0.000476021, 0.000332301, 0.000234826, 0.000166151, 0.000117413, 8.30753e-05, 5.87065e-05,
    4.15099e-05,
)

Y_BAR = (
    0.000039, 0.000064, 0.00012, 0.000217, 0.000396, 0.00064, 0.00121, 0.00218, 0.004, 0.0073,
    0.0116, 0.01684, 0.023, 0.0298, 0.038, 0.048, 0.06, 0.0739, 0.09098, 0.1126,
    0.13902, 0.1693, 0.20802, 0.2586, 0.323, 0.4073, 0.503, 0.6082, 0.71, 0.7932,
    0.862, 0.9148501, 0.954, 0.9803, 0.9949501, 1.0, 0.995, 0.9786, 0.952, 0.9154,
    0.87, 0.8163, 0.757, 0.6949, 0.631, 0.5668, 0.503, 0.4412, 0.381, 0.321,
    0.265, 0.217, 0.175, 0.1382, 0.107, 0.0816, 0.061, 0.04458, 0.032, 0.0232,
    0.017, 0.01192, 0.00821, 0.005723, 0.004102, 0.002929, 0.002091, 0.001484, 0.001047, 0.00074,
    0.00052, 0.0003611, 0.0002492, 0.0001719, 0.00012, 0.0000848, 0.00006, 0.0000424, 0.00003, 0.0000212,
    0.00001499,
)

Z_BAR = (
    0.006450001, 0.01054999, 0.02005001, 0.03621, 0.06785001, 0.1102, 0.2074, 0.3713, 0.6456, 1.0390501,
    1.3856, 1.62296, 1.74706, 1.7826, 1.77211, 1.7441, 1.6692, 1.5281, 1.28764, 1.0419,
    0.8129501, 0.6162, 0.46518, 0.3533, 0.272, 0.2123, 0.1582, 0.1117, 0.07824999, 0.05725001,
    0.04216, 0.02984, 0.0203, 0.0134, 0.008749999, 0.005749999, 0.0039, 0.002749999, 0.0021, 0.0018,
    0.001650001, 0.0014, 0.0011, 0.001, 0.0008, 0.0006, 0.00034, 0.00024, 0.00019, 0.0001,
    5e-05, 0.00003, 0.00002, 0.00001, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    0.0,
)


def _gaussian(x: float, mu: float, sigma: float, amp: float) -> float:
    exponent = -((x - mu) ** 2) / (2.0 * (sigma ** 2))
    return amp * math.exp(exponent) / (sigma * math.sqrt(2.0 * math.pi))


def _linear_interp(x: float, wl_table: Sequence[int], val_table: Sequence[float]) -> float:
    if x < wl_table[0] or x > wl_table[-1]:
        return 0.0
    for i in range(len(wl_table) - 1):
        if wl_table[i] <= x <= wl_table[i + 1]:
            frac = (x - wl_table[i]) / (wl_table[i + 1] - wl_table[i])
            return val_table[i] + frac * (val_table[i + 1] - val_table[i])
    return 0.0


def reconstruct_spd(
    violet: float,
    blue: float,
    green: float,
    yellow: float,
    orange: float,
    red: float,
) -> List[Tuple[float, float]]:
    """Reconstruct the 400-700 nm Gaussian spectral power distribution (1 nm step)."""
    channels = (violet, blue, green, yellow, orange, red)
    extras = ((channels[1] + channels[2]) / 2.0, (channels[4] + channels[5]) / 2.0)

    amps = [channels[i] * AS7262_GAINS[i] for i in range(6)]
    amps_extra = [extras[i] * EXTRA_GAINS[i] for i in range(2)]

    spd: List[Tuple[float, float]] = []
    for lam in WAVELENGTHS_SPD:
        power = 0.0
        for i in range(6):
            power += _gaussian(lam, AS7262_WL[i], AS7262_SIGMAS[i], amps[i])
        for i in range(2):
            power += _gaussian(lam, EXTRA_WL[i], EXTRA_SIGMAS[i], amps_extra[i])
        spd.append((float(lam), power))
    return spd


def compute_cie1931_chromaticity(
    violet: float,
    blue: float,
    green: float,
    yellow: float,
    orange: float,
    red: float,
) -> Tuple[float, float, float, float, float]:
    """Integrate the reconstructed SPD with CIE 1931 2-deg observer curves."""
    spd = reconstruct_spd(violet, blue, green, yellow, orange, red)
    x_sum = 0.0
    y_sum = 0.0
    z_sum = 0.0

    for lam, power in spd:
        x_sum += power * _linear_interp(lam, WAVELENGTHS_CIE, X_BAR)
        y_sum += power * _linear_interp(lam, WAVELENGTHS_CIE, Y_BAR)
        z_sum += power * _linear_interp(lam, WAVELENGTHS_CIE, Z_BAR)

    total = x_sum + y_sum + z_sum
    x = (x_sum / total) if total != 0.0 else 0.0
    y = (y_sum / total) if total != 0.0 else 0.0
    return x_sum, y_sum, z_sum, x, y


def compute_cct_mccamy(x: float, y: float) -> float:
    """Compute Correlated Color Temperature (K) using McCamy's cubic formula."""
    denom = y - 0.1858
    if abs(denom) < 1e-9:
        denom = 1e-9 if denom >= 0 else -1e-9
    n = (x - 0.332) / denom
    cct = -449.0 * (n ** 3) + 3525.0 * (n ** 2) - 6823.3 * n + 5520.33
    return max(CCT_MIN, min(CCT_MAX, cct))


def compute_medi(lux: float, cct: float) -> float:
    """Estimate CIE S 026 Melanopic Equivalent Daylight Illuminance (mEDI)."""
    cct_clamped = max(CCT_MIN, min(CCT_MAX, cct))
    mder = (
        4.324e-12 * (cct_clamped ** 3)
        - 8.325e-8 * (cct_clamped ** 2)
        + 6.147e-4 * cct_clamped
        - 0.750
    )
    return max(0.0, lux) * mder


def determine_actuation_state(medi: float) -> Dict[str, object]:
    """Map mEDI to the closed-loop circadian luminaire state and Tuya HSV payload."""
    if EDI_PINK_MIN <= medi < EDI_PINK_MAX:
        return {
            "state": "LOW_STIMULATION_PINK",
            "tuya_commands": [
                {"code": "switch_led", "value": True},
                {"code": "work_mode", "value": "colour"},
                {"code": "colour_data_v2", "value": {"h": 330, "s": 1000, "v": 1000}},
            ],
        }
    if EDI_RED_MIN <= medi < EDI_RED_MAX:
        return {
            "state": "HIGH_STIMULATION_RED",
            "tuya_commands": [
                {"code": "switch_led", "value": True},
                {"code": "work_mode", "value": "colour"},
                {"code": "colour_data_v2", "value": {"h": 0, "s": 1000, "v": 1000}},
            ],
        }
    return {
        "state": "OFF",
        "tuya_commands": [{"code": "switch_led", "value": False}],
    }


@dataclass
class SpectralSample:
    temp: float
    violet: float
    blue: float
    green: float
    yellow: float
    orange: float
    red: float
    lux: float
    label: str
    cie_x: float
    cie_y: float
    cct: float
    medi: float
    actuation_state: str

    def feature_vector(self) -> Tuple[float, ...]:
        total_spec = self.violet + self.blue + self.green + self.yellow + self.orange + self.red
        norm = total_spec if total_spec > 0.0 else 1.0
        return (
            self.violet / norm,
            self.blue / norm,
            self.green / norm,
            self.yellow / norm,
            self.orange / norm,
            self.red / norm,
            self.cie_x,
            self.cie_y,
            self.cct / CCT_MAX,
            math.log1p(max(0.0, self.lux)),
            math.log1p(max(0.0, self.medi)),
        )


def load_spectral_dataset(csv_path: Path) -> List[SpectralSample]:
    """Load AS7262 + BH1750 measurements and enrich each sample with photometric metrics."""
    samples: List[SpectralSample] = []
    with csv_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            violet = float(row["violet"])
            blue = float(row["blue"])
            green = float(row["green"])
            yellow = float(row["yellow"])
            orange = float(row["orange"])
            red = float(row["red"])
            lux = float(row["lux"])
            temp = float(row["temp"])
            label = str(row["label"]).strip()

            _, _, _, cie_x, cie_y = compute_cie1931_chromaticity(
                violet, blue, green, yellow, orange, red
            )
            cct = compute_cct_mccamy(cie_x, cie_y)
            medi = compute_medi(lux, cct)
            actuation = str(determine_actuation_state(medi)["state"])

            samples.append(
                SpectralSample(
                    temp=temp,
                    violet=violet,
                    blue=blue,
                    green=green,
                    yellow=yellow,
                    orange=orange,
                    red=red,
                    lux=lux,
                    label=label,
                    cie_x=cie_x,
                    cie_y=cie_y,
                    cct=cct,
                    medi=medi,
                    actuation_state=actuation,
                )
            )
    return samples


def _standardize(
    train_x: List[Tuple[float, ...]], test_x: List[Tuple[float, ...]]
) -> Tuple[List[Tuple[float, ...]], List[Tuple[float, ...]]]:
    dims = len(train_x[0])
    means = [sum(row[d] for row in train_x) / len(train_x) for d in range(dims)]
    stds = []
    for d in range(dims):
        var = sum((row[d] - means[d]) ** 2 for row in train_x) / max(1, len(train_x))
        stds.append(math.sqrt(var) if var > 1e-12 else 1.0)

    def _apply(rows: List[Tuple[float, ...]]) -> List[Tuple[float, ...]]:
        return [tuple((row[d] - means[d]) / stds[d] for d in range(dims)) for row in rows]

    return _apply(train_x), _apply(test_x)


def _sq_dist(a: Sequence[float], b: Sequence[float]) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b))


def evaluate_stratified_cv(samples: List[SpectralSample], n_folds: int = 5) -> Dict[str, float]:
    """Run stratified k-fold cross-validation with Nearest-Centroid and k-NN (k=3)."""
    by_label: Dict[str, List[SpectralSample]] = {}
    for s in samples:
        by_label.setdefault(s.label, []).append(s)

    folds: List[List[SpectralSample]] = [[] for _ in range(n_folds)]
    for label in sorted(by_label):
        group = by_label[label]
        for idx, item in enumerate(group):
            folds[idx % n_folds].append(item)

    nc_correct = 0
    knn_correct = 0
    total = 0

    for fold_idx in range(n_folds):
        test_set = folds[fold_idx]
        train_set = [item for i, fold in enumerate(folds) if i != fold_idx for item in fold]

        train_vecs_raw = [s.feature_vector() for s in train_set]
        test_vecs_raw = [s.feature_vector() for s in test_set]
        train_vecs, test_vecs = _standardize(train_vecs_raw, test_vecs_raw)
        train_labels = [s.label for s in train_set]

        # Nearest Centroid per class
        centroids: Dict[str, List[float]] = {}
        counts: Dict[str, int] = {}
        dims = len(train_vecs[0])
        for vec, lbl in zip(train_vecs, train_labels):
            if lbl not in centroids:
                centroids[lbl] = [0.0] * dims
                counts[lbl] = 0
            for d in range(dims):
                centroids[lbl][d] += vec[d]
            counts[lbl] += 1
        for lbl, c_vec in centroids.items():
            centroids[lbl] = [v / counts[lbl] for v in c_vec]

        for vec, sample in zip(test_vecs, test_set):
            pred_nc = min(centroids, key=lambda lbl: _sq_dist(vec, centroids[lbl]))
            if pred_nc == sample.label:
                nc_correct += 1

            dists = sorted(
                ((_sq_dist(vec, tr_v), tr_lbl) for tr_v, tr_lbl in zip(train_vecs, train_labels)),
                key=lambda pair: pair[0],
            )[:3]
            votes: Dict[str, int] = {}
            for _, lbl in dists:
                votes[lbl] = votes.get(lbl, 0) + 1
            pred_knn = max(votes, key=lambda lbl: votes[lbl])
            if pred_knn == sample.label:
                knn_correct += 1

            total += 1

    return {
        "total_samples": float(total),
        "n_folds": float(n_folds),
        "nearest_centroid_accuracy": nc_correct / max(1, total),
        "knn3_accuracy": knn_correct / max(1, total),
    }


def summarize_dataset(samples: List[SpectralSample]) -> Dict[str, object]:
    """Aggregate per-class photometric and actuation statistics plus CV metrics."""
    by_label: Dict[str, List[SpectralSample]] = {}
    for s in samples:
        by_label.setdefault(s.label, []).append(s)

    classes = []
    for label in sorted(by_label, key=lambda x: (int(x) if x.isdigit() else 999, x)):
        group = by_label[label]
        n = len(group)
        mean_lux = sum(s.lux for s in group) / n
        mean_cct = sum(s.cct for s in group) / n
        mean_x = sum(s.cie_x for s in group) / n
        mean_y = sum(s.cie_y for s in group) / n
        mean_medi = sum(s.medi for s in group) / n
        state_counts: Dict[str, int] = {}
        for s in group:
            state_counts[s.actuation_state] = state_counts.get(s.actuation_state, 0) + 1
        dominant_state = max(state_counts, key=lambda k: state_counts[k])
        classes.append(
            {
                "label": label,
                "samples": n,
                "mean_lux": round(mean_lux, 2),
                "mean_cie_x": round(mean_x, 4),
                "mean_cie_y": round(mean_y, 4),
                "mean_cct_k": round(mean_cct, 1),
                "mean_medi": round(mean_medi, 2),
                "dominant_actuation": dominant_state,
            }
        )

    cv_metrics = evaluate_stratified_cv(samples, n_folds=5)
    return {
        "dataset_samples": len(samples),
        "num_regimes": len(classes),
        "cross_validation": cv_metrics,
        "regimes": classes,
    }


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    csv_path = repo_root / "hardware" / "python" / "data" / "mediciones.csv"
    out_path = repo_root / "hardware" / "python" / "data" / "photometric_summary.json"

    samples = load_spectral_dataset(csv_path)
    summary = summarize_dataset(samples)
    out_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
