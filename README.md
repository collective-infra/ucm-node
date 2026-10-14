# UCM Sensor Node Deployment README

## Project Overview

**UCM (Urban Climate Mesh)** is a distributed environmental sensing platform designed to collect high-resolution environmental data from geographically distributed sensor nodes.

Each UCM sensor node uses an **ESP32-P4** microcontroller connected to a **Sensirion SEN54** environmental sensor. Nodes communicate over Ethernet or Wi-Fi and publish environmental telemetry to an MQTT broker hosted on a Raspberry Pi UCM Hub.

The UCM Hub provides centralized message brokering, time-series storage, stream processing, visualization, and geospatial mapping.

The current system supports:

* Autonomous node operation
* Ethernet connectivity
* Wi-Fi connectivity
* Wi-Fi provisioning
* Automatic network reconnection
* MQTT telemetry
* SEN54 environmental sensing
* JSON-formatted sensor data
* Node identification and metadata
* Autonomous reboot recovery
* Remote monitoring through the UCM Hub
* Grafana and Leaflet visualization

A store-and-forward mechanism for buffering measurements during network outages is planned but is **not yet implemented**.

---

[Build Instructions](build-instructions.md)

# System Architecture

```text
                  ┌──────────────────────────┐
                  │       SEN54 Sensor       │
                  │                          │
                  │ PM1 / PM2.5 / PM4 / PM10│
                  │ Temperature / RH         │
                  │ VOC / NOx                │
                  └────────────┬─────────────┘
                               │ I²C
                               │
                  ┌────────────▼─────────────┐
                  │       ESP32-P4 Node      │
                  │                          │
                  │ Sensor acquisition       │
                  │ Network management       │
                  │ Wi-Fi / Ethernet         │
                  │ MQTT publishing          │
                  │ Node identification      │
                  └────────────┬─────────────┘
                               │
                         Wi-Fi / Ethernet
                               │
                               ▼
                  ┌──────────────────────────┐
                  │       UCM-HUB            │
                  │      Raspberry Pi 4      │
                  │                          │
                  │ Mosquitto MQTT           │
                  │ Telegraf                 │
                  │ InfluxDB                 │
                  │ Grafana                  │
                  │ Nginx                    │
                  │ Leaflet GIS              │
                  └──────────────────────────┘
```

Multiple sensor nodes may communicate with the same UCM Hub over a shared network.

---

# UCM Hub

| Component      | Current Configuration |
| -------------- | --------------------- |
| Hostname       | `UCM-HUB`             |
| Hardware       | Raspberry Pi 4        |
| OS             | Trixie OS Lite        |
| MQTT Broker    | Mosquitto             |
| Database       | InfluxDB v2           |
| Data Ingestion | Telegraf              |
| Visualization  | Grafana               |
| Web Server     | Nginx                 |
| GIS Interface  | Leaflet               |
| API            | UCM GIS/API service   |

The hub provides the central aggregation point for sensor telemetry.

The hub does **not** need to be on the same physical LAN as the sensor nodes as long as the network provides IP connectivity between the nodes and the MQTT broker.

---

# Sensor Node

## Hardware

| Component            | Configuration                     |
| -------------------- | --------------------------------- |
| MCU                  | ESP32-P4                          |
| Development Board    | Waveshare ESP32-P4 WiFi 6 PoE ETH |
| Environmental Sensor | Sensirion SEN54                   |
| Ethernet PHY         | IC+ IP101GR                       |
| Ethernet Speed       | 10/100 Mbps                       |
| Network Interfaces   | Ethernet and Wi-Fi                |
| Sensor Interface     | I²C                               |
| I²C Controller       | `I2C_NUM_0`                       |
| SDA                  | GPIO 7                            |
| SCL                  | GPIO 8                            |

The node is designed to use Ethernet when available and Wi-Fi when operating wirelessly.

---

# Node Identification

Each node has a unique UCM node identifier.

Example:

```text
UCM-E0A695
UCM-E0A8DC
```

