#include "wled.h"
#include "sensor_bus.h"

/*
 * Sensor Hub - a central usermod that receives sensor readings from other
 * ("provider") usermods over the bus defined in sensor_bus.h, and is the
 * only thing that turns them into:
 *   - MQTT state topics, with Home Assistant MQTT discovery
 *   - entries in the /json/state and /json/info JSON API
 *   - rows in the WLED web UI Info tab
 *
 * Provider usermods (temperature, motion, humidity, ...) never talk to
 * MQTT/JSON/Info directly - they call registerSensor()/updateSensor() on
 * this hub (looked up via getSensorHub()) instead. This keeps sensor
 * drivers small, reusable, and consistent with each other regardless of
 * who wrote them. See examples/demo_sensor_provider.cpp for a template.
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
 */

class SensorHubUsermod : public SensorHub {

  private:

    struct Sensor {
      bool used;
      char name[24];
      SensorType type;
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

    Sensor sensors[SENSOR_HUB_MAX_SENSORS];

    bool enabled = true;
    bool initDone = false;

    // config
    bool haDiscovery = true;
    String haDiscoveryPrefix = "homeassistant";
    uint16_t publishIntervalS = 300; // heartbeat republish even if unchanged; 0 = only on change
    bool retainMqtt = true;

    static const char _name[];
    static const char _enabled[];
    static const char _haDiscovery[];
    static const char _publishInterval[];

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

    static const char* defaultUnit(SensorType t) {
      switch (t) {
        case SensorType::Temperature: return "°C";
        case SensorType::Humidity:    return "%";
        case SensorType::Pressure:    return "hPa";
        case SensorType::Illuminance: return "lx";
        case SensorType::VoltageV:    return "V";
        case SensorType::Battery:     return "%";
        case SensorType::Co2:         return "ppm";
        case SensorType::Acceleration:return "m/s²";
        case SensorType::Distance:    return "mm";
        case SensorType::Current:     return "A";
        case SensorType::Power:       return "W";
        default:                      return "";
      }
    }

    static const char* defaultDeviceClass(SensorType t) {
      switch (t) {
        case SensorType::Temperature: return "temperature";
        case SensorType::Humidity:    return "humidity";
        case SensorType::Pressure:    return "pressure";
        case SensorType::Illuminance: return "illuminance";
        case SensorType::VoltageV:    return "voltage";
        case SensorType::Battery:     return "battery";
        case SensorType::Co2:         return "carbon_dioxide";
        case SensorType::Distance:    return "distance";
        case SensorType::Current:     return "current";
        case SensorType::Power:       return "power";
        case SensorType::Motion:      return "motion";
        case SensorType::Contact:     return "door";
        // Acceleration: no standard HA device_class - falls through to "" (plain sensor)
        default:                      return "";
      }
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
    // available, valid sensors of that type, ties broken by array index
    // (== registration order). SENSOR_HANDLE_INVALID if none qualify.
    uint8_t bestSensorOfType(SensorType type) {
      uint8_t best = SENSOR_HANDLE_INVALID;
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) {
        Sensor& s = sensors[i];
        if (!s.used || s.type != type || !s.valid || !s.available) continue;
        if (best == SENSOR_HANDLE_INVALID || s.priority < sensors[best].priority) best = i;
      }
      return best;
    }

  public:

    void setup() override {
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) sensors[i].used = false;
      initDone = true;
    }

