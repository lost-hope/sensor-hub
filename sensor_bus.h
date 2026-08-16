#pragma once

/*
 * Sensor Bus - shared interface for the Sensor Hub usermod family.
 *
 * This header defines the standardized way "sensor provider" usermods
 * (temperature, humidity, motion, contact, ...) push their readings into
 * the central "Sensor Hub" usermod (see usermod_sensor_hub.cpp), which is
 * the only thing that talks to MQTT, the JSON API and the Info tab.
 *
 * A provider usermod's job is only to read hardware and call the methods
 * below - it never builds MQTT topics, HA discovery payloads or JSON
 * itself. This keeps sensor drivers small and lets the hub apply one
 * consistent policy (topics, retained state, HA discovery, rate limiting)
 * to every sensor in the system, no matter which usermod produced it.
 *
 * Include this header *after* "wled.h" (it relies on UsermodManager and
 * Usermod already being declared).
 *
 * --- Using the bus from a provider usermod -------------------------------
 *
 *   #include "wled.h"
 *   #include "sensor_bus.h"
 *
 *   class MyTempSensor : public Usermod {
 *     uint8_t handle = SENSOR_HANDLE_INVALID;
 *     SensorHub* hub = nullptr;
 *
 *     void setup() override {}
 *
 *     void loop() override {
 *       if (!hub) hub = getSensorHub();       // lazy lookup, hub may load after us
 *       if (!hub) return;                     // Sensor Hub usermod not present in this build
 *       if (handle == SENSOR_HANDLE_INVALID) {
 *         handle = hub->registerSensor("kitchen_temperature", SensorType::Temperature);
 *       }
 *       // ... read your sensor on its own schedule, then:
 *       hub->updateSensor(handle, myReading);
 *     }
 *   };
 *
 * The hub takes care of publishing "kitchen_temperature" to MQTT (with HA
 * discovery), /json/state and the Info tab. See examples/demo_sensor_provider.cpp
 * for a complete, compilable example (incl. a binary/motion sensor).
 *
 * --- Using the bus from a *consumer* usermod ------------------------------
 *
 * Any other usermod that wants a sensor reading to react to (e.g. an
 * animation that speeds up with temperature) can ask the hub for a value by
 * SensorType instead of tracking a specific provider/name - the hub picks
 * which registered sensor answers (see getValue() below):
 *
 *   void loop() override {
 *     SensorHub* hub = getSensorHub();
 *     if (!hub) return;
 *     float temperature;
 *     if (hub->getValue(SensorType::Temperature, temperature)) {
 *       // ... use temperature, regardless of whether it came from a DHT,
 *       // SHTC3, BME280, ...
 *     }
 *   }
 */

#include <Arduino.h>

// Unique WLED usermod ID for the Sensor Hub. Not part of the official WLED
// usermod ID list (wled00/const.h) since this is an out-of-tree usermod. If
// 199 is already used by another usermod in your build, override it with a
// build flag, e.g. -D USERMOD_ID_SENSOR_HUB=210 (define it identically for
// every usermod that includes this header).
#ifndef USERMOD_ID_SENSOR_HUB
#define USERMOD_ID_SENSOR_HUB 199
#endif

// Maximum number of sensors the hub can hold at once. Override with a build
// flag if you have more sensors than this (each slot costs ~70 bytes RAM).
#ifndef SENSOR_HUB_MAX_SENSORS
#define SENSOR_HUB_MAX_SENSORS 16
#endif

// Returned by registerSensor() when registration fails (hub full, no name,
// or duplicate name) and used to mark a handle as "not registered".
#define SENSOR_HANDLE_INVALID 0xFF

// The kind of physical quantity a sensor reports. Selects the default unit,
// decimal precision and Home Assistant device_class used when none are
// given explicitly in registerSensor(). Motion/Contact/GenericBinary are
// exposed as MQTT "ON"/"OFF" and a HA binary_sensor instead of a number.
enum class SensorType : uint8_t {
  Temperature = 0,  // deg C, HA device_class "temperature"
  Humidity,         // %, HA device_class "humidity"
  Pressure,         // hPa, HA device_class "pressure"
  Illuminance,      // lx, HA device_class "illuminance"
  VoltageV,         // V, HA device_class "voltage"
  Battery,          // %, HA device_class "battery"
  Co2,              // ppm, HA device_class "carbon_dioxide"
  Acceleration,     // m/s^2, no standard HA device_class (plain numeric sensor)
  Distance,         // mm, HA device_class "distance"
  Current,          // A, HA device_class "current"
  Power,            // W, HA device_class "power"
  Motion,           // binary, HA device_class "motion"
  Contact,          // binary, HA device_class "door"
  Generic,          // numeric value; pass unit/deviceClass explicitly
  GenericBinary     // on/off value; pass deviceClass explicitly (optional)
};

inline bool sensorTypeIsBinary(SensorType t) {
  return t == SensorType::Motion || t == SensorType::Contact || t == SensorType::GenericBinary;
}