The node identifier is used throughout the UCM system to associate telemetry with the physical sensor node.

Node identity should be treated as a logical identifier rather than a network address.

**Do not assume that a node has a permanent IP address.**

This is particularly important for deployment on networks such as NYC Mesh, where addresses may be assigned dynamically.

---

# Firmware

The sensor node firmware is developed using:

* ESP-IDF 5.5.5
* C/C++
* ESP32-P4 platform
* ESP-IDF networking components
* MQTT
* SEN54 sensor drivers
* Wi-Fi provisioning
* Ethernet networking

The firmware is responsible for:

1. Initializing the hardware
2. Detecting and initializing the SEN54
3. Establishing network connectivity
4. Connecting to the MQTT broker
5. Collecting environmental measurements
6. Formatting measurements as JSON
7. Publishing telemetry
8. Publishing node metadata
9. Recovering from network interruptions
10. Recovering from system reboots

---

# Environmental Measurements

The SEN54 provides the primary environmental measurements used by UCM.

Current telemetry includes:

| Field         | Description               |
| ------------- | ------------------------- |
| `pm1`         | PM1.0 particulate matter  |
| `pm25`        | PM2.5 particulate matter  |
| `pm4`         | PM4 particulate matter    |
| `pm10`        | PM10 particulate matter   |
| `temperature` | Temperature               |
| `humidity`    | Relative humidity         |
| `voc`         | VOC Index                 |
| `nox`         | NOx Index, when available |

Example MQTT payload:

```json
{
  "node": "UCM-E0A695",
  "timestamp": "2026-08-10T11:57:06Z",
  "pm1": 6.2,
  "pm25": 6.5,
  "pm4": 6.5,
  "pm10": 6.5,
  "temperature": 29.1,
  "humidity": 44.5,
  "voc": 156.0
}
```

---

# MQTT

The UCM MQTT topic structure is:

```text
ucm/node-<node_id>/environment
```

Example:

```text
ucm/node-UCM-E0A695/environment
```

The node publishes environmental telemetry to this topic.

The MQTT broker is currently hosted by the UCM Hub.

The firmware is configured to use the hub hostname:

```text
UCM-HUB.local
```

rather than relying on a fixed hub IP address.

This allows the deployment to operate across different networks where the hub's IP address may change.

---

# Network Connectivity

The node supports:

### Ethernet

Ethernet is provided through the Waveshare ESP32-P4 PoE/Ethernet hardware.

The Ethernet PHY is:

```text
IC+ IP101GR
```

with:

```text
MAC: ESP32-P4 internal EMAC
PHY address: 1
MDC: GPIO 31
MDIO: GPIO 52
PHY reset: GPIO 51
```

### Wi-Fi

The node can operate over Wi-Fi when Ethernet is unavailable.

Wi-Fi credentials are provisioned to the node and stored for subsequent reboots.

The node is designed to reconnect automatically when network connectivity is interrupted.

---

# Network Independence

A primary design goal of UCM is that sensor nodes should not depend on a particular local network configuration.

The node should therefore **not depend on a hard-coded IP address**.

The following may change between deployments:

* Node IP address
* Hub IP address
* Wi-Fi network
* Network gateway
* DNS configuration

The logical UCM node identity remains constant.

This allows the same firmware and hardware to be deployed in different environments, including:

* Institutional networks
* Residential networks
* Community networks
* NYC Mesh
* Other research test networks

---

# Data Pipeline

The current data path is:

```text
SEN54
   │
   ▼
ESP32-P4
   │
   │ MQTT
   ▼
Mosquitto
   │
   ▼
Telegraf
   │
   ▼
InfluxDB
   │
   ├──────────────► Grafana
   │
   └──────────────► UCM GIS / Leaflet
```

InfluxDB stores the environmental time-series data.

---

# InfluxDB

| Setting      | Value               |
| ------------ | ------------------- |
| Organization | `NYC-Mesh-Project`  |
| Bucket       | `Microclimate-Data` |
| Measurement  | `mqtt_consumer`     |

