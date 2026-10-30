#!/usr/bin/env python3
"""Unit checks for the fixed RVC 32/40 kHz shape contract."""

import unittest

from fixed_profile import (
    crossfade_samples,
    emitted_samples,
    generator_output_samples,
    model_sample_rate_label,
    normalize_model_sample_rate,
    sola_search_samples,
)


class FixedProfileTest(unittest.TestCase):
    def test_32k_contract(self) -> None:
        self.assertEqual(normalize_model_sample_rate("32k"), 32_000)
        self.assertEqual(model_sample_rate_label(32_000), "32k")
        self.assertEqual(generator_output_samples(32_000), 2_560)
        self.assertEqual(emitted_samples(32_000), 1_600)
        self.assertEqual(crossfade_samples(32_000), 640)
        self.assertEqual(sola_search_samples(32_000), 320)

    def test_40k_contract(self) -> None:
        self.assertEqual(normalize_model_sample_rate("40k"), 40_000)
        self.assertEqual(model_sample_rate_label(40_000), "40k")
        self.assertEqual(generator_output_samples(40_000), 3_200)
        self.assertEqual(emitted_samples(40_000), 2_000)
        self.assertEqual(crossfade_samples(40_000), 800)
        self.assertEqual(sola_search_samples(40_000), 400)

    def test_unsupported_rate_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            normalize_model_sample_rate("48k")


if __name__ == "__main__":
    unittest.main()
