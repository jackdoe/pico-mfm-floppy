#ifndef STUB_HARDWARE_SYNC_H
#define STUB_HARDWARE_SYNC_H

#include <stdint.h>
#include <stdbool.h>

typedef unsigned int uint;
typedef struct { uint32_t value; } spin_lock_t;

static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t status) { (void)status; }

int spin_lock_claim_unused(bool required);
void spin_lock_unclaim(uint lock_num);
spin_lock_t *spin_lock_instance(uint lock_num);
uint32_t spin_lock_blocking(spin_lock_t *lock);
void spin_unlock(spin_lock_t *lock, uint32_t status);

#endif
