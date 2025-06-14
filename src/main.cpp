#include "main.hpp"

#include <ArduinoOTA.h>
#include <IotWebConf.h>
#include <Preferences.h>

#include "bus.hpp"
#include "enhanced.hpp"
#include "http.hpp"
#include "mqtt.hpp"
#include "track.hpp"

#ifdef EBUS_INTERNAL
#include "schedule.hpp"
#endif

#ifdef ESP32
#include <ESPmDNS.h>
#include <IotWebConfESP32HTTPUpdateServer.h>
#include <esp_task_wdt.h>

#include "esp32c3/rom/rtc.h"

HTTPUpdateServer httpUpdater;
#else
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266TrueRandom.h>
#include <ESP8266mDNS.h>

ESP8266HTTPUpdateServer httpUpdater;
#endif

Preferences preferences;

// minimum time of reset pin
#define RESET_MS 1000

// PWM
#define PWM_CHANNEL 0
#define PWM_FREQ 10000
#define PWM_RESOLUTION 8

// mDNS
#define HOSTNAME "esp-eBus"

// IotWebConf
// adjust this if the iotwebconf structure has changed
#define CONFIG_VERSION "eeb"

#define STRING_LEN 64
#define NUMBER_LEN 8

#define DEFAULT_APMODE_PASS "ebusebus"
#define DEFAULT_AP "ebus-test"
#define DEFAULT_PASS "lectronz"

#define DUMMY_STATIC_IP "192.168.1.180"
#define DUMMY_GATEWAY "192.168.1.1"
#define DUMMY_NETMASK "255.255.255.0"

#define DUMMY_MQTT_SERVER DUMMY_GATEWAY
#define DUMMY_MQTT_USER "roger"
#define DUMMY_MQTT_PASS "password"

#ifdef ESP32
TaskHandle_t Task1;
#endif

char unique_id[7]{};

DNSServer dnsServer;

char staticIPValue[STRING_LEN];
char ipAddressValue[STRING_LEN];
char gatewayValue[STRING_LEN];
char netmaskValue[STRING_LEN];

uint32_t pwm;
char pwm_value[NUMBER_LEN];

char ebus_address[NUMBER_LEN];
static char ebus_address_values[][NUMBER_LEN] = {
    "00", "10", "30", "70", "f0", "01", "11", "31", "71",
    "f1", "03", "13", "33", "73", "f3", "07", "17", "37",
    "77", "f7", "0f", "1f", "3f", "7f", "ff"};

char command_distance[NUMBER_LEN];

char mqtt_server[STRING_LEN];
char mqtt_user[STRING_LEN];
char mqtt_pass[STRING_LEN];
char mqttPublishCountersValue[STRING_LEN];

char haSupportValue[STRING_LEN];

IotWebConf iotWebConf(HOSTNAME, &dnsServer, &configServer, DEFAULT_APMODE_PASS,
                      CONFIG_VERSION);

iotwebconf::ParameterGroup connGroup =
    iotwebconf::ParameterGroup("conn", "Connection parameters");
iotwebconf::CheckboxParameter staticIPParam = iotwebconf::CheckboxParameter(
    "Static IP", "staticIPParam", staticIPValue, STRING_LEN);
iotwebconf::TextParameter ipAddressParam = iotwebconf::TextParameter(
    "IP address", "ipAddress", ipAddressValue, STRING_LEN, "", DUMMY_STATIC_IP);
iotwebconf::TextParameter gatewayParam = iotwebconf::TextParameter(
    "Gateway", "gateway", gatewayValue, STRING_LEN, "", DUMMY_GATEWAY);
iotwebconf::TextParameter netmaskParam = iotwebconf::TextParameter(
    "Subnet mask", "netmask", netmaskValue, STRING_LEN, "", DUMMY_NETMASK);

iotwebconf::ParameterGroup ebusGroup =
    iotwebconf::ParameterGroup("ebus", "eBUS configuration");
iotwebconf::NumberParameter pwmParam =
    iotwebconf::NumberParameter("PWM value", "pwm_value", pwm_value, NUMBER_LEN,
                                "130", "1..255", "min='1' max='255' step='1'");
