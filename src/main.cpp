/*
  Libraries :
    - ESP8266 core for Arduino :  https://github.com/esp8266/Arduino
    - ESP8266httpUpdate:          auto update esp8266 sketch
    - PubSubClient:               https://github.com/knolleary/pubsubclient

  Schematic Sonoff:
    - VCC (Sonoff) -> VCC (FTDI)
    - RX  (Sonoff) -> TX  (FTDI)
    - TX  (Sonoff) -> RX  (FTDI)
    - GND (Sonoff) -> GND (FTDI)
*/

#include <ESP8266WiFi.h>        // https://github.com/esp8266/Arduino
#include <ESP8266httpUpdate.h>  // ota update for esp8266
#include <PubSubClient.h>       // https://github.com/knolleary/pubsubclient/releases/tag/v2.6
#include "config.h"

// macros for debugging
#ifdef DEBUG
  #define         DEBUG_PRINT(x)    Serial.print(x)
  #define         DEBUG_PRINTLN(x)  Serial.println(x)
#else
  #define         DEBUG_PRINT(x)
  #define         DEBUG_PRINTLN(x)
#endif

// Board properties
#define           FW_VERSION                "1.0"

// MQTT
#define           MQTT_ON_PAYLOAD           "1"
#define           MQTT_OFF_PAYLOAD          "0"
#define           MQTT_ENDPOINT_SIZE        30

#define           MQTT_TOPIC_BASE           ""
#define           MQTT_TOPIC_SYSTEM_VERSION "system/version"
#define           MQTT_TOPIC_SYSTEM_UPDATE  "system/update"
#define           MQTT_TOPIC_SYSTEM_RESET   "system/reset"

#define           MQTT_TOPIC_RELAY_POWER    "relay/power"
#define           MQTT_TOPIC_RELAY_STATE    "relay/state"
#define           MQTT_TOPIC_SENSOR_STATE   "sensor/state"

char              MQTT_CLIENT_ID[7]                               = {0};
char              MQTT_ENDPOINT_SYS_VERSION[MQTT_ENDPOINT_SIZE]   = {0};
char              MQTT_ENDPOINT_SYS_UPDATE[MQTT_ENDPOINT_SIZE]    = {0};
char              MQTT_ENDPOINT_SYS_RESET[MQTT_ENDPOINT_SIZE]     = {0};

char              MQTT_ENDPOINT_RELAY_POWER[MQTT_ENDPOINT_SIZE]   = {0};
char              MQTT_ENDPOINT_RELAY_STATE[MQTT_ENDPOINT_SIZE]   = {0};
char              MQTT_ENDPOINT_SENSOR_STATE[MQTT_ENDPOINT_SIZE]  = {0};

enum CMD {
  CMD_NOT_DEFINED,
  CMD_BUTTON_CHANGED,
  CMD_SENSOR_CHANGED,
};
volatile uint8_t cmd = CMD_NOT_DEFINED;

uint8_t           relayState                                        = HIGH;  // HIGH: closed switch
uint8_t           buttonState                                       = HIGH; // HIGH: opened switch
uint8_t           currentButtonState                                = buttonState;
long              buttonStartPressed                                = 0;
long              buttonDurationPressed                             = 0;

#ifdef DEBUG
  unsigned long lastPing                       = 0;
#endif

#ifdef TLS
WiFiClientSecure  wifiClient;
#else
WiFiClient        wifiClient;
#endif
PubSubClient      mqttClient(wifiClient);

// PREDEFINED FUNCTIONS
void reconnect();
void updateFW(char* fwUrl);
void reset();
void publishData(char* topic, char* payload);
void publishState(char* topic, int state);
void setRelayState(uint8_t _relayState);

///////////////////////////////////////////////////////////////////////////
//   MQTT with SSL/TLS
///////////////////////////////////////////////////////////////////////////
/*
  Function called to verify the fingerprint of the MQTT server certificate
 */
#ifdef TLS
void verifyFingerprint() {
  DEBUG_PRINT(F("INFO: Connecting to "));
  DEBUG_PRINTLN(MQTT_SERVER);

  if (!wifiClient.connect(MQTT_SERVER, atoi(MQTT_PORT))) {
    DEBUG_PRINTLN(F("ERROR: Connection failed. Halting execution"));
    reset();
  }

  if (wifiClient.verify(MQTT_FINGERPRINT, MQTT_SERVER)) {
    DEBUG_PRINTLN(F("INFO: Connection secure"));
  } else {
    DEBUG_PRINTLN(F("ERROR: Connection insecure! Halting execution"));
    reset();
  }
}
#endif

