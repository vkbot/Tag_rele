#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <FastBot.h>
#include <ArduinoOTA.h>

// ---------- Настройки ----------
#define RELAY_PIN 0              // GPIO0
#define EEPROM_SIZE 128

#define TELEGRAM_BOT_TOKEN "8041803190:AAHRkiEDfsomB1Kv7w4mxiFCOW9G_6yxA0I"
#define TELEGRAM_CHAT_ID  "619084238"
#define DEFAULT_SSID     "Xiaomi_775D"
#define DEFAULT_PASSWORD "135791113"
ESP8266WebServer server(80);
FastBot bot(TELEGRAM_BOT_TOKEN);

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
  unsigned long lastAnyActivation; // ← новое поле
  unsigned long lastAnyDelta; // сколько мс прошло с последнего включения
};

WiFiData wifiData;
RelayData relayData;

bool emergencyLockout = false;          // активна ли аварийная блокировка
bool maintenanceMode = false;
unsigned long lockoutStartTime = 0;     // когда началась блокировка (0 = не активна)
const unsigned long LOCKOUT_DURATION = 3600 * 1000UL; // 1 час в миллисекундах
bool relayState = false;
unsigned long lastOnTime = 0;
unsigned long lastAnyActivation = 0; // время последнего включения реле (любым способом)
unsigned long CHECK_INTERVAL = 2000 * 1000UL; // 33.3 минуты
const unsigned long MAINTENANCE_DURATION = 3 * 60 * 1000UL; // 3 минуты
const unsigned long AUTO_OFF_TIME = 900 * 1000UL;

unsigned long lastEepromWrite = 0;
const unsigned long EEPROM_WRITE_INTERVAL = 300000;

String currentSSID;
String currentPASS;