#ifdef EBUS_INTERNAL
iotwebconf::SelectParameter ebusAddressParam = iotwebconf::SelectParameter(
    "eBUS address", "ebus_address", ebus_address, NUMBER_LEN,
    reinterpret_cast<char*>(ebus_address_values),
    reinterpret_cast<char*>(ebus_address_values),
    sizeof(ebus_address_values) / NUMBER_LEN, NUMBER_LEN, "ff");
iotwebconf::NumberParameter commandDistanceParam = iotwebconf::NumberParameter(
    "Command distance", "command_distance", command_distance, NUMBER_LEN, "2",
    "0..60", "min='0' max='60' step='1'");
#endif

iotwebconf::ParameterGroup mqttGroup =
    iotwebconf::ParameterGroup("mqtt", "MQTT configuration");
iotwebconf::TextParameter mqttServerParam =
    iotwebconf::TextParameter("MQTT server", "mqtt_server", mqtt_server,
                              STRING_LEN, "", DUMMY_MQTT_SERVER);
iotwebconf::TextParameter mqttUserParam = iotwebconf::TextParameter(
    "MQTT user", "mqtt_user", mqtt_user, STRING_LEN, "", DUMMY_MQTT_USER);
iotwebconf::PasswordParameter mqttPasswordParam = iotwebconf::PasswordParameter(
    "MQTT password", "mqtt_pass", mqtt_pass, STRING_LEN, "", DUMMY_MQTT_PASS);
iotwebconf::CheckboxParameter mqttPublishCountersParam =
    iotwebconf::CheckboxParameter("Publish Counters to MQTT",
                                  "mqttPublishCountersParam",
                                  mqttPublishCountersValue, STRING_LEN);

iotwebconf::ParameterGroup haGroup =
    iotwebconf::ParameterGroup("ha", "Home Assistant configuration");
iotwebconf::CheckboxParameter haSupportParam = iotwebconf::CheckboxParameter(
    "Home Assistant support", "haSupportParam", haSupportValue, STRING_LEN);

IPAddress ipAddress;
IPAddress gateway;
IPAddress netmask;

WiFiServer wifiServer(3333);
WiFiServer wifiServerRO(3334);
WiFiServer wifiServerEnh(3335);
WiFiServer statusServer(5555);
WiFiClient serverClients[MAX_SRV_CLIENTS];
WiFiClient serverClientsRO[MAX_SRV_CLIENTS];
WiFiClient enhClients[MAX_SRV_CLIENTS];

uint32_t last_comms = 0;

bool needMqttConnect = false;
uint32_t lastMqttConnectionAttempt = 0;
uint32_t lastMqttUpdate = 0;

// status
uint32_t reset_code = 0;
Track<uint32_t> uptime("state/uptime", 10);
Track<uint32_t> loopDuration("state/loop_duration", 10);
uint32_t maxLoopDuration;
Track<uint32_t> free_heap("state/free_heap", 10);

// wifi
uint32_t last_connect = 0;
int reconnect_count = 0;

bool connectMqtt() {
  if (mqtt.connected()) return true;

  if (1000 > millis() - lastMqttConnectionAttempt) return false;

  mqtt.connect();

  if (!mqtt.connected()) {
    lastMqttConnectionAttempt = millis();
    return false;
  }

  return true;
}

void wifiConnected() {
  last_connect = millis();
  ++reconnect_count;
  needMqttConnect = true;
}

void wdt_start() {
#ifdef ESP32
  esp_task_wdt_init(6, true);
#else
  ESP.wdtDisable();
#endif
}

void wdt_feed() {
#ifdef ESP32
  esp_task_wdt_reset();
#else
  ESP.wdtFeed();
#endif
}

inline void disableTX() {
#ifdef TX_DISABLE_PIN
  pinMode(TX_DISABLE_PIN, OUTPUT);
  digitalWrite(TX_DISABLE_PIN, HIGH);
#endif
}

inline void enableTX() {
#ifdef TX_DISABLE_PIN
  digitalWrite(TX_DISABLE_PIN, LOW);
#endif
}

void set_pwm(uint8_t value) {
#ifdef PWM_PIN
  ledcWrite(PWM_CHANNEL, value);
#ifdef EBUS_INTERNAL
  schedule.resetCounters();
#endif
#endif
}

uint32_t get_pwm() {
#ifdef PWM_PIN
  return ledcRead(PWM_CHANNEL);
#else
  return 0;
#endif
}

