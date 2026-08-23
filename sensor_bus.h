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
 * --- Sensor identity: compile-time slots, runtime attachment -------------
 *
 * Every sensor a provider will ever expose is declared once, at file
 * scope, via REGISTER_SENSOR_SLOT() - this places a small constant
 * descriptor (quantity + name suffix + default precision/priority) into a
 * WLED "dynarray" (see dynarray.h; the same linker-section mechanism
 * REGISTER_USERMOD already uses), so the hub's sensor table is sized
 * exactly to however many slots are actually linked in across every
 * provider usermod in your build - no fixed cap, nothing to raise via a
 * build flag as you add sensors.
 *
 * A slot's *name* is only half-known at compile time: the suffix (e.g.
 * "_accel_x") is fixed in the provider's source, but the prefix is a
 * runtime-configurable Settings field (so e.g. two DHT providers in one
 * build can be told apart as "kitchen"/"bedroom"). Likewise a slot's
 * precision/priority are provider Settings, editable live - the
 * descriptor only carries their compile-time defaults. So a provider still
 * calls into the hub at runtime, once per sensor, via attachSensor():
 * this is not "register a sensor" (the slot already reserves that) but
 * "here is this slot's current prefix/precision/priority" - typically once
 * per loop() call, guarded the same idempotent way registerSensor() used
 * to be.
 *
 * --- Using the bus from a provider usermod -------------------------------
 *
 *   #include "wled.h"
 *   #include "sensor_bus.h"
 *
 *   // File scope, outside any class - one slot per sensor this provider
 *   // will ever attach. Precision/priority here are just the compile-time
 *   // defaults; a provider's own Settings still control the live values
 *   // passed to attachSensor() below.
 *   REGISTER_SENSOR_SLOT(_slotTemp, "_temperature", SensorTypes::Temperature, 1, 100);
 *
 *   class MyTempSensor : public Usermod {
 *     uint8_t handle = SENSOR_HANDLE_INVALID;
 *     SensorHub* hub = nullptr;
 *     String namePrefix = "kitchen";
 *     uint8_t precision = 1, priority = 100;
 *
 *     void setup() override {}
 *
 *     void loop() override {
 *       if (!hub) hub = getSensorHub();       // lazy lookup, hub may load after us
 *       if (!hub) return;                     // Sensor Hub usermod not present in this build
 *       if (handle == SENSOR_HANDLE_INVALID) {
 *         handle = hub->attachSensor(&_slotTemp, namePrefix.c_str(), precision, priority);
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
 * A sensor that doesn't fit one of the standard SensorTypes below (e.g. raw
 * gas resistance) isn't a special case - just declare your own
 * SensorTypeInfo (own id/unit/device_class) next to your slot, local to
 * your provider file. That's the whole "open set" property: no shared
 * "Generic" bucket to collide in, no central header to edit.
 *
 * --- Using the bus from a *consumer* usermod ------------------------------
 *
 * Any other usermod that wants a sensor reading to react to (e.g. an
 * animation that speeds up with temperature) can ask the hub for a value by
 * quantity instead of tracking a specific provider/name - the hub picks
 * which registered sensor answers (see getValue() below):
 *
 *   void loop() override {
 *     SensorHub* hub = getSensorHub();
 *     if (!hub) return;
 *     float temperature;
 *     if (hub->getValue(SensorTypes::Temperature, temperature)) {
 *       // ... use temperature, regardless of whether it came from a DHT,
 *       // SHTC3, BME280, ...
 *     }
 *   }
 */

#include <Arduino.h>
#include "dynarray.h"

// Unique WLED usermod ID for the Sensor Hub. Not part of the official WLED
// usermod ID list (wled00/const.h) since this is an out-of-tree usermod. If
// 199 is already used by another usermod in your build, override it with a
// build flag, e.g. -D USERMOD_ID_SENSOR_HUB=210 (define it identically for
// every usermod that includes this header).
#ifndef USERMOD_ID_SENSOR_HUB
#define USERMOD_ID_SENSOR_HUB 199
#endif

// Returned by attachSensor() when attachment fails (unknown slot, no name,
// or a duplicate resulting name) and used to mark a handle as "not
// attached".
#define SENSOR_HANDLE_INVALID 0xFF

// Identifies a kind of physical quantity by an open-ended string id instead
// of a fixed enum - any provider can define its own SensorTypeInfo for a
// quantity that isn't one of the standard ones below (e.g. soil moisture),
// with no central header edit required. Two sensors are "the same type"
// (for getValue()/bestSensorOfType() purposes) purely by comparing 'id' as
// a string, not by object identity - this is what lets independently
// written providers agree on a shared quantity just by agreeing on its id.
// 'isBinary' marks quantities exposed as MQTT "ON"/"OFF" and a HA
// binary_sensor instead of a number (e.g. Motion/Contact).
struct SensorTypeInfo {
  const char* id;
  const char* unit;          // nullptr if the quantity has none (e.g. binary types)
  const char* haDeviceClass; // nullptr if there's no standard HA device_class
  bool isBinary;
};

inline bool sensorTypeIsBinary(const SensorTypeInfo& t) { return t.isBinary; }

// Standard quantities, provided so common providers don't have to invent
// their own id/unit/device_class - each is a header-only 'constexpr'
// (internal linkage per-TU, same as any const/constexpr namespace-scope
// variable - no 'inline' needed since this targets C++11, and no separate
// out-of-line definition exists to keep either way), so a build that never
// references e.g. SensorTypes::Co2 doesn't pay for it. Two
// sensors of the same standard type are always directly comparable
// (matching units), since there's no per-slot override here - a sensor
// that needs a different unit just isn't this type (declare your own
// SensorTypeInfo instead, see above).
namespace SensorTypes {
  constexpr SensorTypeInfo Temperature {"temperature",  "°C",   "temperature",      false};
  constexpr SensorTypeInfo Humidity    {"humidity",     "%",    "humidity",         false};
  constexpr SensorTypeInfo Pressure    {"pressure",     "hPa",  "pressure",         false};
  constexpr SensorTypeInfo Illuminance {"illuminance",  "lx",   "illuminance",      false};
  constexpr SensorTypeInfo VoltageV    {"voltage",      "V",    "voltage",          false};
  constexpr SensorTypeInfo Battery     {"battery",      "%",    "battery",          false};
  constexpr SensorTypeInfo Co2         {"co2",          "ppm",  "carbon_dioxide",   false};
  constexpr SensorTypeInfo Acceleration{"acceleration", "m/s²", nullptr,            false}; // no standard HA device_class
  constexpr SensorTypeInfo Distance    {"distance",     "mm",   "distance",         false};
  constexpr SensorTypeInfo Current     {"current",      "A",    "current",          false};
  constexpr SensorTypeInfo Power       {"power",        "W",    "power",            false};
  constexpr SensorTypeInfo Motion      {"motion",       nullptr,"motion",           true};
  constexpr SensorTypeInfo Contact     {"contact",      nullptr,"door",              true};
}

// A compile-time-constant description of one sensor a provider will ever
// attach - reserved via REGISTER_SENSOR_SLOT() at file scope (see the
// usage example above), never constructed at runtime. Holds only what's
// genuinely fixed in every provider's source today; the *name*'s prefix
// and the live precision/priority remain runtime Settings, supplied when
// the provider calls attachSensor().
struct SensorSlotDescriptor {
  const SensorTypeInfo* type;
  const char* nameSuffix;   // e.g. "_accel_x"; full sensor name = namePrefix + nameSuffix
  uint8_t defaultPrecision; // fallback shown before a provider has attached; providers normally pass their own live value to attachSensor()
  uint8_t defaultPriority;  // ditto
};

// The dynarray backing every provider's REGISTER_SENSOR_SLOT() calls.
// DECLARE_DYNARRAY itself must appear in exactly one translation unit (see
// dynarray.h) - that's usermod_sensor_hub.cpp, not here; every provider
// file only ever *adds* a member via the macro below.
#define REGISTER_SENSOR_SLOT(varName, nameSuffix, typeInfo, precision, priority) \
  DYNARRAY_MEMBER(SensorSlotDescriptor, sensorSlots, varName, 1) = \
    { &(typeInfo), (nameSuffix), (precision), (priority) }

// Abstract interface implemented by the Sensor Hub usermod. It extends
// Usermod (rather than standing alone) so that a plain Usermod* obtained
// from UsermodManager::lookup() can be safely downcast to it with
// static_cast - the two classes must share a single-inheritance chain for
// that to be well-defined, and multiple inheritance from two unrelated
// bases would not be castable to like this. Obtain a pointer to it via
// getSensorHub() (below) rather than doing the lookup/cast yourself.
class SensorHub : public Usermod {
  public:
    // Attaches to a previously-declared slot (see REGISTER_SENSOR_SLOT
    // above) and returns a handle for future updates, or
    // SENSOR_HANDLE_INVALID if the slot pointer is unknown to this hub or
    // the resulting name (namePrefix + slot's nameSuffix) is already used
    // by another sensor.
    //
    // The resulting name is used as-is for the MQTT topic segment, HA
    // object_id and JSON key - keep namePrefix short, lowercase, and use
    // '_' instead of spaces/slashes (e.g. "kitchen"). The hub copies the
    // computed name internally, so a stack buffer/temporary is fine.
    // 'precision' is the number of decimal places shown/published for
    // numeric sensors (ignored for binary types); clamped to 6.
    // 'priority' only matters if another sensor of the same quantity is
    // also attached: it decides which one getValue()/getValueBinary() hand
    // out (lower wins; ties go to whichever attached first). Leave at the
    // slot's default unless you specifically want to prefer/deprefer this
    // sensor over another of the same quantity (e.g. an I2C sensor over a
    // cheaper GPIO one for the same physical quantity).
    virtual uint8_t attachSensor(const SensorSlotDescriptor* slot, const char* namePrefix,
                                  uint8_t precision, uint8_t priority) = 0;

    // Detaches a previously-attached sensor, e.g. when hardware is no
    // longer present. Publishes "offline" and removes its HA discovery
    // entry. The handle must not be used again afterwards; the underlying
    // slot itself still exists (it's compile-time-permanent) and may be
    // re-attached later with a new handle.
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
    // when a sensor is attached.
    virtual void setSensorAvailable(uint8_t handle, bool available) = 0;

    // ---- Consumer API: pull a value by quantity, not by name/handle ------
    //
    // Lets a *consumer* usermod ask "give me the temperature" without caring
    // which provider (dht, shtc3, bme280, ...) - or how many of them - are
    // actually attached. If several sensors share the same quantity
    // (matched by SensorTypeInfo::id), the hub answers with whichever one
    // is currently available() and valid() (has received a reading) with
    // the lowest attachSensor() 'priority'; ties go to whichever attached
    // first. Returns false (and leaves 'value' untouched) if no matching
    // sensor currently qualifies.
    //
    // Deliberately not handle-based: which physical sensor answers a given
    // quantity can change at runtime (e.g. a higher-priority sensor going
    // offline), so call this fresh whenever you need a reading rather than
    // caching a handle across loop() calls.
    virtual bool getValue(const SensorTypeInfo& type, float& value) = 0;
    virtual bool getValueBinary(const SensorTypeInfo& type, bool& value) = 0;

    // Same selection as getValue()/getValueBinary(), but returns the
    // attached name of the sensor that would answer (or currently answers)
    // that quantity - handy for logging/UI ("temperature from
    // 'kitchen_bme280'"). Returns nullptr if none currently qualify.
    virtual const char* getValueSourceName(const SensorTypeInfo& type) = 0;

    // Looks up one specific sensor by its exact attached name (same
    // matching as attachSensor()'s duplicate check) and returns its
    // current numeric value. Prefer getValue(type) when you just want "the"
    // reading for a quantity and don't care which provider supplies it;
    // use this when you need a SPECIFIC sensor instead - e.g. one axis of a
    // multi-axis sensor (several axes legitimately share the same
    // quantity, so getValue(SensorTypes::Acceleration) can't tell them
    // apart). Returns false (value untouched) for an unknown name, a binary
    // sensor, or one that hasn't reported a reading yet / is unavailable.
    virtual bool getValueByName(const char* name, float& value) = 0;
};

// Looks up the Sensor Hub usermod. Returns nullptr if it isn't present in
// this build (e.g. forgot to add it to custom_usermods) - always check
// before use. Cheap enough to call every loop(), but caching the result
// after the first non-null lookup is recommended.
static inline SensorHub* getSensorHub() {
  Usermod* mod = UsermodManager::lookup(USERMOD_ID_SENSOR_HUB);
  return mod ? static_cast<SensorHub*>(mod) : nullptr; // safe: SensorHub derives from Usermod
}