String urlencode(const String& str) {
  String encoded = "";
  const char *cstr = str.c_str();

  for (size_t i = 0; i < strlen(cstr); i++) {
    unsigned char c = cstr[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

void sendMsg(String text, String chatId) {
  bot.sendMessage(urlencode(text), chatId);
}
// ---------- EEPROM ----------
void saveWiFiData(const char* s, const char* p) {
  strncpy(wifiData.ssid, s, sizeof(wifiData.ssid));
  strncpy(wifiData.pass, p, sizeof(wifiData.pass));
  wifiData.flag = 1;
  EEPROM.put(0, wifiData);
  EEPROM.commit();
}

bool loadWiFiData(String &s, String &p) {
  EEPROM.get(0, wifiData);
  if (wifiData.flag != 1) return false;
  s = String(wifiData.ssid);
  p = String(wifiData.pass);
  return true;
}

void saveRelayState() {
  relayData.relayState = relayState;
  relayData.lastOnTime = lastOnTime;
  relayData.lastAnyActivation = lastAnyActivation;
  relayData.lastAnyDelta = millis() - lastAnyActivation;
  EEPROM.put(sizeof(WiFiData), relayData);
  EEPROM.commit();
  Serial.println("💾 EEPROM: relayState и lastAnyActivation сохранены");
}

void saveMsgID() {
  if (millis() - lastEepromWrite < EEPROM_WRITE_INTERVAL) return;
  EEPROM.put(sizeof(WiFiData), relayData);
  EEPROM.commit();
  lastEepromWrite = millis();
  Serial.println("💾 EEPROM: lastMsgID сохранён");
}

void loadRelayData() {
  EEPROM.get(sizeof(WiFiData), relayData);
  if (relayData.lastMsgID < 0 || relayData.lastMsgID > 100000000)
  relayData.lastMsgID = 0;
  relayState = relayData.relayState;
  digitalWrite(RELAY_PIN, relayState ? LOW : HIGH);
  if (relayData.lastMsgID < 0) relayData.lastMsgID = 0;
  lastOnTime = relayState ? millis() : 0;
 lastAnyActivation = millis() - relayData.lastAnyDelta;
  Serial.println("🔄 Данные загружены из EEPROM");
}
bool isEmergencyLocked() {
  if (!emergencyLockout) return false;
  // Если прошёл час — снимаем блокировку автоматически
  if (millis() - lockoutStartTime >= LOCKOUT_DURATION) {
    emergencyLockout = false;
    lockoutStartTime = 0;
    Serial.println("🔓 Аварийная блокировка снята автоматически (прошёл 1 час)");
  }
  return emergencyLockout;
}
// ---------- Реле ----------
void switchRelay(bool state, String source = "") {
  if (state && isEmergencyLocked()) {
    unsigned long elapsed = millis() - lockoutStartTime;
    unsigned long remaining = (elapsed < LOCKOUT_DURATION) ? (LOCKOUT_DURATION - elapsed) : 0;
    int minutes = (remaining + 59999) / 60000;
    sendMsg("🔒 Аварийная блокировка активна!\n"
                    "⏱ Осталось: ~" + String(minutes) + " мин\n"
                    "🔄 Или перезагрузите устройство (/reboot).",
                    TELEGRAM_CHAT_ID);
    return;
  }

  relayState = state;
  maintenanceMode = (source == "Maintenance");
  digitalWrite(RELAY_PIN, state ? LOW : HIGH);
  if (state) {
    lastOnTime = millis();
    lastAnyActivation = millis();
  } else {
    maintenanceMode = false; // сброс при выключении
  }
  saveRelayState();

  String msg = state ? "🔥 Котёл включён" : "❄️ Котёл выключен";
  if (source.length()) msg += " (" + source + ")";
  sendMsg(msg, TELEGRAM_CHAT_ID);
}
// ---------- Web ----------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><title>Умный котёл</title></head><body>";
  html += "<h1>Умный котёл</h1>";
  html += "<p>Состояние: <b>" + String(relayState ? "ВКЛЮЧЕН" : "ВЫКЛЮЧЕН") + "</b></p>";
  html += "<a href=\"/on\"><button>ВКЛ</button></a> ";
  html += "<a href=\"/off\"><button>ВЫКЛ</button></a> ";
  html += "<a href=\"/status\"><button>СТАТУС</button></a>";
  html += "</body></html>";
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
    lockoutStartTime = millis(); // запоминаем момент срабатывания
    sendMsg("🚨 АВАРИЯ: котёл работал слишком долго!\n"
                    "🔒 Включение заблокировано на 1 час или до перезагрузки.",
                    TELEGRAM_CHAT_ID);
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
    Serial.println("⚠️ Wi-Fi данные отсутствуют. Используем дефолтные.");
  } else {
    Serial.println("✅ Загружены сохранённые Wi-Fi данные: " + ssid);
  }

  // Статический IP
  IPAddress local_IP(192, 168, 31, 200);
  IPAddress gateway(192, 168, 31, 1);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns(8, 8, 8, 8); // ← DNS = ваш роутер (часто совпадает с gateway)
  WiFi.mode(WIFI_STA);
WiFi.setAutoReconnect(true);
WiFi.persistent(true);

currentSSID = ssid;
currentPASS = password;

// Если хочешь — временно закомментируй для проверки
WiFi.config(local_IP, gateway, subnet, dns);
WiFi.hostname("ESP_kotel");
WiFi.begin(currentSSID.c_str(), currentPASS.c_str());

Serial.println("📡 Wi-Fi: попытка подключения запущена");


  loadRelayData();

  server.on("/", handleRoot);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/status", handleStatus);
  server.begin();
  bot.attach([](FB_msg& msg) {
  // --- 1. Проверка дубликатов ---
 if (relayData.lastMsgID > 0 && msg.messageID <= relayData.lastMsgID) {
    return;
  }
  relayData.lastMsgID = msg.messageID;
  saveMsgID();


  // --- 2. Сохраняем ID сообщения ---
  relayData.lastMsgID = msg.messageID;
  saveMsgID(); // ← ЭТО ФУНКЦИЯ ИЗ ТВОЕГО КОДА — она останется!

  // --- 3. Обработка команд ---
  String cmd = msg.text;
  cmd.toLowerCase();

  if (cmd == "/on") {
    switchRelay(true, "Telegram");
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
    status += "🛠 Maintenance: " + String(CHECK_INTERVAL / 60000) + " мин\n";
    if (relayState) status += "⏱ Время работы: " + String((millis() - lastOnTime) / 1000) + " сек";
    sendMsg(status, TELEGRAM_CHAT_ID);
  }
    else if (cmd == "/reboot") {
    sendMsg("🔄 Перезагрузка...", TELEGRAM_CHAT_ID);
    relayData.lastMsgID = msg.messageID;  // ← добавить
    EEPROM.put(sizeof(WiFiData), relayData); // ← добавить
    EEPROM.commit();                      // ← добавить
    delay(100);
    yield();
    ESP.restart();
  }
  else if (cmd.startsWith("/setwifi ")) {
    int sp = msg.text.indexOf(' ', 9);
    if (sp != -1) {
      String newSSID = msg.text.substring(9, sp);
      String newPASS = msg.text.substring(sp + 1);
      if (newSSID.length() < 1 || newPASS.length() < 1) {
        sendMsg("⚠️ Неверный формат SSID/PASSWORD", TELEGRAM_CHAT_ID);
        return;
      }
      sendMsg("⏳ Подключение к " + newSSID + "...", TELEGRAM_CHAT_ID);
      WiFi.begin(newSSID.c_str(), newPASS.c_str());
      bool connected = false;
      for (int i = 0; i < 10; i++) {
        if (WiFi.status() == WL_CONNECTED) {
          connected = true;
          break;
        }
        delay(1000);
      }
      if (connected) {
        currentSSID = newSSID;
        currentPASS = newPASS;
        saveWiFiData(newSSID.c_str(), newPASS.c_str());
        sendMsg("✅ Успешно!\nSSID: " + newSSID + "\nПерезагрузка...", TELEGRAM_CHAT_ID);
        delay(2000);
        ESP.restart();
      } else {
        sendMsg("❌ Не удалось подключиться. Проверь SSID и пароль.", TELEGRAM_CHAT_ID);
      }
    } else {
      sendMsg("⚠️ Используй: /setwifi SSID PASSWORD", TELEGRAM_CHAT_ID);
    }
  }
  else if (cmd == "/resetmsg") {
  relayData.lastMsgID = 0;
  EEPROM.put(sizeof(WiFiData), relayData);
  EEPROM.commit();
  sendMsg("♻️ lastMsgID сброшен", TELEGRAM_CHAT_ID);
}
else if (cmd.startsWith("/setmaint ")) {
  unsigned long minutes = cmd.substring(10).toInt();
  if (minutes < 10 || minutes > 1000) { // 10 мин – 7 суток
    sendMsg("⚠️ Интервал от 10 до 1000 минут", TELEGRAM_CHAT_ID);
    return;
  }
  CHECK_INTERVAL = minutes * 60UL * 1000UL;
  sendMsg("🔧 Профилактическое включение каждые " +
                  String(minutes) + " мин", TELEGRAM_CHAT_ID);
}
  else {
    // Неизвестная команда
    sendMsg("❓ Неизвестная команда. Используй /on, /off, /status, /reboot, /setwifi, /resetmsg, /setmaint", TELEGRAM_CHAT_ID);
  }
});
sendMsg("🤖 Устройство онлайн!\nIP: " + WiFi.localIP().toString() +
                  "\nСостояние: " + String(relayState ? "ВКЛ" : "ВЫКЛ"), "-1001819803857");
  ArduinoOTA.setHostname("ESP_tag_kot");
  ArduinoOTA.setPassword("12345678");
  ArduinoOTA.onStart([]() { Serial.println("📡 Начало OTA обновления..."); });
  ArduinoOTA.onEnd([]() { Serial.println("\n✅ OTA обновление завершено"); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("❌ OTA ошибка [%u]\n", error); });
  ArduinoOTA.begin();
}

// ---------- Loop ----------
void loop() {
  server.handleClient();
  bot.tick();
  safetyCheck();
  ArduinoOTA.handle();

  // Если Wi‑Fi нет — переподключаемся, но без блокировки loop
  static unsigned long lastReconnect = 0;

if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 30000) {
  Serial.println("🔄 Wi-Fi не подключен. Переподключаемся...");

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(currentSSID.c_str(), currentPASS.c_str());

  lastReconnect = millis();
}

    // --- Профилактическое включение: если 8 часов не включался — включить на 3 минуты ---
  if (!relayState && !isEmergencyLocked()) {
    if (millis() - lastAnyActivation >= CHECK_INTERVAL) {
      switchRelay(true, "Maintenance");
    }
  }

  // --- Авто-выключение после 3 минут в режиме техобслуживания ---
  if (maintenanceMode && relayState && millis() - lastOnTime >= MAINTENANCE_DURATION) {
    switchRelay(false, "Maintenance Auto-Off");
  }
}
