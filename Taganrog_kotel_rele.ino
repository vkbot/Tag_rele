#define FB_NO_URLENCODE  // 🔧 Отключаем встроенный кодер FastBot (используем свой)

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <FastBot.h>
#include <ArduinoOTA.h>
#include <ctype.h>
#include <stdio.h>

// ---------- Настройки ----------
#define RELAY_PIN 0
#define EEPROM_SIZE 256  // Увеличили, чтобы вместить всё с запасом

#define TOKEN "8041803190:AAHRkiEDfsomB1Kv7w4mxiFCOW9G_6yxA0I"
const char* CHAT_ID   = "619084238";
const char* WORKER    = "royal-river-71a9.dragonforceedge.workers.dev";
#define DEFAULT_SSID     "Xiaomi_775D"
#define DEFAULT_PASSWORD "135791113"
ESP8266WebServer server(80);
FastBot bot(TOKEN);

// 🔧 Глобальный клиент для отправки
WiFiClientSecure tgClient;
bool tgClientReady = false;  // Флаг инициализации

// ---------- Структуры EEPROM ----------
struct WiFiData {
  char ssid[32];
  char pass[32];
  uint8_t flag;
};

struct RelayData {
  bool relayState;
  unsigned long lastOnTime;
  int32_t lastMsgID;
};

WiFiData wifiData;
RelayData relayData;

bool emergencyLockout = false;
unsigned long lockoutStartTime = 0;
const unsigned long LOCKOUT_DURATION = 3600 * 1000UL;
bool relayState = false;
unsigned long lastOnTime = 0;
const unsigned long AUTO_OFF_TIME = 900 * 1000UL;

unsigned long lastEepromWrite = 0;
const unsigned long EEPROM_WRITE_INTERVAL = 30000; // 30 сек

String currentSSID;
String currentPASS;

// 🔧 UTF-8 → %XX
String urlEncodeUTF8(const String& text) {
    String enc; enc.reserve(text.length() * 3);
    char buf[4];
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') enc += c;
        else { sprintf(buf, "%%%02X", (uint8_t)c); enc += buf; }
    }
    return enc;
}

// 📦 sendTG через POST (надёжно для длинных сообщений)
bool sendTG(const String& msg, const String& chatId = "") {
    String targetChat = chatId.length() ? chatId : CHAT_ID;
    
    if (!tgClientReady) {
        tgClient.setInsecure();
        tgClient.setTimeout(10000);
        tgClient.setBufferSizes(2048, 2048);  // 🔥 ДОБАВЛЕНО: буферы для SSL
        tgClientReady = true;
    }
    if (tgClient.connected()) tgClient.stop();

    if (!tgClient.connect(WORKER, 443)) {
        delay(200);
        if (!tgClient.connect(WORKER, 443)) return false;
    }

    // POST-запрос
    String body = "chat_id=" + targetChat + "&text=" + urlEncodeUTF8(msg);
    String path = "/bot" + String(TOKEN) + "/sendMessage";

    tgClient.print("POST " + path + " HTTP/1.1\r\n");
    tgClient.print("Host: " + String(WORKER) + "\r\n");
    tgClient.print("Content-Type: application/x-www-form-urlencoded\r\n");
    tgClient.print("Content-Length: " + String(body.length()) + "\r\n");
    tgClient.print("Connection: close\r\n\r\n");
    tgClient.print(body);
    tgClient.flush();
    
    delay(100); // Пауза для переключения SSL-стека
    yield();

    // Читаем ответ
    String response;
    response.reserve(2048);
    unsigned long lastByte = millis();
    unsigned long start = millis();
    
    while (!tgClient.available() && millis() - start < 5000) {
        delay(10); yield();
    }
    
    while (millis() - lastByte < 2000 && millis() - start < 12000) {
        while (tgClient.available()) {
            char c = tgClient.read();
            response += c;
            lastByte = millis();
            if (response.length() >= 2000) goto parse_response;
        }
        delay(10); yield();
    }
    
parse_response:
    tgClient.stop();
    delay(50);

    return response.indexOf("\"ok\":true") >= 0;
}

void sendMsg(const String& text, const String& chatId) {
    sendTG(text, chatId);
}

// ---------- EEPROM ----------
void saveWiFiData(const char* s, const char* p) {
  strncpy(wifiData.ssid, s, sizeof(wifiData.ssid) - 1);
  strncpy(wifiData.pass, p, sizeof(wifiData.pass) - 1);
  wifiData.ssid[31] = '\0';
  wifiData.pass[31] = '\0';
  wifiData.flag = 1;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, wifiData);
  EEPROM.commit();
  EEPROM.end();
}

