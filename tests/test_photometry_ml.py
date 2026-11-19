import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "hardware" / "python"))

import circadian_ml_pipeline as pipe  # noqa: E402


class TestPhotometryAndML(unittest.TestCase):
    def test_spd_positivity_and_peak_alignment(self):
        # Pure green stimulus should produce non-negative SPD peaking near 550 nm
        spd = pipe.reconstruct_spd(0, 0, 5000, 0, 0, 0)
        self.assertEqual(len(spd), 301)
        self.assertTrue(all(power >= 0.0 for _, power in spd))
        peak_wl, _ = max(spd, key=lambda pair: pair[1])
        self.assertLessEqual(abs(peak_wl - 550.0), 10.0)

    def test_cie1931_chromaticity_normalization(self):
        x_sum, y_sum, z_sum, x, y = pipe.compute_cie1931_chromaticity(
            2409, 395, 3294, 3416, 17061, 3658
        )
        total = x_sum + y_sum + z_sum
        self.assertGreater(total, 0.0)
        z = z_sum / total
        self.assertAlmostEqual(x + y + z, 1.0, places=6)
        self.assertGreater(x, 0.0)
        self.assertGreater(y, 0.0)

    def test_mccamy_cct_bounds(self):
        self.assertEqual(pipe.compute_cct_mccamy(0.60, 0.36), 2000.0)
        self.assertEqual(pipe.compute_cct_mccamy(0.22, 0.25), 10000.0)

    def test_medi_monotonic_with_lux_and_cct(self):
        low_lux = pipe.compute_medi(100.0, 5000.0)
        high_lux = pipe.compute_medi(300.0, 5000.0)
        self.assertGreater(high_lux, low_lux)

        warm_medi = pipe.compute_medi(200.0, 2700.0)
        cool_medi = pipe.compute_medi(200.0, 6500.0)
        self.assertGreater(cool_medi, warm_medi)

    def test_actuation_regimes(self):
        self.assertEqual(pipe.determine_actuation_state(5.0)["state"], "LOW_STIMULATION_PINK")
        self.assertEqual(pipe.determine_actuation_state(15.0)["state"], "HIGH_STIMULATION_RED")
        self.assertEqual(pipe.determine_actuation_state(30.0)["state"], "OFF")

    def test_dataset_cross_validation_exceeds_90_percent(self):
        csv_path = REPO_ROOT / "hardware" / "python" / "data" / "mediciones.csv"
        samples = pipe.load_spectral_dataset(csv_path)
        self.assertEqual(len(samples), 159)
        metrics = pipe.evaluate_stratified_cv(samples, n_folds=5)
        self.assertGreater(metrics["nearest_centroid_accuracy"], 0.90)
        self.assertGreater(metrics["knn3_accuracy"], 0.90)


if __name__ == "__main__":
    unittest.main()
