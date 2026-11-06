#include "i2c_shared.h"

#include <sys/lock.h>

static _lock_t bus_lock;

void shared_i2c_acquire(void) { _lock_acquire(&bus_lock); }

void shared_i2c_release(void) { _lock_release(&bus_lock); }