bool loadWiFiData(String &s, String &p) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiData);
  EEPROM.end();
  if (wifiData.flag != 1) return false;
  s = String(wifiData.ssid);
  p = String(wifiData.pass);
  return true;
}

void saveRelayState() {
  relayData.relayState = relayState;
  relayData.lastOnTime = lastOnTime;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(sizeof(WiFiData), relayData);
  EEPROM.commit();
  EEPROM.end();
}

void saveMsgID() {
  if (millis() - lastEepromWrite < EEPROM_WRITE_INTERVAL) return;
  EEPROM.begin(EEPROM_SIZE);
  // Сохраняем ID по отдельному адресу, чтобы не затереть relayData
  EEPROM.put(sizeof(WiFiData) + sizeof(RelayData), relayData.lastMsgID);
  EEPROM.commit();
  EEPROM.end();
  lastEepromWrite = millis();
}

void loadRelayData() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(sizeof(WiFiData), relayData);
  // Загружаем lastMsgID из отдельной ячейки
  int32_t savedID = 0;
  EEPROM.get(sizeof(WiFiData) + sizeof(RelayData), savedID);
  EEPROM.end();
  
  if (savedID > 0 && savedID < 100000000) relayData.lastMsgID = savedID;
  else relayData.lastMsgID = 0;
  
  relayState = relayData.relayState;
  digitalWrite(RELAY_PIN, relayState ? LOW : HIGH);
  lastOnTime = relayState ? millis() : 0;
}

bool isEmergencyLocked() {
  if (!emergencyLockout) return false;
  if (millis() - lockoutStartTime >= LOCKOUT_DURATION) {
    emergencyLockout = false;
    lockoutStartTime = 0;
  }
  return emergencyLockout;
}

// ---------- Реле ----------
void switchRelay(bool state, const String& source = "") {
  if (state && isEmergencyLocked()) {
    unsigned long elapsed = millis() - lockoutStartTime;
    unsigned long remaining = (elapsed < LOCKOUT_DURATION) ? (LOCKOUT_DURATION - elapsed) : 0;
    int minutes = (remaining + 59999) / 60000;
    sendTG("🔒 Аварийная блокировка активна!\n⏱ Осталось: ~" + String(minutes) + " мин\n🔄 Или перезагрузите устройство (/reboot).", CHAT_ID);
    return;
  }

  relayState = state;
  digitalWrite(RELAY_PIN, state ? LOW : HIGH);
  if (state) lastOnTime = millis();
  
  saveRelayState();

  String msg = state ? "🔥 Котёл включён" : "❄️ Котёл выключен";
  if (source.length()) msg += " (" + source + ")";
  sendTG(msg, CHAT_ID);
}

