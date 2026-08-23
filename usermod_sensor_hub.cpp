#include "wled.h"
#include "sensor_bus.h"
#include <vector>

/*
 * Sensor Hub - a central usermod that receives sensor readings from other
 * ("provider") usermods over the bus defined in sensor_bus.h, and is the
 * only thing that turns them into:
 *   - MQTT state topics, with Home Assistant MQTT discovery
 *   - entries in the /json/state and /json/info JSON API
 *   - rows in the WLED web UI Info tab
 *
 * Provider usermods (temperature, motion, humidity, ...) never talk to
 * MQTT/JSON/Info directly - they declare their sensors at compile time via
 * REGISTER_SENSOR_SLOT() and call attachSensor()/updateSensor() on this hub
 * (looked up via getSensorHub()) instead. This keeps sensor drivers small,
 * reusable, and consistent with each other regardless of who wrote them.
 * See examples/demo_sensor_provider.cpp for a template.
 *
 * Why not WLED's built-in getUMData()/um_data_t exchange mechanism?
 * getUMData() is a pull-based, one-slot-per-usermod mechanism (a usermod
 * exposes a single struct that others poll for by ID) - a great fit for
 * things like audioreactive's FFT data. Here we need many independent
 * provider usermods to each dynamically register an arbitrary number of
 * named sensors with a shared hub, so a small push-based interface (this
 * file) is a better match, following the same "lookup by usermod ID, cast
 * to a known interface" pattern WLED usermods already use for e.g. the
 * four-line-display usermod.
 *
 * Sensor table sizing: DECLARE_DYNARRAY(SensorSlotDescriptor, sensorSlots)
 * must appear in exactly one translation unit (see dynarray.h) - this is
 * it. Every provider usermod's REGISTER_SENSOR_SLOT() call (in sensor_bus.h)
 * just *adds* a member to this same linker-managed array from its own
 * file, so DYNARRAY_LENGTH(sensorSlots) below is exactly the number of
 * sensor slots linked into this particular build - no fixed cap.
 */
DECLARE_DYNARRAY(SensorSlotDescriptor, sensorSlots);

class SensorHubUsermod : public SensorHub {

  private:

    // Per-quantity Info tab display units (see displayValue() below). Values
    // are always stored/published to MQTT/HA/JSON in the canonical SI unit
    // regardless of these settings - only the Info tab ("u" object in
    // /json/info) renders in the chosen unit, so automations/HA history
    // reading MQTT or /json/state are never affected by a user changing
    // their display preference.
    enum class TempUnit     : uint8_t { Celsius = 0, Fahrenheit = 1 };
    enum class PressureUnit : uint8_t { HPa = 0, InHg = 1 };
    enum class DistanceUnit : uint8_t { Millimeters = 0, Centimeters = 1, Meters = 2, Inches = 3 };
    enum class AccelUnit    : uint8_t { MetersPerSecond2 = 0, G = 1 };

    struct DisplayValue { float value; const char* unit; uint8_t precision; };

    struct Sensor {
      bool used;
      char name[24];
      const SensorTypeInfo* type;
      char unit[8];
      char deviceClass[24];
      uint8_t precision;
      uint8_t priority;    // lower wins ties among sensors of the same type in getValue()
      float value;
      bool boolValue;
      bool available;
      bool valid;          // has received at least one reading
      bool dirty;          // value changed since last MQTT publish
      bool discoverySent;
      unsigned long lastPublish;
    };

    // Sized once in setup() from DYNARRAY_LENGTH(sensorSlots) - one entry
    // per compile-time-declared slot across every linked-in provider, never
    // grown/shrunk afterwards. Index i always corresponds to
    // sensorSlots_begin[i]; 'used' distinguishes a slot no provider has
    // attached (yet, or ever, e.g. its provider's pins are unconfigured)
    // from one that has.
    std::vector<Sensor> sensors;

    bool enabled = true;
    bool initDone = false;