void calcUniqueId() {
  uint32_t id = 0;
#ifdef ESP32
  for (int i = 0; i < 6; ++i) {
    id |= ((ESP.getEfuseMac() >> (8 * (5 - i))) & 0xff) << (8 * i);
  }
#else
  id = ESP.getChipId();
#endif
  char tmp[9]{};
  snprintf(tmp, sizeof(tmp), "%08x", id);
  strncpy(unique_id, &tmp[2], 6);
}

void restart() {
  disableTX();
  ESP.restart();
}

void check_reset() {
  // check if RESET_PIN being hold low and reset
  pinMode(RESET_PIN, INPUT_PULLUP);
  uint32_t resetStart = millis();
  while (digitalRead(RESET_PIN) == 0) {
    if (millis() > resetStart + RESET_MS) {
      preferences.clear();
      restart();
    }
  }
}

void loop_duration() {
  static uint32_t lastTime = 0;
  uint32_t now = micros();
  uint32_t delta = now - lastTime;
  float alpha = 0.3;

  lastTime = now;

  loopDuration = ((1 - alpha) * loopDuration.value() + (alpha * delta));

  if (delta > maxLoopDuration) {
    maxLoopDuration = delta;
  }
}

void data_process() {
  loop_duration();

  // check clients for data
  for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
    handleClient(&serverClients[i]);
    handleEnhClient(&enhClients[i]);
  }

#ifdef EBUS_INTERNAL
  // check schedule for data
  schedule.nextCommand();
#endif

  // check queue for data
  BusType::data d;
  if (Bus.read(d)) {
#ifdef EBUS_INTERNAL
    if (!d._enhanced) {
      schedule.processData(d._d);
      last_comms = millis();
    }
#endif

    for (int i = 0; i < MAX_SRV_CLIENTS; i++) {
      if (d._enhanced) {
        if (d._client == &enhClients[i]) {
          if (pushEnhClient(&enhClients[i], d._c, d._d, true)) {
            last_comms = millis();
          }
        }
      } else {
        if (pushClient(&serverClients[i], d._d)) {
          last_comms = millis();
        }
        if (pushClient(&serverClientsRO[i], d._d)) {
          last_comms = millis();
        }
        if (d._client != &enhClients[i]) {
          if (pushEnhClient(&enhClients[i], d._c, d._d,
                            d._logtoclient == &enhClients[i])) {
            last_comms = millis();
          }
        }
      }
    }
  }
}

void data_loop(void* pvParameters) {
  while (1) {
    data_process();
  }
}

bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper) {
  bool valid = true;

  if (webRequestWrapper->arg(staticIPParam.getId()).equals("selected")) {
    if (!ipAddress.fromString(webRequestWrapper->arg(ipAddressParam.getId()))) {
      ipAddressParam.errorMessage = "Please provide a valid IP address!";
      valid = false;
    }
    if (!netmask.fromString(webRequestWrapper->arg(netmaskParam.getId()))) {
      netmaskParam.errorMessage = "Please provide a valid netmask!";
      valid = false;
    }
    if (!gateway.fromString(webRequestWrapper->arg(gatewayParam.getId()))) {
      gatewayParam.errorMessage = "Please provide a valid gateway address!";
      valid = false;
    }
  }

  if (webRequestWrapper->arg(mqttServerParam.getId()).length() >
      STRING_LEN - 1) {
    String tmp = "max. ";
    tmp += String(STRING_LEN);
    tmp += " characters allowed";
    mqttServerParam.errorMessage = tmp.c_str();
    valid = false;
  }

  return valid;
}

void saveParamsCallback() {
  set_pwm(atoi(pwm_value));
  pwm = get_pwm();

#ifdef EBUS_INTERNAL
  schedule.setAddress(uint8_t(std::strtoul(ebus_address, nullptr, 16)));
  schedule.setDistance(atoi(command_distance));
#endif

  if (mqtt_server[0] != '\0') mqtt.setServer(mqtt_server, 1883);

  if (mqtt_user[0] != '\0') mqtt.setCredentials(mqtt_user, mqtt_pass);

  schedule.setPublishCounters(mqttPublishCountersParam.isChecked());

  mqtt.setHASupport(haSupportParam.isChecked());
  mqtt.publishHA();
}