// ---------- Web ----------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Умный котёл</title></head><body>";
  html += "<h1>Умный котёл</h1><p>Состояние: <b>" + String(relayState ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН") + "</b></p>";
  html += "<a href=\"/on\"><button>ВКЛ</button></a> ";
  html += "<a href=\"/off\"><button>ВЫКЛ</button></a> ";
  html += "<a href=\"/status\"><button>СТАТУС</button></a></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleOn() { switchRelay(true, "Web"); server.send(200, "text/plain", "ON"); }
void handleOff() { switchRelay(false, "Web"); server.send(200, "text/plain", "OFF"); }
void handleStatus() {
  String st = relayState ? "ON" : "OFF";
  unsigned long sec = relayState ? (millis() - lastOnTime) / 1000 : 0;
  server.send(200, "text/plain", "Status: " + st + ", Time: " + String(sec) + " sec");
}

// ---------- Safety ----------
void safetyCheck() {
  if (!isEmergencyLocked() && relayState && millis() - lastOnTime > AUTO_OFF_TIME) {
    switchRelay(false, "EMERGENCY");
    emergencyLockout = true;
    lockoutStartTime = millis();
    sendTG("🚨 АВАРИЯ: котёл работал слишком долго!\n🔒 Включение заблокировано на 1 час или до перезагрузки.", CHAT_ID);
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  
  EEPROM.begin(EEPROM_SIZE);
  String ssid, password;
  if (!loadWiFiData(ssid, password)) {
    ssid = DEFAULT_SSID;
    password = DEFAULT_PASSWORD;
  }
  EEPROM.end();

  IPAddress local_IP(192, 168, 31, 200);
  IPAddress gateway(192, 168, 31, 1);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns(8, 8, 8, 8);
  
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  currentSSID = ssid;
  currentPASS = password;
  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.hostname("ESP_kotel");
  WiFi.begin(currentSSID.c_str(), currentPASS.c_str());
  
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500); yield();
  }
  
  loadRelayData();
  bot.setOffset(relayData.lastMsgID);
  bot.setChatID(CHAT_ID);
  bot.setBufferSizes(2048, 2048);
  
  server.on("/", handleRoot);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/status", handleStatus);
  server.begin();

  bot.attach([](FB_msg& msg) {
    // Защита от дубликатов по update_id (так как мы сохраняем его)
if (relayData.lastMsgID > 0 && msg.update_id + 1 <= relayData.lastMsgID) return;
relayData.lastMsgID = msg.update_id + 1;
    saveMsgID();

    String cmd = msg.text;
    cmd.toLowerCase();

    if (cmd == "/on") {
      switchRelay(true, "Telegram");
    }
    else if (cmd == "/resetid") {
        relayData.lastMsgID = 0;
        saveMsgID();
        sendTG("♻️ ID сброшен. Жду команды.", CHAT_ID);
    }
    else if (cmd == "/off") {
      switchRelay(false, "Telegram");
    }
    else if (cmd == "/status") {
      String status = "🔧 Статус котла:\n";
      status += "📌 IP: " + WiFi.localIP().toString() + "\n";
      status += "📶 WiFi: " + WiFi.SSID() + "\n";
      status += "📡 RSSI: " + String(WiFi.RSSI()) + " dBm\n";
      status += "🔌 Реле: " + String(relayState ? "ВКЛ" : "ВЫКЛ") + "\n";
      if (relayState) status += "⏱ Время работы: " + String((millis() - lastOnTime) / 1000) + " сек";
      sendTG(status, CHAT_ID);
    }
    else if (cmd == "/reboot") {
      sendTG("🔄 Перезагрузка...", CHAT_ID);
      relayData.lastMsgID = msg.messageID;
      saveMsgID();
      delay(500); yield();
      ESP.restart();
    }
    else if (cmd.startsWith("/setwifi ")) {
      int sp = msg.text.indexOf(' ', 9);
      if (sp != -1) {
        String newSSID = msg.text.substring(9, sp);
        String newPASS = msg.text.substring(sp + 1);
        if (newSSID.length() < 1 || newPASS.length() < 1) {
          sendTG("⚠️ Неверный формат SSID/PASSWORD", CHAT_ID);
          return;
        }
        sendTG("⏳ Подключение к " + newSSID + "...", CHAT_ID);
        WiFi.begin(newSSID.c_str(), newPASS.c_str());
        bool connected = false;
        for (int i = 0; i < 15; i++) {
          if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
          delay(1000); yield();
        }
        if (connected) {
          currentSSID = newSSID;
          currentPASS = newPASS;
          saveWiFiData(newSSID.c_str(), newPASS.c_str());
          sendTG("✅ Успешно!\nSSID: " + newSSID + "\nПерезагрузка...", CHAT_ID);
          delay(2000);
          ESP.restart();
        } else {
          sendTG("❌ Не удалось подключиться. Проверь данные.", CHAT_ID);
        }
      } else {
        sendTG("⚠️ Используй: /setwifi SSID PASSWORD", CHAT_ID);
      }
    }
    else if (cmd == "/resetmsg") {
      relayData.lastMsgID = 0;
      saveMsgID();
      sendTG("♻️ lastMsgID сброшен", CHAT_ID);
    }
    else {
      sendTG("❓ Неизвестная команда. /on, /off, /status, /reboot, /setwifi", CHAT_ID);
    }
  });

  // Стартовое сообщение (в группу)
  sendTG("🤖 Устройство онлайн!\nIP: " + WiFi.localIP().toString() + "\nСостояние: " + String(relayState ? "ВКЛ" : "ВЫКЛ"), "-1001819803857");

  ArduinoOTA.setHostname("ESP_tag_kot");
  ArduinoOTA.setPassword("12345678");
  ArduinoOTA.begin();
}

// ---------- Loop ----------
void loop() {
  server.handleClient();
  bot.tick();
  safetyCheck();
  ArduinoOTA.handle();

  static unsigned long lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 30000) {
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(currentSSID.c_str(), currentPASS.c_str());
    lastReconnect = millis();
    yield();
  }
  yield();
}