///////////////////////////////////////////////////////////////////////////
//   MQTT
///////////////////////////////////////////////////////////////////////////
/*
   Function called when a MQTT message arrived
   @param topic   The topic of the MQTT message
   @param _payload The payload of the MQTT message
   @param length  The length of the payload
*/
void callback(char* topic, byte* _payload, unsigned int length) {
  // handle the MQTT topic of the received message
  _payload[length] = '\0';
  char* payload = (char*) _payload;
  if (strcmp(topic, MQTT_ENDPOINT_RELAY_POWER)==0) {
    if (strcmp(payload, MQTT_ON_PAYLOAD)==0) {
      setRelayState(HIGH);
    }
    if (strcmp(payload, MQTT_OFF_PAYLOAD)==0) {
      setRelayState(LOW);
    }
  } else if (strcmp(topic, MQTT_ENDPOINT_SYS_RESET)==0) {
    if (strcmp(payload, MQTT_ON_PAYLOAD)==0) {
      publishState(MQTT_ENDPOINT_SYS_RESET, LOW);
      reset();
    }
  } else if (strcmp(topic, MQTT_ENDPOINT_SYS_UPDATE)==0) {
    if (length > 0) {
      updateFW(payload);
    }
  }
}

/*
  Function called to pulish payload data
*/
void publishData(char* topic, char* payload) {
  if (mqttClient.publish(topic, payload, true)) {
    DEBUG_PRINT(F("INFO: MQTT message publish succeeded. Topic: "));
    DEBUG_PRINT(topic);
    DEBUG_PRINT(F(" Payload: "));
    DEBUG_PRINTLN(payload);
  } else {
    DEBUG_PRINTLN(F("ERROR: MQTT message publish failed, either connection lost, or message too large"));
  }
}

/*
  Function called to publish a state
*/
void publishState(char* topic, int state) {
  publishData(topic, (char*) (state == 1 ? MQTT_ON_PAYLOAD : MQTT_OFF_PAYLOAD));
}

/*
  Function called to connect/reconnect to the MQTT broker
 */
void reconnect() {
  // test if the module has an IP address
  // if not, restart the module
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN(F("ERROR: The module isn't connected to the internet"));
    reset();
  }

  // try to connect to the MQTT broker
  while (!mqttClient.connected()) {
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      DEBUG_PRINTLN(F("INFO: The client is successfully connected to the MQTT broker"));
    } else {
      DEBUG_PRINTLN(F("ERROR: The connection to the MQTT broker failed"));
      delay(1000);
    }
  }

  mqttClient.subscribe(MQTT_ENDPOINT_SYS_RESET);
  mqttClient.subscribe(MQTT_ENDPOINT_SYS_UPDATE);

  mqttClient.subscribe(MQTT_ENDPOINT_RELAY_POWER);
}

///////////////////////////////////////////////////////////////////////////
//   Sonoff switch
///////////////////////////////////////////////////////////////////////////
/*
 Function called to set the state of the relay
 */
void setRelayState(uint8_t _relayState) {
  if (relayState == _relayState)
    return;
  relayState = _relayState;
  digitalWrite(PIN_RELAY, _relayState);
  digitalWrite(PIN_LED, (_relayState + 1) % 2);
  publishState(MQTT_ENDPOINT_RELAY_STATE, _relayState);
}

///////////////////////////////////////////////////////////////////////////
//   SYSTEM
///////////////////////////////////////////////////////////////////////////
/*
 Function called to update chip firmware
 */
void updateFW(char* fwUrl) {
  DEBUG_PRINTLN(F("INFO: updating firmware ..."));
  t_httpUpdate_return ret = ESPhttpUpdate.update(fwUrl);
#ifdef DEBUG
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      DEBUG_PRINT(F("ERROR: firmware update failed: "));
      DEBUG_PRINTLN(ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      DEBUG_PRINTLN(F("ERROR: no new firmware"));
      break;
    case HTTP_UPDATE_OK:
      DEBUG_PRINTLN(F("Info: firmware update ok"));
      break;
  }
#endif
}

/*
  Function called to reset / restart the switch
 */
void reset() {
  DEBUG_PRINTLN(F("INFO: Reset..."));
  ESP.reset();
  delay(1000);
}

///////////////////////////////////////////////////////////////////////////
//   ISR
///////////////////////////////////////////////////////////////////////////
/*
  Function called when the button is pressed
 */
void buttonInterrupt() {
  cmd = CMD_BUTTON_CHANGED;
}

/*
  Fuction called when the sensor (pir, reed) is triggered
*/
#ifdef SENSOR
void sensorInterrupt() {
  cmd = CMD_SENSOR_CHANGED;
}
#endif

/*
  Function called to connect to Wifi
 */
