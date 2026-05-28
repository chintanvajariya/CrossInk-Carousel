#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
void disableWiFiBeforeDeepSleep() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (wifiMode == WIFI_MODE_NULL) {
    return;
  }

  LOG_DBG("PWR", "Disabling WiFi before deep sleep (mode=%d)", static_cast<int>(wifiMode));
  if (wifiMode & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (wifiMode & WIFI_MODE_STA) {
    WiFi.disconnect(false);
  }
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
}
}  // namespace

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  disableWiFiBeforeDeepSleep();

  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t rawSoc = (hi << 8) | lo;
    uint16_t newSoc = rawSoc > 100 ? 100 : rawSoc;

    // Rate-limit + direction-monotonicity to mitigate BQ27220 fuel gauge
    // miscalibration. The X3's chip is known to snap RSOC to 100% mid-charge
    // when its FCC estimate is wrong (see crosspoint-reader issues #1444,
    // #1963 and the TI E2E discussion on bq27220 FCC RAM behavior). Without
    // smoothing, users see jumps like 27% → 100% in seconds while charging.
    //
    // Strategy:
    //   - While charging (USB connected): only accept INCREASES, capped at
    //     MAX_DELTA_PER_POLL per 1.5s poll. A genuine 0-100% charge over
    //     ~90 min stays accurate (2%/poll × 50 polls ≈ 100% over 75s of
    //     poll budget, but charging real-world is throttled by current).
    //   - While discharging: only accept DECREASES, capped at the same rate.
    //     Filters chip-side jitter while still reflecting genuine drain.
    //
    // First read is accepted verbatim so the initial display reflects the
    // chip's reading on boot. `_batterySocInitialized` distinguishes "first
    // poll ever" from "real reading of 0%."
    if (_batterySocInitialized) {
      constexpr int16_t MAX_DELTA_PER_POLL = 2;
      const bool charging = gpio.isUsbConnected();
      const int16_t cached = static_cast<int16_t>(_batteryCachedPercent);
      int16_t adjusted = static_cast<int16_t>(newSoc);
      const int16_t delta = adjusted - cached;
      if (charging) {
        if (delta < 0) {
          adjusted = cached;  // ignore decreases during charging
        } else if (delta > MAX_DELTA_PER_POLL) {
          adjusted = cached + MAX_DELTA_PER_POLL;
        }
      } else {
        if (delta > 0) {
          adjusted = cached;  // ignore increases during discharge
        } else if (delta < -MAX_DELTA_PER_POLL) {
          adjusted = cached - MAX_DELTA_PER_POLL;
        }
      }
      newSoc = static_cast<uint16_t>(adjusted);
    }
    _batteryCachedPercent = newSoc;
    _batterySocInitialized = true;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
