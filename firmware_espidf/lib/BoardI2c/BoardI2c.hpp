#pragma once
#if defined(BOARD_CUSTOM)
#include <driver/i2c_master.h>

/**
 * The custom PCB's (Waveshare ESP32-P4-WIFI6-Touch-LCD-4B) I2C bus: port 1, SDA GPIO 7, SCL GPIO 8. On it
 * are the GT911 touch controller (0x5D or 0x14, DsiDisplay), the ES7210 microphone ADC (0x40,
 * VoiceCapture) and the ES8311 playback codec (0x18).
 *
 * Created once, by the first caller, and shared: each user adds its own devices to it. The i2c_master
 * driver holds the bus for a whole transaction, so the users' transactions never interleave; one waits for
 * the one before it to end (a register read or write takes well under a millisecond).
 */
class BoardI2c {
public:
  // Any task: the bus, or nullptr if it could not be created (logged).
  static i2c_master_bus_handle_t bus();
};
#endif
