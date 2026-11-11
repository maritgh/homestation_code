#pragma once

// Voer scherm- en sensoroverdrachten na elkaar uit, inclusief het voorbereiden van drivertransacties.
void shared_i2c_acquire(void);
void shared_i2c_release(void);
