#include <Arduino.h>
#include <initializer_list>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "alarm_logic.h"
#include "web_page.h"

// Контакти підключення
constexpr uint8_t DHT_PIN = 15;
constexpr uint8_t LED_PIN = 18;

// Період зчитування датчика
constexpr uint32_t SAMPLE_MS = 2000;

DHTesp dht;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

Limits limits;

float temperature = NAN;
float humidity = NAN;

bool sampled = false;
bool sensorOk = false;
bool serverStarted = false;

uint32_t lastRead = 0;
uint32_t lastCleanup = 0;
uint32_t lastWifiTry = 0;

// Прапорці виходу показників за допустимі межі
unsigned alarmFlags = 0;

struct Command {
  uint32_t clientId;
  Limits limits;
};

QueueHandle_t commandQueue;

// Виведення інформації про автора
void drawAuthor() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_t_cyrillic);

  oled.drawUTF8(0, 12, "Григор'єв");
  oled.drawUTF8(0, 27, "Георгій Артемович");
  oled.drawUTF8(0, 43, "М-КІ-52");
  oled.drawUTF8(0, 59, "WebSocket-сервер");

  oled.sendBuffer();
}

// Керування світлодіодом
void updateLed() {
  bool ledOn = false;

  if (sampled) {
    if (!sensorOk) {
      // Блимання у разі помилки датчика
      ledOn = ((millis() / 500) % 2 == 0);
    } else {
      // Постійне світіння у разі виходу за межі
      ledOn = (alarmFlags != 0);
    }
  }

  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
}