void connectWifi(const char* ssid, const char* password) {
  if (staticIPParam.isChecked()) {
    bool valid = true;
    valid = valid && ipAddress.fromString(String(ipAddressValue));
    valid = valid && netmask.fromString(String(netmaskValue));
    valid = valid && gateway.fromString(String(gatewayValue));

    if (valid) WiFi.config(ipAddress, gateway, netmask);
  }

  WiFi.begin(ssid, password);
}

char* status_string() {
  size_t bufferSize = 2048;
  char* status = new char[bufferSize];
  if (!status) return nullptr;

  int pos = 0;

  pos += snprintf(status + pos, bufferSize - pos, "async_mode: %s\n",
                  USE_ASYNCHRONOUS ? "true" : "false");
  pos += snprintf(status + pos, bufferSize - pos, "software_serial_mode: %s\n",
                  USE_SOFTWARE_SERIAL ? "true" : "false");
  pos += snprintf(status + pos, bufferSize - pos, "unique_id: %s\n", unique_id);
  pos += snprintf(status + pos, bufferSize - pos, "uptime: %ld ms\n", millis());
  pos += snprintf(status + pos, bufferSize - pos, "last_connect_time: %u ms\n",
                  last_connect);
  pos += snprintf(status + pos, bufferSize - pos, "reconnect_count: %d \n",
                  reconnect_count);
  pos +=
      snprintf(status + pos, bufferSize - pos, "rssi: %d dBm\n", WiFi.RSSI());
  pos += snprintf(status + pos, bufferSize - pos, "free_heap: %u B\n",
                  free_heap.value());
  pos +=
      snprintf(status + pos, bufferSize - pos, "reset_code: %u\n", reset_code);
  pos += snprintf(status + pos, bufferSize - pos, "loop_duration: %u us\r\n",
                  loopDuration.value());
  pos += snprintf(status + pos, bufferSize - pos,
                  "max_loop_duration: %u us\r\n", maxLoopDuration);
  pos +=
      snprintf(status + pos, bufferSize - pos, "version: %s\r\n", AUTO_VERSION);

  pos +=
      snprintf(status + pos, bufferSize - pos, "pwm_value: %u\r\n", get_pwm());

#ifdef EBUS_INTERNAL
  pos += snprintf(status + pos, bufferSize - pos, "ebus_address: %s\r\n",
                  ebus_address);
  pos += snprintf(status + pos, bufferSize - pos, "command_distance: %i\r\n",
                  atoi(command_distance));
  pos += snprintf(status + pos, bufferSize - pos, "active_commands: %u\r\n",
                  store.getActiveCommands());
  pos += snprintf(status + pos, bufferSize - pos, "passive_commands: %u\r\n",
                  store.getPassiveCommands());
#endif

  pos += snprintf(status + pos, bufferSize - pos, "mqtt_connected: %s\r\n",
                  mqtt.connected() ? "true" : "false");
  pos += snprintf(status + pos, bufferSize - pos, "mqtt_server: %s\r\n",
                  mqtt_server);
  pos +=
      snprintf(status + pos, bufferSize - pos, "mqtt_user: %s\r\n", mqtt_user);

#ifdef EBUS_INTERNAL
  pos +=
      snprintf(status + pos, bufferSize - pos, "mqtt_publish_counters: %s\r\n",
               mqttPublishCountersParam.isChecked() ? "true" : "false");
#endif

  pos += snprintf(status + pos, bufferSize - pos, "ha_support: %s\r\n",
                  haSupportParam.isChecked() ? "true" : "false");

  if (pos >= bufferSize) status[bufferSize - 1] = '\0';

  return status;
}

