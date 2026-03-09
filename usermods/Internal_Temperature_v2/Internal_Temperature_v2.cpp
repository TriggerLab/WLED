#include "wled.h"

#ifdef ARDUINO_ARCH_ESP32
  #include <HTTPClient.h>
#else
  #include <ESP8266HTTPClient.h>
#endif

class InternalTemperatureUsermod : public Usermod {
private:
  static constexpr unsigned long minLoopInterval = 1000;
  unsigned long loopInterval = 10000;
  unsigned long lastTime = 0;
  bool isEnabled = false;

  float temperature = 0.0f;

  uint8_t previousPlaylist = 0;
  uint8_t previousPreset = 0;
  uint8_t savedPresetToActivate = 0;

  float activationThreshold = 95.0f;
  float resetMargin = 2.0f;

  bool isAboveThreshold = false;

  bool webhookEnabled = false;
  String webhookUrl = "";
  bool webhookAppendParams = true;
  float webhookThreshold = 95.0f;
  bool webhookTriggered = false;

  bool oneTimeDisable = false;
  bool blockRestore = false;

  static const char _name[];
  static const char _enabled[];
  static const char _loopInterval[];
  static const char _activationThreshold[];
  static const char _presetToActivate[];
  static const char _webhookUrl[];
  static const char _webhookEnabled[];
  static const char _webhookAppendParams[];
  static const char _webhookThreshold[];
  static const char _oneTimeDisable[]; // ключ конфигурации

  void publishMqtt(const char *state, bool retain = false);