    // config
    bool haDiscovery = true;
    String haDiscoveryPrefix = "homeassistant";
    uint16_t publishIntervalS = 300; // heartbeat republish even if unchanged; 0 = only on change
    bool retainMqtt = true;
    uint8_t tempUnit = (uint8_t)TempUnit::Celsius;
    uint8_t pressureUnit = (uint8_t)PressureUnit::HPa;
    uint8_t distanceUnit = (uint8_t)DistanceUnit::Millimeters;
    uint8_t accelUnit = (uint8_t)AccelUnit::MetersPerSecond2;

    static const char _name[];
    static const char _enabled[];
    static const char _haDiscovery[];
    static const char _publishInterval[];
    static const char _tempUnit[];
    static const char _pressureUnit[];
    static const char _distanceUnit[];
    static const char _accelUnit[];

    static bool namesEqual(const char* a, const char* b) {
      while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
      }
      return *a == *b;
    }

    static String slug(const char* name) {
      String s; s.reserve(strlen(name));
      for (const char* p = name; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '_';
        s += c;
      }
      return s;
    }

    // Bumps 'base' precision by 'extra' decimal places, clamped to 6 (same
    // bound attachSensor() enforces) so a converted value never overflows
    // the small fixed-size buffers publishState()/addToJsonInfo() assume.
    static uint8_t bumpPrecision(uint8_t base, uint8_t extra) {
      uint16_t p = (uint16_t)base + extra;
      return p > 6 ? 6 : (uint8_t)p;
    }

    // Converts a sensor's canonical value/precision to the unit the user
    // configured for its quantity (Info tab display only - see the unit
    // fields above). Falls through to the canonical value/unit/precision
    // unchanged for binary types, ad-hoc/custom types, and whenever the
    // configured unit *is* the canonical one. Matches by SensorTypeInfo::id
    // string, not object identity - see sensor_bus.h.
    DisplayValue displayValue(const Sensor& s) const {
      if (strcmp(s.type->id, SensorTypes::Temperature.id) == 0) {
        if ((TempUnit)tempUnit == TempUnit::Fahrenheit)
          return { s.value * 9.0f / 5.0f + 32.0f, "°F", s.precision };
      } else if (strcmp(s.type->id, SensorTypes::Pressure.id) == 0) {
        if ((PressureUnit)pressureUnit == PressureUnit::InHg)
          return { s.value * 0.0295299830714f, "inHg", bumpPrecision(s.precision, 1) };
      } else if (strcmp(s.type->id, SensorTypes::Distance.id) == 0) {
        switch ((DistanceUnit)distanceUnit) {
          case DistanceUnit::Centimeters: return { s.value / 10.0f,   "cm", bumpPrecision(s.precision, 1) };
          case DistanceUnit::Meters:      return { s.value / 1000.0f, "m",  bumpPrecision(s.precision, 2) };
          case DistanceUnit::Inches:      return { s.value / 25.4f,   "in", bumpPrecision(s.precision, 1) };
          default: break; // Millimeters - canonical, nothing to convert
        }
      } else if (strcmp(s.type->id, SensorTypes::Acceleration.id) == 0) {
        if ((AccelUnit)accelUnit == AccelUnit::G)
          return { s.value / 9.80665f, "g", s.precision };
      }
      return { s.value, s.unit, s.precision };
    }

    void stateTopic(const Sensor& s, char* buf, size_t len) {
      snprintf_P(buf, len, PSTR("%s/sensor/%s/state"), mqttDeviceTopic, slug(s.name).c_str());
    }

    void availabilityTopic(const Sensor& s, char* buf, size_t len) {
      snprintf_P(buf, len, PSTR("%s/sensor/%s/availability"), mqttDeviceTopic, slug(s.name).c_str());
    }

    // All four publish* helpers are declared unconditionally so call sites
    // never need to care whether MQTT is compiled in; their bodies compile
    // to a no-op when WLED_DISABLE_MQTT is set (mirrors the pattern used in
    // the WLED usermod template's publishMqtt()).
    void publishState(Sensor& s);
    void publishAvailability(Sensor& s);
    void publishDiscovery(Sensor& s);
    void publishDiscoveryRemoval(Sensor& s);

    // Selects the sensor a getValue()/getValueBinary()/getValueSourceName()
    // query for 'type' should answer with: lowest 'priority' among used,
    // available, valid sensors of that type (matched by id string), ties
    // broken by slot index (== link order). SENSOR_HANDLE_INVALID if none
    // qualify.
    uint8_t bestSensorOfType(const SensorTypeInfo& type) {
      uint8_t best = SENSOR_HANDLE_INVALID;
      for (size_t i = 0; i < sensors.size(); i++) {
        Sensor& s = sensors[i];
        if (!s.used || strcmp(s.type->id, type.id) != 0 || !s.valid || !s.available) continue;
        if (best == SENSOR_HANDLE_INVALID || s.priority < sensors[best].priority) best = (uint8_t)i;
      }
      return best;
    }

  public:

    void setup() override {
      size_t n = (size_t)DYNARRAY_LENGTH(sensorSlots);
      sensors.clear();
      sensors.reserve(n);
      for (size_t i = 0; i < n; i++) {
        const SensorSlotDescriptor& slot = DYNARRAY_BEGIN(sensorSlots)[i];
        Sensor s{};
        s.used = false;
        s.type = slot.type;
        s.precision = slot.defaultPrecision > 6 ? 6 : slot.defaultPrecision;
        s.priority = slot.defaultPriority;
        sensors.push_back(s);
      }
      initDone = true;
    }

    void loop() override {
      if (!enabled || !initDone || !WLED_MQTT_CONNECTED) return;
      unsigned long now = millis();
      for (Sensor& s : sensors) {
        if (!s.used || !s.valid) continue;
        bool heartbeatDue = publishIntervalS > 0 && (now - s.lastPublish) >= (unsigned long)publishIntervalS * 1000UL;
        if (s.dirty || heartbeatDue) publishState(s);
      }
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      for (Sensor& s : sensors) {
        if (!s.used) continue;
        JsonArray arr = user.createNestedArray(s.name);
        if (!s.valid) {
          arr.add(F("n/a"));
          continue;
        }
        if (sensorTypeIsBinary(*s.type)) {
          arr.add(s.boolValue ? F("on") : F("off"));
        } else {
          DisplayValue dv = displayValue(s);
          float rounded = roundf(dv.value * powf(10, dv.precision)) / powf(10, dv.precision);
          arr.add(rounded);
          if (dv.unit[0]) arr.add(dv.unit);
        }
      }
    }

    void addToJsonState(JsonObject& root) override {
      if (!initDone) return;
      JsonObject mod = root[FPSTR(_name)];
      if (mod.isNull()) mod = root.createNestedObject(FPSTR(_name));
      JsonArray arr = mod.createNestedArray(F("sensors"));
      for (Sensor& s : sensors) {
        if (!s.used) continue;
        JsonObject o = arr.createNestedObject();
        o[F("name")] = s.name;
        o[F("available")] = s.available;
        if (!s.valid) continue;
        if (sensorTypeIsBinary(*s.type)) {
          o[F("value")] = s.boolValue;
        } else {
          o[F("value")] = roundf(s.value * powf(10, s.precision)) / powf(10, s.precision);
          if (s.unit[0]) o[F("unit")] = s.unit;
        }
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_haDiscovery)] = haDiscovery;
      top[F("haPrefix")] = haDiscoveryPrefix;
      top[FPSTR(_publishInterval)] = publishIntervalS;
      top[F("retain")] = retainMqtt;
      top[FPSTR(_tempUnit)] = tempUnit;
      top[FPSTR(_pressureUnit)] = pressureUnit;
      top[FPSTR(_distanceUnit)] = distanceUnit;
      top[FPSTR(_accelUnit)] = accelUnit;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_haDiscovery)], haDiscovery);
      configComplete &= getJsonValue(top[F("haPrefix")], haDiscoveryPrefix);
      configComplete &= getJsonValue(top[FPSTR(_publishInterval)], publishIntervalS);
      configComplete &= getJsonValue(top[F("retain")], retainMqtt);
      configComplete &= getJsonValue(top[FPSTR(_tempUnit)], tempUnit);
      configComplete &= getJsonValue(top[FPSTR(_pressureUnit)], pressureUnit);
      configComplete &= getJsonValue(top[FPSTR(_distanceUnit)], distanceUnit);
      configComplete &= getJsonValue(top[FPSTR(_accelUnit)], accelUnit);
      return configComplete;
    }

    void appendConfigData(Print& settingsScript) override {
      settingsScript.print(F("addInfo('SensorHub:haPrefix',1,'Home Assistant MQTT discovery topic prefix');"));
      settingsScript.print(F("addInfo('SensorHub:publishInterval',1,'seconds between heartbeat republishes of an unchanged value, 0=off');"));
      settingsScript.print(F("addInfo('SensorHub:retain',1,'retain MQTT sensor state messages');"));
      settingsScript.print(F("dd=addDropdown('SensorHub','tempUnit');addOption(dd,'°C',0);addOption(dd,'°F',1);"));
      settingsScript.print(F("addInfo('SensorHub:tempUnit',1,'Info tab display unit only - MQTT/Home Assistant/JSON API always report °C');"));
      settingsScript.print(F("dd=addDropdown('SensorHub','pressureUnit');addOption(dd,'hPa',0);addOption(dd,'inHg',1);"));
      settingsScript.print(F("addInfo('SensorHub:pressureUnit',1,'Info tab display unit only - MQTT/Home Assistant/JSON API always report hPa');"));
      settingsScript.print(F("dd=addDropdown('SensorHub','distanceUnit');addOption(dd,'mm',0);addOption(dd,'cm',1);addOption(dd,'m',2);addOption(dd,'in',3);"));
      settingsScript.print(F("addInfo('SensorHub:distanceUnit',1,'Info tab display unit only - MQTT/Home Assistant/JSON API always report mm');"));
      settingsScript.print(F("dd=addDropdown('SensorHub','accelUnit');addOption(dd,'m/s²',0);addOption(dd,'g',1);"));
      settingsScript.print(F("addInfo('SensorHub:accelUnit',1,'Info tab display unit only - MQTT/Home Assistant/JSON API always report m/s²');"));
    }

    void onMqttConnect(bool sessionPresent) override {
      if (!enabled) return;
      for (Sensor& s : sensors) {
        if (!s.used) continue;
        s.discoverySent = false;
        if (haDiscovery) publishDiscovery(s);
        publishAvailability(s);
        if (s.valid) s.dirty = true; // republish last known value on (re)connect
      }
    }

    uint16_t getId() override { return USERMOD_ID_SENSOR_HUB; }

    // ---- SensorHub bus interface -----------------------------------------

    uint8_t attachSensor(const SensorSlotDescriptor* slot, const char* namePrefix,
                          uint8_t precision, uint8_t priority) override {
      if (!slot || !namePrefix || !namePrefix[0]) return SENSOR_HANDLE_INVALID;

      // Resolve the slot pointer to its index - it's one of the compile-time
      // entries setup() pre-populated sensors[] from, in the same order.
      const SensorSlotDescriptor* base = &DYNARRAY_BEGIN(sensorSlots)[0];
      ptrdiff_t idx = slot - base;
      if (idx < 0 || (size_t)idx >= sensors.size()) return SENSOR_HANDLE_INVALID; // slot not known to this hub

      char fullName[sizeof(Sensor::name)];
      snprintf(fullName, sizeof(fullName), "%s%s", namePrefix, slot->nameSuffix);

      for (size_t i = 0; i < sensors.size(); i++) {
        if ((ptrdiff_t)i == idx) continue;
        if (sensors[i].used && namesEqual(sensors[i].name, fullName)) return SENSOR_HANDLE_INVALID; // duplicate
      }

      Sensor& s = sensors[(size_t)idx];
      s.used = true;
      strlcpy(s.name, fullName, sizeof(s.name));
      s.type = slot->type;
      strlcpy(s.unit, slot->type->unit ? slot->type->unit : "", sizeof(s.unit));
      strlcpy(s.deviceClass, slot->type->haDeviceClass ? slot->type->haDeviceClass : "", sizeof(s.deviceClass));
      s.precision = precision > 6 ? 6 : precision; // clamp: bounds dtostrf() output length in publishState()
      s.priority = priority;
      s.value = NAN;
      s.boolValue = false;
      s.available = true;
      s.valid = false;
      s.dirty = false;
      s.discoverySent = false;
      s.lastPublish = 0;
      if (initDone && enabled && haDiscovery) publishDiscovery(s);
      return (uint8_t)idx;
    }

    void unregisterSensor(uint8_t handle) override {
      if (handle >= sensors.size() || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      s.available = false;
      publishAvailability(s);
      if (s.discoverySent) publishDiscoveryRemoval(s);
      s.used = false;
    }

    void updateSensor(uint8_t handle, float value) override {
      if (handle >= sensors.size() || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      if (sensorTypeIsBinary(*s.type)) return;
      bool changed = !s.valid || fabsf(s.value - value) > 0.0001f;
      s.value = value;
      s.valid = true;
      if (changed) s.dirty = true;
    }

    void updateSensorBinary(uint8_t handle, bool value) override {
      if (handle >= sensors.size() || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      if (!sensorTypeIsBinary(*s.type)) return;
      bool changed = !s.valid || s.boolValue != value;
      s.boolValue = value;
      s.valid = true;
      if (changed) s.dirty = true;
    }

    void setSensorAvailable(uint8_t handle, bool available) override {
      if (handle >= sensors.size() || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      if (s.available == available) return;
      s.available = available;
      publishAvailability(s);
    }

    // ---- Consumer API: pull a value by quantity --------------------------

    bool getValue(const SensorTypeInfo& type, float& value) override {
      if (type.isBinary) return false;
      uint8_t h = bestSensorOfType(type);
      if (h == SENSOR_HANDLE_INVALID) return false;
      value = sensors[h].value;
      return true;
    }

    bool getValueBinary(const SensorTypeInfo& type, bool& value) override {
      if (!type.isBinary) return false;
      uint8_t h = bestSensorOfType(type);
      if (h == SENSOR_HANDLE_INVALID) return false;
      value = sensors[h].boolValue;
      return true;
    }

    const char* getValueSourceName(const SensorTypeInfo& type) override {
      uint8_t h = bestSensorOfType(type);
      return h == SENSOR_HANDLE_INVALID ? nullptr : sensors[h].name;
    }

    bool getValueByName(const char* name, float& value) override {
      if (!name || !name[0]) return false;
      for (Sensor& s : sensors) {
        if (!s.used || sensorTypeIsBinary(*s.type) || !s.valid || !s.available) continue;
        if (namesEqual(s.name, name)) { value = s.value; return true; }
      }
      return false;
    }
};

const char SensorHubUsermod::_name[]            PROGMEM = "SensorHub";
const char SensorHubUsermod::_enabled[]         PROGMEM = "enabled";
const char SensorHubUsermod::_haDiscovery[]     PROGMEM = "haDiscovery";
const char SensorHubUsermod::_publishInterval[] PROGMEM = "publishInterval";
const char SensorHubUsermod::_tempUnit[]        PROGMEM = "tempUnit";
const char SensorHubUsermod::_pressureUnit[]    PROGMEM = "pressureUnit";
const char SensorHubUsermod::_distanceUnit[]    PROGMEM = "distanceUnit";
const char SensorHubUsermod::_accelUnit[]       PROGMEM = "accelUnit";


// ---- MQTT implementation (no-op when WLED_DISABLE_MQTT is set) ------------

void SensorHubUsermod::publishState(Sensor& s)
{
#ifndef WLED_DISABLE_MQTT
  if (!WLED_MQTT_CONNECTED) return;
  char topic[96];
  stateTopic(s, topic, sizeof(topic));
  char payload[32]; // precision is clamped to <= 6 in attachSensor(), plenty of headroom here
  if (sensorTypeIsBinary(*s.type)) {
    strcpy(payload, s.boolValue ? "ON" : "OFF");
  } else {
    dtostrf(s.value, 0, s.precision, payload);
  }
  mqtt->publish(topic, 0, retainMqtt, payload);
  s.dirty = false;
  s.lastPublish = millis();
#endif
}

void SensorHubUsermod::publishAvailability(Sensor& s)
{
#ifndef WLED_DISABLE_MQTT
  if (!WLED_MQTT_CONNECTED) return;
  char topic[96];
  availabilityTopic(s, topic, sizeof(topic));
  mqtt->publish(topic, 0, true, s.available ? "online" : "offline");
#endif
}

void SensorHubUsermod::publishDiscovery(Sensor& s)
{
#ifndef WLED_DISABLE_MQTT
  if (!WLED_MQTT_CONNECTED) return;
  bool binary = sensorTypeIsBinary(*s.type);
  char stateTopicBuf[96], availTopicBuf[96];
  stateTopic(s, stateTopicBuf, sizeof(stateTopicBuf));
  availabilityTopic(s, availTopicBuf, sizeof(availTopicBuf));

  String discTopic = haDiscoveryPrefix + (binary ? "/binary_sensor/" : "/sensor/") + escapedMac + "/" + slug(s.name) + "/config";

  DynamicJsonDocument doc(768);
  doc[F("name")] = s.name;
  doc[F("uniq_id")] = escapedMac + "_" + slug(s.name);
  doc[F("stat_t")] = stateTopicBuf;

  JsonArray avty = doc.createNestedArray(F("avty"));
  JsonObject a1 = avty.createNestedObject();
  a1[F("topic")] = mqttStatusTopic;      // WLED's own device-wide LWT topic
  JsonObject a2 = avty.createNestedObject();
  a2[F("topic")] = availTopicBuf;        // this sensor's own availability
  doc[F("avty_mode")] = F("all");

  if (s.deviceClass[0]) doc[F("dev_cla")] = s.deviceClass;
  if (binary) {
    doc[F("pl_on")]  = F("ON");
    doc[F("pl_off")] = F("OFF");
  } else {
    if (s.unit[0]) doc[F("unit_of_meas")] = s.unit;
    if (publishIntervalS > 0) doc[F("exp_aft")] = (uint32_t)publishIntervalS * 3;
  }

  JsonObject dev = doc.createNestedObject(F("dev"));
  dev[F("ids")] = "wled-" + escapedMac;
  dev[F("name")] = serverDescription;
  dev[F("mf")] = F(WLED_BRAND);
  dev[F("mdl")] = F(WLED_PRODUCT_NAME);
  dev[F("sw")] = versionString;

  String payload;
  serializeJson(doc, payload);
  mqtt->publish(discTopic.c_str(), 0, true, payload.c_str());
  s.discoverySent = true;
#endif
}

void SensorHubUsermod::publishDiscoveryRemoval(Sensor& s)
{
#ifndef WLED_DISABLE_MQTT
  if (!WLED_MQTT_CONNECTED) return;
  bool binary = sensorTypeIsBinary(*s.type);
  String discTopic = haDiscoveryPrefix + (binary ? "/binary_sensor/" : "/sensor/") + escapedMac + "/" + slug(s.name) + "/config";
  mqtt->publish(discTopic.c_str(), 0, true, ""); // empty retained payload removes the HA entity
  s.discoverySent = false;
#endif
}


static SensorHubUsermod sensor_hub;
REGISTER_USERMOD(sensor_hub);