// Надсилання поточного стану всім вебклієнтам
void sendState() {
  if (!serverStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  StaticJsonDocument<768> doc;

  doc["type"] = "state";
  doc["sampled"] = sampled;
  doc["sensorOk"] = sensorOk;

  if (sensorOk) {
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"] = nullptr;
  }

  // Назва ключа має відповідати JavaScript у web_page.h
  doc["alarmMask"] = alarmFlags;

  if (!sampled) {
    doc["ledMode"] = "off";
  } else if (!sensorOk) {
    doc["ledMode"] = "blink";
  } else {
    doc["ledMode"] = alarmFlags != 0 ? "on" : "off";
  }

  JsonObject settings = doc.createNestedObject("limits");

  settings["tMin"] = limits.tMin;
  settings["tMax"] = limits.tMax;
  settings["hMin"] = limits.hMin;
  settings["hMax"] = limits.hMax;

  String message;
  serializeJson(doc, message);

  ws.textAll(message);
}

// Відповідь окремому клієнту
void reply(
  uint32_t clientId,
  const char* type,
  const char* message
) {
  StaticJsonDocument<256> doc;

  doc["type"] = type;
  doc["message"] = message;

  String output;
  serializeJson(doc, output);

  ws.text(clientId, output);
}

// Оброблення повідомлень WebSocket
void onWebSocket(
  AsyncWebSocket*,
  AsyncWebSocketClient* client,
  AwsEventType type,
  void* arg,
  uint8_t* data,
  size_t len
) {
  if (type != WS_EVT_DATA) {
    return;
  }

  auto* info = static_cast<AwsFrameInfo*>(arg);

  if (
    !info->final ||
    info->index != 0 ||
    info->len != len ||
    info->opcode != WS_TEXT ||
    len > 384
  ) {
    client->close(1003, "Unsupported message");
    return;
  }

  StaticJsonDocument<512> doc;

  DeserializationError error = deserializeJson(
    doc,
    data,
    len
  );

  if (error || doc["type"] != "setLimits") {
    reply(
      client->id(),
      "error",
      "Некоректний формат команди"
    );
    return;
  }

  for (const char* key : {"tMin", "tMax", "hMin", "hMax"}) {
    if (!doc[key].is<float>()) {
      reply(
        client->id(),
        "error",
        "Усі межі мають бути числами"
      );
      return;
    }
  }

  Command command;

  command.clientId = client->id();
  command.limits.tMin = doc["tMin"].as<float>();
  command.limits.tMax = doc["tMax"].as<float>();
  command.limits.hMin = doc["hMin"].as<float>();
  command.limits.hMax = doc["hMax"].as<float>();

  if (!validLimits(command.limits)) {
    reply(
      client->id(),
      "error",
      "Перевірте діапазони та порядок мінімуму й максимуму"
    );
    return;
  }

  // Зміни застосовуються в основному циклі
  if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
    reply(
      client->id(),
      "error",
      "Черга зайнята. Повторіть спробу"
    );
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // OLED: SDA — GPIO21, SCL — GPIO22
  Wire.begin(21, 22);

  oled.setI2CAddress(0x3C * 2);
  oled.begin();
  drawAuthor();

  dht.setup(DHT_PIN, DHTesp::DHT22);

  commandQueue = xQueueCreate(8, sizeof(Command));

  if (commandQueue == nullptr) {
    Serial.println("Queue allocation failed");

    while (true) {
      delay(1000);
    }
  }

  ws.onEvent(onWebSocket);
  server.addHandler(&ws);

  // Вебсторінка зберігається у web_page.h
  server.on(
    "/",
    HTTP_GET,
    [](AsyncWebServerRequest* request) {
      AsyncWebServerResponse* response =
        request->beginResponse(
          200,
          "text/html; charset=utf-8",
          WEB_PAGE
        );

      response->addHeader("Cache-Control", "no-store");
      request->send(response);
    }
  );

  server.onNotFound(
    [](AsyncWebServerRequest* request) {
      request->send(404, "text/plain", "Not found");
    }
  );

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin("Wokwi-GUEST", "", 6);

  lastWifiTry = millis();
  lastRead = millis();

  Serial.println("Connecting to Wokwi-GUEST...");
}

void loop() {
  const uint32_t now = millis();

  // Контроль Wi-Fi та запуск сервера
  if (WiFi.status() == WL_CONNECTED) {
    if (!serverStarted) {
      server.begin();
      serverStarted = true;

      Serial.println(
        "HTTP + WebSocket ready: http://localhost:8180/"
      );

      Serial.print("ESP32 IP: ");
      Serial.println(WiFi.localIP());
    }
  } else if (now - lastWifiTry >= 10000) {
    lastWifiTry = now;
    WiFi.reconnect();
  }

  // Оброблення нових меж із вебінтерфейсу
  Command command;

  for (
    int i = 0;
    i < 8 && xQueueReceive(commandQueue, &command, 0) == pdTRUE;
    ++i
  ) {
    limits = command.limits;

    alarmFlags = sensorOk
      ? alarmMask(temperature, humidity, limits)
      : 0;

    updateLed();

    reply(
      command.clientId,
      "ack",
      "Межі застосовано на ESP32"
    );

    sendState();
  }

  // Періодичне зчитування DHT22
  if (now - lastRead >= SAMPLE_MS) {
    lastRead = now;

    TempAndHumidity sample = dht.getTempAndHumidity();

    temperature = sample.temperature;
    humidity = sample.humidity;
    sampled = true;

    sensorOk =
      dht.getStatus() == DHTesp::ERROR_NONE &&
      isfinite(temperature) &&
      isfinite(humidity) &&
      temperature >= -40 &&
      temperature <= 80 &&
      humidity >= 0 &&
      humidity <= 100;

    alarmFlags = sensorOk
      ? alarmMask(temperature, humidity, limits)
      : 0;

    updateLed();
    sendState();

    Serial.printf(
      "T=%.1f C H=%.1f %% sensor=%s alarmFlags=%u\n",
      temperature,
      humidity,
      sensorOk ? "OK" : "ERROR",
      alarmFlags
    );
  }

  updateLed();

  // Видалення відключених WebSocket-клієнтів
  if (serverStarted && now - lastCleanup >= 1000) {
    lastCleanup = now;
    ws.cleanupClients();
  }

  delay(2);
}