  void sendWebhookIfConfigured() {
    if (!webhookEnabled) return;
	webhookUrl.trim();
    if (webhookUrl.length() == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;

    String call = webhookUrl;

    if (webhookAppendParams) {
      String sep = "";
      if (call.endsWith("?") || call.endsWith("&")) {
        sep = "";
      } else if (call.indexOf('?') >= 0) {
        sep = "&";
      } else {
        sep = "?";
      }

      String tempStr = String(temperature, 1);

      call += sep + "temp=" + tempStr;
    }

    HTTPClient http;
    http.begin(call);
	#ifdef ARDUINO_ARCH_ESP32
	  http.setTimeout(5000);
	#else
	  http.setTimeout(5);
	#endif
    int code = http.GET();
    http.end();
    (void)code;
  }

public:
  void setup() { }

  void loop() {
    if (!isEnabled || strip.isUpdating() || (millis() - lastTime) <= loopInterval) return;
    lastTime = millis();

    #ifdef ESP8266
      temperature = -1;
    #elif defined(CONFIG_IDF_TARGET_ESP32S2)
      temperature = -1;
    #else
      temperature = roundf(temperatureRead() * 10) / 10;
    #endif

    if (webhookEnabled && webhookUrl.length() > 0) {
      if (!webhookTriggered && temperature >= webhookThreshold) {
        sendWebhookIfConfigured();
        webhookTriggered = true;
      } else if (webhookTriggered && temperature < webhookThreshold) {
        webhookTriggered = false;
      }
    }

    if (savedPresetToActivate != 0) {
      if (temperature >= activationThreshold) {
        if (!isAboveThreshold) {
          isAboveThreshold = true;

          if (oneTimeDisable) {
            blockRestore = true;
          }
        }

        if (currentPreset != savedPresetToActivate) {
          if (currentPlaylist > 0) {
            previousPlaylist = currentPlaylist;
          } else if (currentPreset > 0) {
            previousPreset = currentPreset;
          } else {
            saveTemporaryPreset();
          }
          applyPreset(savedPresetToActivate);
        }
      } else if (temperature <= (activationThreshold - resetMargin)) {
        if (isAboveThreshold) {
          isAboveThreshold = false;
        }

        if (oneTimeDisable && blockRestore) {
          // intentionally skip restore; user actions still processed and automation can still disable again
        } else {
          if (currentPreset == savedPresetToActivate) {
            if (previousPlaylist > 0) {
              applyPreset(previousPlaylist);
              previousPlaylist = 0;
            } else if (previousPreset > 0) {
              applyPreset(previousPreset);
              previousPreset = 0;
            } else {
              applyTemporaryPreset();
            }
          }
        }
      }
    }

    #ifndef WLED_DISABLE_MQTT
    if (WLED_MQTT_CONNECTED) {
      char array[16];
      String s = String(temperature, 1);
      snprintf(array, sizeof(array), "%s", s.c_str());
      publishMqtt(array);
    }
    #endif
  }

  void addToJsonInfo(JsonObject &root) {
    if (!isEnabled) return;
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray userTempArr = user.createNestedArray(FPSTR(_name));
    userTempArr.add(temperature);
    userTempArr.add(F(" °C"));

    JsonObject sensor = root[F("sensor")];
    if (sensor.isNull()) sensor = root.createNestedObject(F("sensor"));
    JsonArray sensorTempArr = sensor.createNestedArray(FPSTR(_name));
    sensorTempArr.add(temperature);
    sensorTempArr.add(F("°C"));
  }

  void addToConfig(JsonObject &root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = isEnabled;
    top[FPSTR(_loopInterval)] = loopInterval;
    top[FPSTR(_activationThreshold)] = activationThreshold;
    top[FPSTR(_presetToActivate)] = savedPresetToActivate;
    top[FPSTR(_oneTimeDisable)] = oneTimeDisable;

    top[FPSTR(_webhookEnabled)] = webhookEnabled;
    top[FPSTR(_webhookUrl)] = String(webhookUrl);
    top[FPSTR(_webhookAppendParams)] = webhookAppendParams;
    top[FPSTR(_webhookThreshold)] = webhookThreshold;
  }

  void appendConfigData() {
    oappend(F("addInfo('Internal Temperature:Loop Interval', 1, 'ms');"));
    oappend(F("addInfo('Internal Temperature:Activation Threshold', 1, '°C');"));
    oappend(F("addInfo('Internal Temperature:Preset To Activate', 1, '0 = unused');"));
    oappend(F("addInfo('Internal Temperature:Disable Auto Restore', 1, 'Do not restore previous preset after cooldown');"));
    oappend(F("addInfo('Internal Temperature:Webhook URL', 1, 'GET or exact URL as required');"));
    oappend(F("addInfo('Internal Temperature:Webhook Append Params', 1, 'If enabled, WLED will append ?temp=.. to the provided URL');"));
    oappend(F("addInfo('Internal Temperature:Webhook Threshold', 1, 'Trigger webhook when temp >= this value (°C)');"));
  }

  bool readFromConfig(JsonObject &root) {
    JsonObject top = root[FPSTR(_name)];
    bool configComplete = !top.isNull();
    configComplete &= getJsonValue(top[FPSTR(_enabled)], isEnabled);
    configComplete &= getJsonValue(top[FPSTR(_loopInterval)], loopInterval);

    loopInterval = max(loopInterval, minLoopInterval);

    configComplete &= getJsonValue(top[FPSTR(_presetToActivate)], savedPresetToActivate);
    configComplete &= getJsonValue(top[FPSTR(_activationThreshold)], activationThreshold);

    if (!top.isNull() && top.containsKey(FPSTR(_webhookEnabled))) {
      configComplete &= getJsonValue(top[FPSTR(_webhookEnabled)], webhookEnabled);
    }
    if (!top.isNull() && top.containsKey(FPSTR(_webhookUrl))) {
      String tmp;
      configComplete &= getJsonValue(top[FPSTR(_webhookUrl)], tmp);
      webhookUrl = tmp;
    }
    if (!top.isNull() && top.containsKey(FPSTR(_webhookAppendParams))) {
      configComplete &= getJsonValue(top[FPSTR(_webhookAppendParams)], webhookAppendParams);
    }
    if (!top.isNull() && top.containsKey(FPSTR(_webhookThreshold))) {
      configComplete &= getJsonValue(top[FPSTR(_webhookThreshold)], webhookThreshold);
    } else {
      webhookThreshold = activationThreshold;
    }
    if (!top.isNull() && top.containsKey(FPSTR(_oneTimeDisable))) {
      configComplete &= getJsonValue(top[FPSTR(_oneTimeDisable)], oneTimeDisable);
    } else {
      oneTimeDisable = false;
    }

    blockRestore = false;
    isAboveThreshold = false;
    webhookTriggered = false;

    return configComplete;
  }

  uint16_t getId() { return USERMOD_ID_INTERNAL_TEMPERATURE; }
};

// PROGMEM строки (ключи конфигурации)
const char InternalTemperatureUsermod::_name[] PROGMEM = "Internal Temperature";
const char InternalTemperatureUsermod::_enabled[] PROGMEM = "Enabled";
const char InternalTemperatureUsermod::_loopInterval[] PROGMEM = "Loop Interval";
const char InternalTemperatureUsermod::_activationThreshold[] PROGMEM = "Activation Threshold";
const char InternalTemperatureUsermod::_presetToActivate[] PROGMEM = "Preset To Activate";
const char InternalTemperatureUsermod::_oneTimeDisable[] PROGMEM = "Disable Auto Restore";
const char InternalTemperatureUsermod::_webhookUrl[] PROGMEM = "Webhook URL";
const char InternalTemperatureUsermod::_webhookEnabled[] PROGMEM = "Webhook Enabled";
const char InternalTemperatureUsermod::_webhookAppendParams[] PROGMEM = "Webhook Append Params";
const char InternalTemperatureUsermod::_webhookThreshold[] PROGMEM = "Webhook Threshold";

void InternalTemperatureUsermod::publishMqtt(const char *state, bool retain) {
  #ifndef WLED_DISABLE_MQTT
  if (WLED_MQTT_CONNECTED) {
    char subuf[64];
    strcpy(subuf, mqttDeviceTopic);
    strcat_P(subuf, PSTR("/mcutemp"));
    mqtt->publish(subuf, 0, retain, state);
  }
  #endif
}

static InternalTemperatureUsermod internal_temperature_v2;
REGISTER_USERMOD(internal_temperature_v2);