const std::string getAdapterJson() {
  std::string payload;
  JsonDocument doc;

  doc["Unique_ID"] = unique_id;

  // Firmware
  JsonObject Firmware = doc["Firmware"].to<JsonObject>();
  Firmware["Version"] = AUTO_VERSION;
  Firmware["SDK"] = ESP.getSdkVersion();
  Firmware["Async"] = USE_ASYNCHRONOUS ? true : false;
  Firmware["Software_Serial"] = USE_SOFTWARE_SERIAL ? true : false;

  // Settings
  JsonObject Settings = doc["Settings"].to<JsonObject>();
  Settings["PWM"] = get_pwm();
#ifdef EBUS_INTERNAL
  Settings["Ebus_Address"] = ebus_address;
  Settings["Command_Distance"] = atoi(command_distance);
  Settings["Active_Commands"] = store.getActiveCommands();
  Settings["Passive_Commands"] = store.getPassiveCommands();
#endif

  // Mqtt
  JsonObject Mqtt = doc["Mqtt"].to<JsonObject>();
  Mqtt["Server"] = mqtt_server;
  Mqtt["User"] = mqtt_user;
  Mqtt["Connected"] = mqtt.connected();
#ifdef EBUS_INTERNAL
  Mqtt["Publish_Counters"] = mqttPublishCountersParam.isChecked();
#endif

  // HomeAssistant
  JsonObject HomeAssistant = doc["Home_Assistant"].to<JsonObject>();
  HomeAssistant["Support"] = haSupportParam.isChecked();

  doc.shrinkToFit();
  serializeJson(doc, payload);

  return payload;
}

const std::string getStatusJson() {
  std::string payload;
  JsonDocument doc;

  doc["Reset_Code"] = reset_code;
  doc["Uptime"] = uptime.value();
  doc["Loop_Duration"] = loopDuration.value();
  doc["Loop_Duration_Max"] = maxLoopDuration;
  doc["Free_Heap"] = free_heap.value();

  // WIFI
  JsonObject WIFI = doc["WIFI"].to<JsonObject>();
  WIFI["Last_Connect"] = last_connect;
  WIFI["Reconnect_Count"] = reconnect_count;
  WIFI["RSSI"] = WiFi.RSSI();

  // Arbitration
  JsonObject Arbitration = doc["Arbitration"].to<JsonObject>();
  Arbitration["Total"] = static_cast<int>(Bus._nbrArbitrations);
  Arbitration["Restarts1"] = static_cast<int>(Bus._nbrRestarts1);
  Arbitration["Restarts2"] = static_cast<int>(Bus._nbrRestarts2);
  Arbitration["Won1"] = static_cast<int>(Bus._nbrWon1);
  Arbitration["Won2"] = static_cast<int>(Bus._nbrWon2);
  Arbitration["Lost1"] = static_cast<int>(Bus._nbrLost1);
  Arbitration["Lost2"] = static_cast<int>(Bus._nbrLost2);
  Arbitration["Late"] = static_cast<int>(Bus._nbrLate);
  Arbitration["Errors"] = static_cast<int>(Bus._nbrErrors);

  doc.shrinkToFit();
  serializeJson(doc, payload);

  return payload;
}

bool handleStatusServerRequests() {
  if (!statusServer.hasClient()) return false;

  WiFiClient client = statusServer.accept();

  if (client.availableForWrite() >= AVAILABLE_THRESHOLD) {
    client.print(status_string());
    client.flush();
    client.stop();
  }
  return true;
}

void setup() {
  preferences.begin("esp-ebus", false);

  check_reset();

#ifdef ESP32
  reset_code = rtc_get_reset_reason(0);
#else
  reset_code = ESP.getResetInfoPtr()->reason;
#endif

  calcUniqueId();

  Bus.begin();

  DebugSer.begin(115200);
  DebugSer.setDebugOutput(true);

  disableTX();

#ifdef PWM_PIN
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
#endif

  // IotWebConf
  connGroup.addItem(&staticIPParam);
  connGroup.addItem(&ipAddressParam);
  connGroup.addItem(&gatewayParam);
  connGroup.addItem(&netmaskParam);

  ebusGroup.addItem(&pwmParam);

#ifdef EBUS_INTERNAL
  ebusGroup.addItem(&ebusAddressParam);
  ebusGroup.addItem(&commandDistanceParam);
#endif

  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserParam);
  mqttGroup.addItem(&mqttPasswordParam);
#ifdef EBUS_INTERNAL
  mqttGroup.addItem(&mqttPublishCountersParam);
#endif

  haGroup.addItem(&haSupportParam);

  iotWebConf.addParameterGroup(&connGroup);
  iotWebConf.addParameterGroup(&ebusGroup);
  iotWebConf.addParameterGroup(&mqttGroup);
  iotWebConf.addParameterGroup(&haGroup);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setConfigSavedCallback(&saveParamsCallback);
  iotWebConf.getApTimeoutParameter()->visible = true;
  iotWebConf.setWifiConnectionTimeoutMs(7000);
  iotWebConf.setWifiConnectionHandler(&connectWifi);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);

