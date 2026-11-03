#pragma once

// Serialize display and sensor transfers, including driver transaction setup.
void shared_i2c_acquire(void);
void shared_i2c_release(void);
