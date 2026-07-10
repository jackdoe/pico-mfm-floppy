#include "flux_noise.h"
#include <limits.h>
#include <string.h>

static uint32_t flux_noise_random(flux_noise_t *noise) {
  uint32_t value = noise->state;
  value ^= value << 13u;
  value ^= value >> 17u;
  value ^= value << 5u;
  noise->state = value;
  return value;
}

static int32_t flux_noise_signed(flux_noise_t *noise, int32_t limit) {
  if (limit == 0) return 0;
  uint32_t span = (uint32_t)limit * 2u + 1u;
  return (int32_t)(flux_noise_random(noise) % span) - limit;
}

bool flux_noise_configure(flux_noise_t *noise,
                          const flux_noise_config_t *config) {
  if (!noise || !config || config->jitter_ticks < 0 ||
      config->jitter_ticks > UINT16_MAX ||
      config->drift_ppm <= -(int32_t)FLUX_NOISE_RATE_SCALE ||
      config->drift_ppm > (int32_t)FLUX_NOISE_RATE_SCALE ||
      config->wander_step_ppm < 0 || config->wander_limit_ppm < 0 ||
      config->wander_limit_ppm > (int32_t)FLUX_NOISE_RATE_SCALE ||
      config->wander_step_ppm > config->wander_limit_ppm ||
      (config->wander_step_ppm != 0 && config->wander_period == 0) ||
      config->impulse_rate_ppm > FLUX_NOISE_RATE_SCALE ||
      config->impulse_ticks < 0 || config->impulse_ticks > UINT16_MAX) {
    return false;
  }
  int64_t minimum_scale = (int64_t)FLUX_NOISE_RATE_SCALE +
      config->drift_ppm - config->wander_limit_ppm;
  int64_t maximum_scale = (int64_t)FLUX_NOISE_RATE_SCALE +
      config->drift_ppm + config->wander_limit_ppm;
  if (minimum_scale <= 0 || maximum_scale > INT32_MAX) return false;
  memset(noise, 0, sizeof(*noise));
  noise->config = *config;
  noise->state = config->seed ? config->seed : UINT32_C(0x6D2B79F5);
  return true;
}

uint16_t flux_noise_apply(flux_noise_t *noise, uint16_t delta) {
  if (!noise) return delta;
  const flux_noise_config_t *config = &noise->config;
  if (config->wander_step_ppm != 0) {
    noise->wander_position++;
    if (noise->wander_position >= config->wander_period) {
      noise->wander_position = 0;
      int64_t wander = (int64_t)noise->wander_ppm +
          flux_noise_signed(noise, config->wander_step_ppm);
      if (wander < -config->wander_limit_ppm) {
        wander = -config->wander_limit_ppm;
      } else if (wander > config->wander_limit_ppm) {
        wander = config->wander_limit_ppm;
      }
      noise->wander_ppm = (int32_t)wander;
    }
  }

  int64_t scale = (int64_t)FLUX_NOISE_RATE_SCALE + config->drift_ppm +
      noise->wander_ppm;
  int64_t adjusted = ((int64_t)delta * scale +
                      FLUX_NOISE_RATE_SCALE / 2u) /
      FLUX_NOISE_RATE_SCALE;
  adjusted += flux_noise_signed(noise, config->jitter_ticks);

  if (config->impulse_rate_ppm != 0 && config->impulse_ticks != 0 &&
      flux_noise_random(noise) % FLUX_NOISE_RATE_SCALE <
          config->impulse_rate_ppm) {
    int32_t impulse = flux_noise_signed(noise, config->impulse_ticks);
    if (impulse == 0) {
      impulse = (noise->state & 1u) != 0
          ? config->impulse_ticks
          : -config->impulse_ticks;
    }
    adjusted += impulse;
  }

  if (adjusted < 1) adjusted = 1;
  if (adjusted > UINT16_MAX) adjusted = UINT16_MAX;
  return (uint16_t)adjusted;
}