// Abstract interface implemented by the Sensor Hub usermod. It extends
// Usermod (rather than standing alone) so that a plain Usermod* obtained
// from UsermodManager::lookup() can be safely downcast to it with
// static_cast - the two classes must share a single-inheritance chain for
// that to be well-defined, and multiple inheritance from two unrelated
// bases would not be castable to like this. Obtain a pointer to it via
// getSensorHub() (below) rather than doing the lookup/cast yourself.
class SensorHub : public Usermod {
  public:
    // Registers a new sensor and returns a handle for future updates, or
    // SENSOR_HANDLE_INVALID if the hub is full or the name is already used.
    //
    // 'name' is used as-is for the MQTT topic segment, HA object_id and JSON
    // key - keep it short, lowercase, and use '_' instead of spaces/slashes
    // (e.g. "kitchen_motion"). The hub copies it internally, so a stack
    // buffer is fine.
    // 'unit' is only honored for SensorType::Generic and
    // SensorType::GenericBinary. For every standard type the hub always
    // stores values in that type's fixed canonical SI unit (e.g. °C for
    // Temperature, mm for Distance - see defaultUnit() in
    // usermod_sensor_hub.cpp) and silently ignores any override here - this
    // guarantees every sensor of a given SensorType is directly comparable
    // (notably for getValue()/getValueBinary()) no matter which provider
    // registered it. If your hardware/library natively reports something
    // else (e.g. cm), convert to the canonical unit yourself before calling
    // updateSensor(). Users who want a different *display* unit configure
    // that once, centrally, on the Sensor Hub itself (e.g. °F, inHg, cm/m/in)
    // - individual providers never need to know about that.
    // 'deviceClass' is only consulted for SensorType::Generic and
    // SensorType::GenericBinary - pass nullptr for standard types to use
    // their built-in default.
    // 'precision' is the number of decimal places shown/published for
    // numeric sensors (ignored for binary types); clamped to 6.
    // 'priority' only matters if another sensor of the same SensorType is
    // also registered: it decides which one getValue()/getValueBinary()
    // hand out (lower wins; ties go to whichever registered first). Leave
    // at the default unless you specifically want to prefer/deprefer this
    // sensor over another of the same type (e.g. an I2C sensor over a
    // cheaper GPIO one for the same physical quantity).
    virtual uint8_t registerSensor(const char* name, SensorType type,
                                    const char* unit = nullptr,
                                    const char* deviceClass = nullptr,
                                    uint8_t precision = 1,
                                    uint8_t priority = 100) = 0;

    // Removes a previously registered sensor, e.g. when hardware is no
    // longer present. Publishes "offline" and removes its HA discovery
    // entry. The handle must not be used again afterwards.
    virtual void unregisterSensor(uint8_t handle) = 0;

    // Pushes a new numeric reading. No-op for binary sensor types or an
    // invalid/unknown handle. Call this whenever you have a fresh reading -
    // the hub decides when/how often to actually publish it.
    virtual void updateSensor(uint8_t handle, float value) = 0;

    // Pushes a new binary reading. No-op for numeric sensor types or an
    // invalid/unknown handle.
    virtual void updateSensorBinary(uint8_t handle, bool value) = 0;

    // Marks a sensor as (un)available, e.g. after repeated read failures or
    // when hardware is temporarily disconnected. Surfaces as the entity
    // going "unavailable" in Home Assistant. Defaults to available=true
    // when a sensor is registered.
    virtual void setSensorAvailable(uint8_t handle, bool available) = 0;

    // ---- Consumer API: pull a value by quantity, not by name/handle ------
    //
    // Lets a *consumer* usermod ask "give me the temperature" without caring
    // which provider (dht, shtc3, bme280, ...) - or how many of them - are
    // actually registered. If several sensors share the same SensorType,
    // the hub answers with whichever one is currently available() and
    // valid() (has received a reading) with the lowest registerSensor()
    // 'priority'; ties go to whichever registered first. Returns false (and
    // leaves 'value' untouched) if no matching sensor currently qualifies.
    //
    // Deliberately not handle-based: which physical sensor answers a given
    // type can change at runtime (e.g. a higher-priority sensor going
    // offline), so call this fresh whenever you need a reading rather than
    // caching a handle across loop() calls.
    virtual bool getValue(SensorType type, float& value) = 0;
    virtual bool getValueBinary(SensorType type, bool& value) = 0;

    // Same selection as getValue()/getValueBinary(), but returns the
    // registered name of the sensor that would answer (or currently
    // answers) that type - handy for logging/UI ("temperature from
    // 'kitchen_bme280'"). Returns nullptr if none currently qualify.
    virtual const char* getValueSourceName(SensorType type) = 0;
};

// Looks up the Sensor Hub usermod. Returns nullptr if it isn't present in
// this build (e.g. forgot to add it to custom_usermods) - always check
// before use. Cheap enough to call every loop(), but caching the result
// after the first non-null lookup is recommended.
static inline SensorHub* getSensorHub() {
  Usermod* mod = UsermodManager::lookup(USERMOD_ID_SENSOR_HUB);
  return mod ? static_cast<SensorHub*>(mod) : nullptr; // safe: SensorHub derives from Usermod
}