#ifdef STATUS_LED_PIN
  iotWebConf.setStatusPin(STATUS_LED_PIN);
#endif

  if (preferences.getBool("firstboot", true)) {
    preferences.putBool("firstboot", false);

    iotWebConf.init();
    strncpy(iotWebConf.getApPasswordParameter()->valueBuffer,
            DEFAULT_APMODE_PASS, IOTWEBCONF_WORD_LEN);
    strncpy(iotWebConf.getWifiSsidParameter()->valueBuffer, DEFAULT_AP,
            IOTWEBCONF_WORD_LEN);
    strncpy(iotWebConf.getWifiPasswordParameter()->valueBuffer, DEFAULT_PASS,
            IOTWEBCONF_WORD_LEN);
    iotWebConf.saveConfig();
  } else {
    iotWebConf.skipApStartup();
    // -- Initializing the configuration.
    iotWebConf.init();
  }

  SetupHttpHandlers();

  iotWebConf.setupUpdateServer(
      [](const char* updatePath) {
        httpUpdater.setup(&configServer, updatePath);
      },
      [](const char* userName, char* password) {
        httpUpdater.updateCredentials(userName, password);
      });

  set_pwm(atoi(pwm_value));

#ifdef EBUS_INTERNAL
  schedule.setAddress(uint8_t(std::strtoul(ebus_address, nullptr, 16)));
  schedule.setDistance(atoi(command_distance));
#endif

  while (iotWebConf.getState() != iotwebconf::NetworkState::OnLine) {
    iotWebConf.doLoop();
  }

  mqtt.setUniqueId(unique_id);
  if (mqtt_server[0] != '\0') mqtt.setServer(mqtt_server, 1883);
  if (mqtt_user[0] != '\0') mqtt.setCredentials(mqtt_user, mqtt_pass);

  schedule.setPublishCounters(mqttPublishCountersParam.isChecked());

  mqtt.setHASupport(haSupportParam.isChecked());

  wifiServer.begin();
  wifiServerRO.begin();
  wifiServerEnh.begin();
  statusServer.begin();

#ifdef ESP32
  ArduinoOTA.onStart([]() { vTaskDelete(Task1); });
#endif

  ArduinoOTA.begin();
  MDNS.begin(HOSTNAME);
  wdt_start();

  last_comms = millis();
  enableTX();

#ifdef EBUS_INTERNAL
  store.loadCommands();  // install saved commands
  mqtt.publishHASensors(false);
#endif

#ifdef ESP32
  xTaskCreate(data_loop, "data_loop", 10000, NULL, 1, &Task1);
#endif
}

void loop() {
  ArduinoOTA.handle();

#ifdef ESP8266
  MDNS.update();
  data_process();
#endif

  wdt_feed();

#ifdef ESP32
  iotWebConf.doLoop();
#endif

  if (needMqttConnect) {
    if (connectMqtt()) needMqttConnect = false;

  } else if ((iotWebConf.getState() == iotwebconf::OnLine) &&
             (!mqtt.connected())) {
    needMqttConnect = true;
  }

  if (mqtt.connected()) {
    uint32_t currentMillis = millis();
    if (currentMillis > lastMqttUpdate + 5 * 1000) {
      lastMqttUpdate = currentMillis;

#ifdef EBUS_INTERNAL
      schedule.fetchCounters();
#endif
    }
#ifdef EBUS_INTERNAL
    mqtt.doLoop();
#endif
  }

  uptime = millis();
  free_heap = ESP.getFreeHeap();

  if (millis() > last_comms + 200 * 1000) {
    restart();
  }

  // Check if new client on the status server
  if (handleStatusServerRequests()) {
    // exclude handleStatusServerRequests from maxLoopDuration calculation
    // as it skews the typical loop duration and set maxLoopDuration to 0
    loop_duration();
    maxLoopDuration = 0;
  }

  // Check if there are any new clients on the eBUS servers
  handleNewClient(&wifiServer, serverClients);
  handleNewClient(&wifiServerEnh, enhClients);
  handleNewClient(&wifiServerRO, serverClientsRO);
}
