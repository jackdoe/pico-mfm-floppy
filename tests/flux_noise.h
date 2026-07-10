#ifndef FLUX_NOISE_H
#define FLUX_NOISE_H

#include <stdbool.h>
#include <stdint.h>

#define FLUX_NOISE_RATE_SCALE 1000000u

typedef struct {
  uint32_t seed;
  int32_t jitter_ticks;
  int32_t drift_ppm;
  int32_t wander_step_ppm;
  int32_t wander_limit_ppm;
  uint32_t wander_period;
  uint32_t impulse_rate_ppm;
  int32_t impulse_ticks;
} flux_noise_config_t;

typedef struct {
  flux_noise_config_t config;
  uint32_t state;
  uint32_t wander_position;
  int32_t wander_ppm;
} flux_noise_t;

bool flux_noise_configure(flux_noise_t *noise,
                          const flux_noise_config_t *config);
uint16_t flux_noise_apply(flux_noise_t *noise, uint16_t delta);

#endif