    void loop() override {
      if (!enabled || !initDone || !WLED_MQTT_CONNECTED) return;
      unsigned long now = millis();
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) {
        Sensor& s = sensors[i];
        if (!s.used || !s.valid) continue;
        bool heartbeatDue = publishIntervalS > 0 && (now - s.lastPublish) >= (unsigned long)publishIntervalS * 1000UL;
        if (s.dirty || heartbeatDue) publishState(s);
      }
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) {
        Sensor& s = sensors[i];
        if (!s.used) continue;
        JsonArray arr = user.createNestedArray(s.name);
        if (!s.valid) {
          arr.add(F("n/a"));
          continue;
        }
        if (sensorTypeIsBinary(s.type)) {
          arr.add(s.boolValue ? F("on") : F("off"));
        } else {
          float rounded = roundf(s.value * powf(10, s.precision)) / powf(10, s.precision);
          arr.add(rounded);
          if (s.unit[0]) arr.add(s.unit);
        }
      }
    }

    void addToJsonState(JsonObject& root) override {
      if (!initDone) return;
      JsonObject mod = root[FPSTR(_name)];
      if (mod.isNull()) mod = root.createNestedObject(FPSTR(_name));
      JsonArray arr = mod.createNestedArray(F("sensors"));
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) {
        Sensor& s = sensors[i];
        if (!s.used) continue;
        JsonObject o = arr.createNestedObject();
        o[F("name")] = s.name;
        o[F("available")] = s.available;
        if (!s.valid) continue;
        if (sensorTypeIsBinary(s.type)) {
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
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_haDiscovery)], haDiscovery);
      configComplete &= getJsonValue(top[F("haPrefix")], haDiscoveryPrefix);
      configComplete &= getJsonValue(top[FPSTR(_publishInterval)], publishIntervalS);
      configComplete &= getJsonValue(top[F("retain")], retainMqtt);
      return configComplete;
    }

    void appendConfigData(Print& settingsScript) override {
      settingsScript.print(F("addInfo('SensorHub:haPrefix',1,'Home Assistant MQTT discovery topic prefix');"));
      settingsScript.print(F("addInfo('SensorHub:publishInterval',1,'seconds between heartbeat republishes of an unchanged value, 0=off');"));
      settingsScript.print(F("addInfo('SensorHub:retain',1,'retain MQTT sensor state messages');"));
    }

    void onMqttConnect(bool sessionPresent) override {
      if (!enabled) return;
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) {
        Sensor& s = sensors[i];
        if (!s.used) continue;
        s.discoverySent = false;
        if (haDiscovery) publishDiscovery(s);
        publishAvailability(s);
        if (s.valid) s.dirty = true; // republish last known value on (re)connect
      }
    }

    uint16_t getId() override { return USERMOD_ID_SENSOR_HUB; }

    // ---- SensorHub bus interface -----------------------------------------

    uint8_t registerSensor(const char* name, SensorType type, const char* unit,
                            const char* deviceClass, uint8_t precision, uint8_t priority) override {
      if (!name || !name[0]) return SENSOR_HANDLE_INVALID;
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) {
        if (sensors[i].used && namesEqual(sensors[i].name, name)) return SENSOR_HANDLE_INVALID; // duplicate
      }
      for (uint8_t i = 0; i < SENSOR_HUB_MAX_SENSORS; i++) {
        if (sensors[i].used) continue;
        Sensor& s = sensors[i];
        s.used = true;
        strlcpy(s.name, name, sizeof(s.name));
        s.type = type;
        strlcpy(s.unit, unit ? unit : defaultUnit(type), sizeof(s.unit));
        strlcpy(s.deviceClass, deviceClass ? deviceClass : defaultDeviceClass(type), sizeof(s.deviceClass));
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
        return i;
      }
      return SENSOR_HANDLE_INVALID; // hub full
    }

    void unregisterSensor(uint8_t handle) override {
      if (handle >= SENSOR_HUB_MAX_SENSORS || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      s.available = false;
      publishAvailability(s);
      if (s.discoverySent) publishDiscoveryRemoval(s);
      s.used = false;
    }

    void updateSensor(uint8_t handle, float value) override {
      if (handle >= SENSOR_HUB_MAX_SENSORS || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      if (sensorTypeIsBinary(s.type)) return;
      bool changed = !s.valid || fabsf(s.value - value) > 0.0001f;
      s.value = value;
      s.valid = true;
      if (changed) s.dirty = true;
    }

    void updateSensorBinary(uint8_t handle, bool value) override {
      if (handle >= SENSOR_HUB_MAX_SENSORS || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      if (!sensorTypeIsBinary(s.type)) return;
      bool changed = !s.valid || s.boolValue != value;
      s.boolValue = value;
      s.valid = true;
      if (changed) s.dirty = true;
    }

    void setSensorAvailable(uint8_t handle, bool available) override {
      if (handle >= SENSOR_HUB_MAX_SENSORS || !sensors[handle].used) return;
      Sensor& s = sensors[handle];
      if (s.available == available) return;
      s.available = available;
      publishAvailability(s);
    }

    // ---- Consumer API: pull a value by quantity --------------------------

    bool getValue(SensorType type, float& value) override {
      if (sensorTypeIsBinary(type)) return false;
      uint8_t h = bestSensorOfType(type);
      if (h == SENSOR_HANDLE_INVALID) return false;
      value = sensors[h].value;
      return true;
    }

    bool getValueBinary(SensorType type, bool& value) override {
      if (!sensorTypeIsBinary(type)) return false;
      uint8_t h = bestSensorOfType(type);
      if (h == SENSOR_HANDLE_INVALID) return false;
      value = sensors[h].boolValue;
      return true;
    }

    const char* getValueSourceName(SensorType type) override {
      uint8_t h = bestSensorOfType(type);
      return h == SENSOR_HANDLE_INVALID ? nullptr : sensors[h].name;
    }
};

const char SensorHubUsermod::_name[]            PROGMEM = "SensorHub";
const char SensorHubUsermod::_enabled[]         PROGMEM = "enabled";
const char SensorHubUsermod::_haDiscovery[]     PROGMEM = "haDiscovery";
const char SensorHubUsermod::_publishInterval[] PROGMEM = "publishInterval";


// ---- MQTT implementation (no-op when WLED_DISABLE_MQTT is set) ------------

void SensorHubUsermod::publishState(Sensor& s)
{
#ifndef WLED_DISABLE_MQTT
  if (!WLED_MQTT_CONNECTED) return;
  char topic[96];
  stateTopic(s, topic, sizeof(topic));
  char payload[32]; // precision is clamped to <= 6 in registerSensor(), plenty of headroom here
  if (sensorTypeIsBinary(s.type)) {
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
  bool binary = sensorTypeIsBinary(s.type);
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
  bool binary = sensorTypeIsBinary(s.type);
  String discTopic = haDiscoveryPrefix + (binary ? "/binary_sensor/" : "/sensor/") + escapedMac + "/" + slug(s.name) + "/config";
  mqtt->publish(discTopic.c_str(), 0, true, ""); // empty retained payload removes the HA entity
  s.discoverySent = false;
#endif
}


static SensorHubUsermod sensor_hub;
REGISTER_USERMOD(sensor_hub);
