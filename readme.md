# WLED Sensor Hub

A central, out-of-tree [WLED](https://github.com/wled/WLED) usermod that turns
sensor readings into MQTT (with Home Assistant discovery), the `/json` API
and the web UI Info tab - all in one consistent place.

## Why

If you write one usermod per sensor (a DHT22 here, a PIR there, a BME280
somewhere else), you usually end up re-implementing the same MQTT topic
naming, Home Assistant discovery payloads and Info tab formatting in every
single one. This repo splits that in two:

- **Sensor Hub** (`usermod_sensor_hub.cpp`, this repo) - the one place that
  owns MQTT, HA discovery, `/json/state`, `/json/info` and the Info tab. You
  build this once and never touch it again when adding a new sensor.
- **Sensor providers** - small, separate usermods that only know how to read
  a piece of hardware (a GPIO pin, an I2C sensor, ...) and push readings to
  the hub. You write one of these per sensor type.

The two talk to each other over a tiny bus defined in [`sensor_bus.h`](sensor_bus.h):
a provider calls `registerSensor()` once and `updateSensor()` /
`updateSensorBinary()` whenever it has a fresh reading. That's the entire
contract - the provider never needs to know MQTT, JSON or HTML exist.

```
 ┌────────────────────┐        registerSensor()        ┌───────────────────┐
 │ Temperature usermod │ ──────────────────────────────▶│                   │
 └────────────────────┘        updateSensor()           │    Sensor Hub     │──▶ MQTT (+ HA discovery)
 ┌────────────────────┐                                 │ (usermod_sensor_  │──▶ /json/state, /json/info
 │   Motion usermod    │ ──────────────────────────────▶│    hub.cpp)       │──▶ Info tab
 └────────────────────┘        updateSensorBinary()     │                   │
          ...                                           └───────────────────┘
```

Any number of provider usermods, of any sensor type, can register with the
same hub. See [`sensor_bus.h`](sensor_bus.h) for the full interface, and
`examples/` for two complete, standalone provider usermods you can build as-is
or use as a template:

- [`examples/demo_sensor_provider/`](examples/demo_sensor_provider/) - a fake
  temperature + motion sensor, no hardware required
- [`examples/shtc3_sensor_provider/`](examples/shtc3_sensor_provider/) - a
  real I2C sensor (Sensirion SHTC3 temperature + humidity)

## What's in this repo

| Path | Purpose |
|---|---|
| `library.json` | PlatformIO library manifest for the hub (`libArchive: false` required; excludes `examples/` from the build) |
| `sensor_bus.h` | The shared bus: `SensorType` enum and the `SensorHub` interface that provider usermods call into |
| `usermod_sensor_hub.cpp` | The hub itself - implements `SensorHub`, publishes MQTT/HA/JSON/Info |
| `examples/demo_sensor_provider/` | A minimal, self-contained provider usermod (own `library.json` + readme) with simulated readings |
| `examples/shtc3_sensor_provider/` | A self-contained provider usermod (own `library.json` + readme) for a real Sensirion SHTC3 sensor |

Each example under `examples/` is its own complete out-of-tree usermod (with
its own `library.json` and `readme.md`), excluded from the hub's own build by
`library.json`'s `srcFilter` - point `custom_usermods` directly at an
example's folder to build it, no copying required.

## Wiring it into your WLED build

Clone this repo alongside your WLED checkout, same as any out-of-tree usermod
(see the [wled-usermod-example](https://github.com/wled/wled-usermod-example)
this project is based on):

```
~/projects/
  WLED/
  wled-sensor-hub/
    library.json
    sensor_bus.h
    usermod_sensor_hub.cpp
```

In `platformio_override.ini` inside the WLED folder:

```ini
[env:esp32dev]
extends = env:esp32dev
custom_usermods =
  ${env:esp32dev.custom_usermods}
  symlink:///home/you/projects/wled-sensor-hub
```

Or reference it directly by URL, no local clone needed:

```ini
custom_usermods =
  ${env:esp32dev.custom_usermods}
  symlink://github.com/you/wled-sensor-hub.git#main
```

Each sensor provider usermod (your DHT22, PIR, the bundled examples, ...) is
added the same way, as its own separate `custom_usermods` entry - just make
sure it can find `sensor_bus.h`, e.g. by adding this repo's path to
`build_flags`:

```ini
build_flags =
  ${env:esp32dev.build_flags}
  -I../wled-sensor-hub
```

## Writing a sensor provider usermod

```cpp
#include "wled.h"
#include "sensor_bus.h"

class MyTemperatureUsermod : public Usermod {
  SensorHub* hub = nullptr;
  uint8_t handle = SENSOR_HANDLE_INVALID;

  public:
    void setup() override { /* init your hardware here */ }

    void loop() override {
      if (!hub) hub = getSensorHub();   // lazy lookup - hub may init after us
      if (!hub) return;                 // Sensor Hub not present in this build

      if (handle == SENSOR_HANDLE_INVALID)
        handle = hub->registerSensor("living_room_temperature", SensorType::Temperature);

      // read your sensor on whatever schedule makes sense, then:
      hub->updateSensor(handle, myLatestReading);

      // if the sensor stops responding:
      // hub->setSensorAvailable(handle, false);
    }
};

static MyTemperatureUsermod my_temperature;
REGISTER_USERMOD(my_temperature);
```

`SensorType` covers `Temperature`, `Humidity`, `Pressure`, `Illuminance`,
`VoltageV`, `Battery`, `Co2`, `Acceleration`, `Distance`, `Current`, `Power`
(all numeric), and `Motion` / `Contact` (binary). Use `Generic` /
`GenericBinary` with an explicit unit/device class for anything else. See
the doc comments in `sensor_bus.h` for the full API.

### Bundled example providers

[`examples/demo_sensor_provider/`](examples/demo_sensor_provider/) and
[`examples/shtc3_sensor_provider/`](examples/shtc3_sensor_provider/) are
ready-to-build provider usermods, each with its own `library.json` (so
PlatformIO/library dependencies like the SHTC3's Adafruit driver are declared
right where they're needed, not forced onto the hub) and its own readme with
wiring/settings specific to that sensor. Point `custom_usermods` at either
folder directly - no copying needed - as shown in their readmes.

## What the hub publishes

For a sensor registered as `"living_room_temperature"` on a device whose
`mqttDeviceTopic` is `wled/livingroom`:

| Topic | Payload | Notes |
|---|---|---|
| `wled/livingroom/sensor/living_room_temperature/state` | `21.4` | retained (configurable) |
| `wled/livingroom/sensor/living_room_temperature/availability` | `online` / `offline` | retained |
| `homeassistant/sensor/<mac>/living_room_temperature/config` | HA discovery JSON | retained, sent on register + on every MQTT (re)connect |

Binary sensors (`Motion`/`Contact`/`GenericBinary`) publish `ON`/`OFF` on
their state topic and register as a HA `binary_sensor` instead of `sensor`.

Every sensor is also exposed via:
- **Info tab**: one row per sensor (`u` object in `/json/info`)
- **JSON API**: `GET /json/state` → `SensorHub.sensors[]`, one object per
  sensor with `name`, `value`/`unit` (or boolean `value` for binary sensors)
  and `available`

## Usermod Settings

| Setting | Default | Description |
|---|---|---|
| Enabled | on | Master on/off switch for the hub |
| HA Discovery | on | Send Home Assistant MQTT discovery configs |
| HA discovery prefix | `homeassistant` | Change if your HA instance uses a non-default discovery prefix |
| Publish interval | 300s | Heartbeat: republish the current value even if unchanged (0 = only publish on change) |
| Retain | on | Retain MQTT state/availability messages |

## Notes

- **Usermod ID**: this hub uses ID `199` (see `sensor_bus.h`), chosen to sit
  outside WLED's official ID range (`wled00/const.h`, currently up to ~58) to
  avoid collisions. If you already use 199 for something else, override it
  with `-D USERMOD_ID_SENSOR_HUB=<free id>` in `build_flags` - just make sure
  every provider usermod that includes `sensor_bus.h` is compiled with the
  same value.
- **Why not `getUMData()`?** WLED already has a generic inter-usermod data
  exchange mechanism (`Usermod::getUMData()` / `um_data_t`), used by e.g. the
  audioreactive usermod. It's a pull-based, one-struct-per-usermod design,
  which is a good fit when there's a single well-known producer. Here we
  need an arbitrary number of independent provider usermods to each
  register an arbitrary number of named sensors with one shared hub, so a
  small push-based interface (`sensor_bus.h`) fits better - it follows the
  same "look the usermod up by ID, cast to a known interface" pattern WLED
  already uses for things like the four-line-display usermod.
- **Home Assistant discovery** was removed from WLED core itself in favor of
  the native WLED integration (see `wled00/mqtt.cpp`) - this hub's discovery
  is only for the *sensors*, not the WLED light entity itself, so it doesn't
  conflict with that.