void connectWiFi() {
  // Connect to WiFi network
  DEBUG_PRINT(F("INFO: Connecting to "));
  DEBUG_PRINTLN(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
  }
  DEBUG_PRINTLN(F(""));
  DEBUG_PRINTLN(F("INFO: WiFi connected"));
}

///////////////////////////////////////////////////////////////////////////
//   Setup() and loop()
///////////////////////////////////////////////////////////////////////////
void setup() {
#ifdef DEBUG
  Serial.begin(115200);
#endif
  DEBUG_PRINTLN(F(""));
  DEBUG_PRINTLN(F(""));
  DEBUG_PRINTLN(F("Info: booted"));

  // init the I/O
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUTTON, INPUT);
  attachInterrupt(PIN_BUTTON, buttonInterrupt, CHANGE);

#ifdef SENSOR
  pinMode(PIN_SENSOR, INPUT);
  attachInterrupt(PIN_SENSOR, sensorInterrupt, CHANGE);
#endif

  // switch on led and relay off
  digitalWrite(PIN_LED, HIGH);
  digitalWrite(PIN_RELAY, LOW);

  // connect to wifi
  connectWiFi();

  // get the Chip ID of the switch and use it as the MQTT client ID
  sprintf(MQTT_CLIENT_ID, "%06X", ESP.getChipId());
  DEBUG_PRINT(F("INFO: MQTT client ID/Hostname: "));
  DEBUG_PRINTLN(MQTT_CLIENT_ID);

  // create MQTT endpoints
  sprintf(MQTT_ENDPOINT_SYS_VERSION, "%s%06X/%s", MQTT_TOPIC_BASE, ESP.getChipId(), MQTT_TOPIC_SYSTEM_VERSION);
  sprintf(MQTT_ENDPOINT_SYS_UPDATE, "%s%06X/%s", MQTT_TOPIC_BASE, ESP.getChipId(), MQTT_TOPIC_SYSTEM_UPDATE);
  sprintf(MQTT_ENDPOINT_SYS_RESET, "%s%06X/%s", MQTT_TOPIC_BASE, ESP.getChipId(), MQTT_TOPIC_SYSTEM_RESET);

  sprintf(MQTT_ENDPOINT_RELAY_POWER, "%s%06X/%s", MQTT_TOPIC_BASE, ESP.getChipId(), MQTT_TOPIC_RELAY_POWER);
  sprintf(MQTT_ENDPOINT_RELAY_STATE, "%s%06X/%s", MQTT_TOPIC_BASE, ESP.getChipId(), MQTT_TOPIC_RELAY_STATE);
  sprintf(MQTT_ENDPOINT_SENSOR_STATE, "%s%06X/%s", MQTT_TOPIC_BASE, ESP.getChipId(), MQTT_TOPIC_SENSOR_STATE);

#ifdef TLS
  // check the fingerprint of io.adafruit.com's SSL cert
  verifyFingerprint();
#endif

  // configure MQTT
  mqttClient.setServer(MQTT_SERVER, atoi(MQTT_PORT));
  mqttClient.setCallback(callback);

  // connect to the MQTT broker
  reconnect();

  // publish running firmware version
  publishData(MQTT_ENDPOINT_SYS_VERSION, (char*) FW_VERSION);

  digitalWrite(PIN_LED, LOW);
}


void loop() {
  // keep the MQTT client connected to the broker
  if (!mqttClient.connected()) {
    reconnect();
  }
  mqttClient.loop();

#ifdef DEBUG
  // ping every 30 seconds the current version to server
  if (lastPing == 0 || millis() - lastPing >= 1000 * 10){
    lastPing = millis();
    publishData(MQTT_ENDPOINT_SYS_VERSION, (char*) FW_VERSION);
  }
#endif

  yield();

  switch (cmd) {
    case CMD_NOT_DEFINED:
      // do nothing
      break;
    case CMD_BUTTON_CHANGED:
      currentButtonState = digitalRead(PIN_BUTTON);
      if (buttonState != currentButtonState) {
        // tests if the button is released or pressed
        if (buttonState == LOW && currentButtonState == HIGH) {
          buttonDurationPressed = millis() - buttonStartPressed;
          if (buttonDurationPressed < 500) {
            setRelayState(relayState == HIGH ? LOW : HIGH);
          } else {
            reset();
          }
        } else if (buttonState == HIGH && currentButtonState == LOW) {
          buttonStartPressed = millis();
        }
        buttonState = currentButtonState;
      }
      cmd = CMD_NOT_DEFINED;
      break;
    case CMD_SENSOR_CHANGED:
      publishState(MQTT_ENDPOINT_SENSOR_STATE, digitalRead(PIN_SENSOR));
      cmd = CMD_NOT_DEFINED;
      break;
  }

  yield();
}
