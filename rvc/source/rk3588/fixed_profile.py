"""Fixed tensor dimensions for the RK3588 RVC v2 32/40 kHz profiles."""

SUPPORTED_MODEL_SAMPLE_RATES = (32_000, 40_000)
DEFAULT_MODEL_SAMPLE_RATE = 32_000
MODEL_SAMPLE_RATE = DEFAULT_MODEL_SAMPLE_RATE
CONTENT_SAMPLE_RATE = 16_000

CONTEXT_MS = 50
BLOCK_MS = 50
CROSSFADE_MS = 20
SOLA_SEARCH_MS = 10

CONTENT_INPUT_SAMPLES = 2_080
BLOCK_INPUT_SAMPLES = 800
RMVPE_AUDIO_SAMPLES = 4_960

CONTENT_FRAMES = 13
SKIP_HEAD_FRAMES = 5
RETURN_FRAMES = 8
CONTENT_FEATURES = 768
LATENT_CHANNELS = 192

RMVPE_MELS = 128
RMVPE_FRAMES = 32
RMVPE_BINS = 360


def normalize_model_sample_rate(value: int | str) -> int:
    """Return a supported integer sample rate from an RVC rate value."""

    aliases = {
        32_000: 32_000,
        40_000: 40_000,
        "32k": 32_000,
        "40k": 40_000,
        "32000": 32_000,
        "40000": 40_000,
    }
    try:
        sample_rate = aliases[value]
    except (KeyError, TypeError) as error:
        raise ValueError(f"unsupported model sample rate: {value!r}") from error
    return sample_rate


def model_sample_rate_label(sample_rate: int | str) -> str:
    normalized = normalize_model_sample_rate(sample_rate)
    return f"{normalized // 1000}k"


def generator_output_samples(sample_rate: int | str) -> int:
    normalized = normalize_model_sample_rate(sample_rate)
    return RETURN_FRAMES * normalized // 100


def emitted_samples(sample_rate: int | str) -> int:
    normalized = normalize_model_sample_rate(sample_rate)
    return BLOCK_MS * normalized // 1000


def crossfade_samples(sample_rate: int | str) -> int:
    normalized = normalize_model_sample_rate(sample_rate)
    return CROSSFADE_MS * normalized // 1000


def sola_search_samples(sample_rate: int | str) -> int:
    normalized = normalize_model_sample_rate(sample_rate)
    return SOLA_SEARCH_MS * normalized // 1000


# Backward-compatible aliases for the original 32 kHz profile.
OUTPUT_SAMPLES = generator_output_samples(DEFAULT_MODEL_SAMPLE_RATE)
EMITTED_SAMPLES = emitted_samples(DEFAULT_MODEL_SAMPLE_RATE)