Environmental fields are stored as time-series measurements and associated with the node identifier.

Example query:

```flux
from(bucket: "Microclimate-Data")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "mqtt_consumer")
  |> filter(fn: (r) => r._field == "pm25")
```

---

# Node Runtime

Unlike the original Raspberry Pi implementation, the current sensor node does **not** run a Python virtual environment or a Linux system service.

The ESP32-P4 runs the UCM firmware directly.

Therefore, the following legacy Raspberry Pi node structure is no longer applicable:

```text
/opt/ucm/
├── sen54_mqtt.py
├── requirements.txt
├── sen54.service
└── venv/
```

The firmware is built and deployed using ESP-IDF.

Typical development operations include:

```bash
idf.py build
idf.py flash
idf.py monitor
```

---

# Reliability and Recovery

The node is designed for unattended operation.

The firmware should recover from:

* ESP32 reboot
* Wi-Fi disconnection
* Network interruption
* MQTT disconnection
* Temporary loss of the MQTT broker

After connectivity is restored, the node resumes publishing current sensor measurements.

## Current Limitation

The current firmware does **not** provide persistent store-and-forward telemetry.

A controlled outage test demonstrated that measurements generated during approximately five minutes of network interruption were not subsequently delivered to the hub.

Current behavior is therefore:

```text
Network available
      │
      ▼
Sensor → MQTT → Hub → InfluxDB
```

During an outage:

```text
Sensor → X → MQTT → Hub
```

Measurements generated during the outage are currently lost.

### Planned Improvement

Implement node-side telemetry buffering:

```text
Sensor
   │
   ▼
Local Buffer
   │
   ├── Network available ──► MQTT ──► Hub
   │
   └── Network unavailable
              │
              ▼
        Store measurements
              │
              ▼
        Reconnect
              │
              ▼
        Transmit backlog
```

The implementation should address:

* Maximum buffer size
* Persistent versus RAM buffering
* Measurement ordering
* Duplicate prevention
* Recovery after node reboot
* Maximum outage duration
* Backlog transmission rate

---

# Deployment Checklist

## Before Deployment

* [ ] Confirm current firmware builds successfully.
* [ ] Confirm SEN54 is detected.
* [ ] Confirm node ID.
* [ ] Confirm Wi-Fi credentials are provisioned.
* [ ] Confirm Ethernet operation if applicable.
* [ ] Confirm MQTT broker address.
* [ ] Confirm node publishes environmental data.
* [ ] Confirm node automatically reconnects after network interruption.
* [ ] Confirm UCM Hub is operational.
* [ ] Confirm InfluxDB is receiving data.
* [ ] Confirm dashboard/GIS displays the node.

## Field Deployment

1. Power the node.
2. Allow the node to boot.
3. Establish Ethernet or Wi-Fi connectivity.
4. Confirm the node receives an IP address.
5. Confirm the node can reach the UCM Hub.
6. Confirm MQTT connectivity.
7. Confirm environmental telemetry appears in InfluxDB.
8. Confirm the node appears in the UCM visualization.
9. Record the node ID and deployment location.
10. Allow the node to operate unattended.

## Field Test

The first NYC Mesh deployment should specifically test:

* Network acquisition
* MQTT connectivity
* Hub reachability
* Sensor telemetry
* Reconnection after network interruption
* Stability during extended operation
* Behavior when the node changes network conditions

The field test should establish a baseline before additional reliability features such as store-and-forward are implemented.

---

# Current UCM Node Status

The current UCM node architecture has demonstrated:

* ESP32-P4 firmware operation
* SEN54 environmental sensing
* Ethernet networking
* Wi-Fi networking
* Wi-Fi provisioning
* MQTT telemetry
* JSON telemetry formatting
* Node identification
* Automatic network reconnection
* Hub-side MQTT ingestion
* InfluxDB storage
* Grafana visualization
* Leaflet/GIS visualization
* Recovery after network interruption

The next major reliability enhancement is **store-and-forward telemetry** for measurements collected while the network is unavailable.
