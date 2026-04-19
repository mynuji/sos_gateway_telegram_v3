// =========================================================================
// [SOS Gateway V3.0 Enterprise Edition]
// 기능: WiFiManager 동적 설정 + OLED 최적화 + GitHub OTA(버전관리) + MAC 보안
// =========================================================================

#define USE_OLED 1

#include "mbedtls/md.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <RCSwitch.h>
#include <Ticker.h>
#include <UniversalTelegramBot.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>

// =========================================================================
// ⚠️ [보안 및 OTA 사용자 설정 영역]
// =========================================================================
// 1. 프로비저닝 시 사용했던 우리 가족만의 마스터 키
#define SECRET_KEY "Secure_SOS_Key_2026!@#"

// 2. 펌웨어 버전 관리 (현재 기기에 설치된 버전)
const String CURRENT_VERSION = "1.0";

// 3. GitHub Private Repository 정보 (HTTP 302 리다이렉트 방지를 위해 직접
// raw.githubusercontent.com 주소 사용)
const char *GITHUB_VERSION_URL =
    "https://raw.githubusercontent.com/mynuji/sos_gateway_firmware/master/"
    "esp32c3_oled/version.txt";
const char *GITHUB_OTA_URL =
    "https://raw.githubusercontent.com/mynuji/sos_gateway_firmware/master/"
    "esp32c3_oled/sos_gateway_firmware.bin";

// 4. GitHub 로그인 아이디 및 PAT (Personal Access Token)
const char *GITHUB_USER = "mynuji";
const char *GITHUB_PAT = "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";
// =========================================================================

// 5. 텔레그램 봇 및 단톡방 기본 하드코딩 (WiFiManager 기본값으로 사용됨)
char telegram_bot_token[64] = "8734638640:AAFqaLXRdm7Im0lK_QA3kHT6XllLJ6pLmyI";
char telegram_chat_id[32] = "-5227492227";
// 대구  -5227492227, 암사동 -1003729821413

// --- [하드웨어 핀 설정] ---
const int RF_RECEIVER_PIN = 2;
const int PAIRING_BUTTON_PIN = 9;
const int STATUS_LED_PIN = 8;

// --- [객체 및 전역 변수] ---
WiFiClientSecure secured_client;
UniversalTelegramBot *bot = nullptr;
RCSwitch mySwitch = RCSwitch();
Preferences prefs;
Ticker ledTicker;

bool ledState = false;
bool shouldSaveConfig = false;
const int MAX_BUTTONS = 20;
unsigned long savedButtonCodes[MAX_BUTTONS] = {
    0,
};
int registeredButtonCount = 0;
int backupButtonCount = 0;
unsigned long backupButtonCodes[MAX_BUTTONS] = {
    0,
};
unsigned long alertDisplayExpireTime = 0;

bool isPairingMode = false;
unsigned long pairingStartTime = 0;
const unsigned long PAIRING_TIMEOUT = 20000;

unsigned long lastStandbyAnimTime = 0;
unsigned long lastBotCheckTime = 0;
const int BOT_CHECK_INTERVAL = 10000; // 10초 마다 텔레그램 명령어 확인

int standbyAnimFrame = 0;
int standbyAnimDir = 1;

#ifdef USE_OLED
#include <U8g2lib.h>
#include <Wire.h>

const int OLED_SDA_PIN = 5;
const int OLED_SCL_PIN = 6;
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const unsigned char bmp_wifi_xbm[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xff, 0x03, 0xf0, 0xff, 0x0f,
    0xf8, 0xff, 0x1f, 0x3c, 0x00, 0x3c, 0x06, 0x00, 0x60, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0xc0, 0xff, 0x03, 0xe0, 0xff, 0x07, 0xf0, 0x00, 0x0f,
    0x18, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x7e, 0x00,
    0x00, 0xff, 0x00, 0x80, 0x81, 0x01, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00,
    0x00, 0x3c, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00};

inline void initOLED() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  u8g2.begin();
  u8g2.setFont(u8g2_font_ncenB08_tr);
}

inline void updateOLED(int mode, const char *msg1 = "", const char *msg2 = "",
                       int badgeCount = 0) {
  u8g2.clearBuffer();
  if (mode == 1) { // WIFI Setup / Connecting
    u8g2.drawXBM(24, 4, 24, 24, bmp_wifi_xbm);
    u8g2.setFont(u8g2_font_micro_tr);
    if (strlen(msg1) > 0) {
      u8g2.setCursor(0, 36);
      u8g2.print(msg1);
    } else {
      u8g2.setCursor(16, 36);
      u8g2.print("WIFI Setup");
    }
  } else if (mode == 5) { // WiFi Error (X mark)
    u8g2.drawLine(24, 4, 48, 28);
    u8g2.drawLine(48, 4, 24, 28);
    u8g2.drawLine(25, 4, 49, 28); // make thick
    u8g2.drawLine(49, 4, 25, 28);
    u8g2.setFont(u8g2_font_micro_tr);
    u8g2.setCursor(0, 36);
    u8g2.print(msg1);
  } else if (mode == 2) { // Pairing
    u8g2.setDrawColor(1);
    u8g2.drawRFrame(26, 4, 20, 24, 4);
    u8g2.drawDisc(36, 18, 6);
    u8g2.drawBox(33, 8, 6, 2);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(46, 6, 7);
    u8g2.setDrawColor(1);
    u8g2.drawDisc(46, 6, 6);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_micro_tr);
    u8g2.setCursor(43 - (badgeCount >= 10 ? 3 : 0), 10);
    u8g2.print(badgeCount);
    u8g2.setDrawColor(1);
    u8g2.setCursor(10, 36);
    u8g2.print(msg2);
  } else if (mode == 3) { // Standby
    for (int i = 0; i < 5; i++) {
      if (i == standbyAnimFrame)
        u8g2.drawBox(21 + i * 6, 10, 5, 12);
      else
        u8g2.drawFrame(21 + i * 6, 13, 5, 6);
    }
    u8g2.setFont(u8g2_font_micro_tr);
    u8g2.setCursor(16, 36);
    u8g2.print(msg1);
  } else if (mode == 4) { // SOS/Update (Telegram Icon)
    u8g2.drawTriangle(26, 18, 34, 22, 48, 8);
    u8g2.drawTriangle(34, 22, 42, 28, 48, 8);
    u8g2.drawTriangle(34, 22, 34, 28, 38, 24);
    u8g2.setFont(u8g2_font_micro_tr);
    int textWidth = u8g2.getStrWidth(msg1);
    if (textWidth < 72) {
      u8g2.setCursor((72 - textWidth) / 2, 38);
    } else {
      u8g2.setCursor(0, 38);
    }
    u8g2.print(msg1);
  } else if (mode == 6) {              // License Error (Lock Icon)
    u8g2.drawRFrame(31, 6, 10, 10, 4); // Shackle
    u8g2.drawBox(28, 15, 16, 13);      // Body
    u8g2.setDrawColor(0);
    u8g2.drawPixel(35, 18);
    u8g2.drawPixel(36, 18);     // Keyhole top
    u8g2.drawBox(35, 19, 2, 5); // Keyhole bottom
    u8g2.setDrawColor(1);

    // X mark
    u8g2.drawLine(48, 6, 56, 14);
    u8g2.drawLine(49, 6, 57, 14);
    u8g2.drawLine(56, 6, 48, 14);
    u8g2.drawLine(57, 6, 49, 14);

    u8g2.setFont(u8g2_font_micro_tr);
    int txtWidth = u8g2.getStrWidth("License Error!");
    u8g2.setCursor((72 - txtWidth) / 2, 38);
    u8g2.print("License Error!");
  } else {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setCursor(0, 15);
    u8g2.print(msg1);
    u8g2.setCursor(0, 30);
    u8g2.print(msg2);
  }
  u8g2.sendBuffer();
}
#else
inline void initOLED() {}
inline void updateOLED(int mode, const char *msg1 = "", const char *msg2 = "",
                       int badgeCount = 0) {}
#endif

inline void tickLED() {
  ledState = !ledState;
  digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
}

WiFiManagerParameter *p_custom_bot_token = nullptr;
WiFiManagerParameter *p_custom_chat_id = nullptr;
Ticker rebootTicker;

void rebootTask() { ESP.restart(); }

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("\n📡 [WiFiManager] 설정 포털 AP가 열렸습니다!");
  Serial.printf("👉 기기(스마트폰/노트북)에서 '%s' Wi-Fi로 접속해 주세요.\n",
                myWiFiManager->getConfigPortalSSID().c_str());
  Serial.println("👉 연결 후 자동으로 뜨는 창이나 인터넷 주소창에서 "
                 "192.168.4.1을 입력하시면 됩니다.\n");
}

void saveConfigCallback() {
  Serial.println(
      "\n📝 [WiFiManager] 메뉴에서 'Save(저장)' 버튼이 클릭되었습니다!");
  if (p_custom_bot_token != nullptr && p_custom_chat_id != nullptr) {
    strncpy(telegram_bot_token, p_custom_bot_token->getValue(),
            sizeof(telegram_bot_token));
    strncpy(telegram_chat_id, p_custom_chat_id->getValue(),
            sizeof(telegram_chat_id));
    saveConfig();
    Serial.println("✅ 사용자 텔레그램 정보 추출 및 메모리 저장 완료!");
    Serial.println("🔄 핸드폰에 'Saved' 화면이 출력될 시간을 주기 위해 3초 "
                   "대기 후 기기를 강제 재부팅합니다!");
    rebootTicker.attach(3.0, rebootTask);
  }
}

// --- [보안 및 시스템 헬퍼 함수] ---
String generateLicenseKey(String mac, String secret) {
  String input = mac + secret;
  byte shaResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *)input.c_str(), input.length());
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx);

  String hashStr = "";
  for (int i = 0; i < 32; i++) {
    char str[3];
    sprintf(str, "%02x", (int)shaResult[i]);
    hashStr += str;
  }
  return hashStr;
}

void loadConfig() {
  prefs.begin("sos_app", true);
  String t = prefs.getString("bot_token", "");
  String c = prefs.getString("chat_id", "");
  if (t.length() > 0)
    strncpy(telegram_bot_token, t.c_str(), sizeof(telegram_bot_token));
  if (c.length() > 0)
    strncpy(telegram_chat_id, c.c_str(), sizeof(telegram_chat_id));

  registeredButtonCount = 0;
  for (int i = 0; i < MAX_BUTTONS; i++) {
    char key[10];
    snprintf(key, sizeof(key), "btn_%d", i);
    unsigned long code = prefs.getULong(key, 0);
    if (code > 0)
      savedButtonCodes[registeredButtonCount++] = code;
  }
  prefs.end();
}

void saveConfig() {
  prefs.begin("sos_app", false);
  prefs.clear();
  prefs.putString("bot_token", telegram_bot_token);
  prefs.putString("chat_id", telegram_chat_id);
  for (int i = 0; i < registeredButtonCount; i++) {
    char key[10];
    snprintf(key, sizeof(key), "btn_%d", i);
    prefs.putULong(key, savedButtonCodes[i]);
  }
  prefs.end();
}

// --- [GitHub OTA 버전 관리 및 업데이트 함수] ---
void performOTA() {
  Serial.println("\n🔄 [OTA 업데이트] 깃허브 원격 서버에서 최신 펌웨어 버전을 "
                 "확인합니다...");
  updateOLED(4, "Checking Ver...");
  bot->sendMessage(telegram_chat_id,
                   "🔍 최신 펌웨어 버전이 있는지 확인 중입니다...", "");

  // ========================================================
  // [1단계] GitHub에서 version.txt 내용 읽어오기
  // ========================================================
  WiFiClientSecure client;
  client.setInsecure(); // 인증서 무시

  HTTPClient http;
  // GitHub의 리다이렉션 추적
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, GITHUB_VERSION_URL);
  http.setAuthorization(GITHUB_USER, GITHUB_PAT); // Private 저장소 접근 권한

  int httpCode = http.GET();
  Serial.printf("📡 깃허브 서버 연결 완료 (HTTP 응답 코드: %d)\n", httpCode);
  String serverVersion = "";

  if (httpCode == HTTP_CODE_OK) {
    serverVersion = http.getString();
    serverVersion.trim(); // 가져온 텍스트의 공백이나 엔터값 제거
    Serial.printf("✅ 웹 문서 읽기 완료 (기존 버전: %s / 최신 서버 버전: %s)\n",
                  CURRENT_VERSION.c_str(), serverVersion.c_str());
  } else {
    Serial.println("❌ 버전 확인에 실패했습니다. (토큰 유효성, 저장소 이름, "
                   "Wi-Fi 불안정 여부를 점검해주세요)");
    bot->sendMessage(
        telegram_chat_id,
        "❌ 버전 확인 실패. GitHub 서버에 접근할 수 없습니다. (HTTP " +
            String(httpCode) + ")",
        "");
    updateOLED(4, "Ver Check Fail");
    http.end();
    return;
  }
  http.end();

  // ========================================================
  // [2단계] 버전 비교 및 판단
  // ========================================================
  if (serverVersion == CURRENT_VERSION) {
    Serial.println("ℹ️ 기기에 완전히 동일한 최신 버전이 설치되어 있으므로, "
                   "펌웨어 다운로드를 건너뜁니다.");
    bot->sendMessage(telegram_chat_id,
                     "ℹ️ 현재 최신 버전(" + CURRENT_VERSION +
                         ")을 사용 중입니다. 업데이트를 건너뜁니다.",
                     "");
    updateOLED(3, "Up to date!"); // 대기 화면으로 복귀
    return;                       // 함수 종료 (.bin 다운로드 안 함)
  }

  // ========================================================
  // [3단계] 새 버전일 경우 펌웨어 다운로드 및 덮어쓰기
  // ========================================================
  bot->sendMessage(telegram_chat_id,
                   "🔄 새 버전(" + serverVersion +
                       ") 발견! 업데이트를 시작합니다.\n"
                       "수 분이 걸릴 수 있으니, 장치의 전원을 끄지 마세요!",
                   "");
  updateOLED(4, "Updating OTA...");
  Serial.println("🔥 버전 불일치 감지! 최신 펌웨어 패키지(.bin) 다운로드 및 "
                 "기기 쓰기를 진짜로 시작합니다.");
  Serial.println("⚠️ 수 분이 걸릴 수 있으며, 업데이트 도중에는 절대 기기의 "
                 "전원을 끄지 마세요!");

  WiFiClientSecure otaClient;
  otaClient.setInsecure();
  // 펌웨어 다운로드 시에도 리다이렉션 추적 필수
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.setAuthorization(GITHUB_USER, GITHUB_PAT);

  // 자동으로 재부팅되는 것을 막고, 완료 후 로그를 출력하기 위해 설정
  httpUpdate.rebootOnUpdate(false);

  // 다운로드 진행률을 10% 단위로 깔끔하게 줄바꿈하여 출력
  httpUpdate.onProgress([](size_t cur, size_t total) {
    static int lastPercent = 0;
    if (cur == 0)
      lastPercent = 0; // 0바이트 시작 시 초기화

    int percent = (cur * 100) / total;
    if (percent >= lastPercent + 10 || cur == total) {
      Serial.printf("📥 플래시 메모리 다운로드 및 쓰기 진행 중... %d%% 완료 "
                    "(%d / %d 바이트)\n",
                    percent, (int)cur, (int)total);
      lastPercent = (percent / 10) * 10;
    }
  });

  Serial.println(
      "📡 펌웨어 서버에 원격 다운로드를 요청합니다. 잠시만 기다려주세요...");
  t_httpUpdate_return ret = httpUpdate.update(otaClient, GITHUB_OTA_URL);

  if (ret == HTTP_UPDATE_FAILED) {
    String errStr = httpUpdate.getLastErrorString();
    int errCode = httpUpdate.getLastError();
    Serial.println(); // 줄바꿈 보정
    Serial.printf("❌ 펌웨어 업데이트 실패! 오류 코드 (%d): %s\n", errCode,
                  errStr.c_str());
    Serial.flush();
    bot->sendMessage(telegram_chat_id, "❌ 업데이트 실패: " + errStr, "");
    updateOLED(4, "OTA Failed");
    delay(3000);
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println();
    Serial.println("ℹ️ 적용할 수 있는 새 업데이트 파일(.bin)이 서버에 발견되지 "
                   "않았습니다.");
    Serial.flush();
    bot->sendMessage(telegram_chat_id,
                     "ℹ️ 깃허브 서버에서 업데이트용 파일(.bin)을 찾을 수 없어 "
                     "취소되었습니다.",
                     "");
    updateOLED(4, "No Updates");
    delay(3000);
  } else if (ret == HTTP_UPDATE_OK) {
    Serial.println();
    Serial.println(
        "✅ 파일 검증 및 메모리 시스템 덮어쓰기가 완벽히 마무리되었습니다!");
    Serial.println("🔄 새로운 펌웨어 시스템 파티션으로 교체하고, 기기를 정상 "
                   "재부팅합니다...");
    Serial.flush();
    updateOLED(4, "OTA Success!");
    bot->sendMessage(telegram_chat_id,
                     "✅ 서버 펌웨어 업데이트 적용 완료! 기기가 재부팅되며 새 "
                     "버전으로 켜집니다.",
                     "");
    delay(3000);
    ESP.restart();
  }
}

void handleTelegramCommands(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot->messages[i].chat_id;
    String text = bot->messages[i].text;

    // 인가된 가족 톡방에서 온 명령어만 허용 (보안 검증)
    if (chat_id == String(telegram_chat_id)) {
      if (text == "/update") {
        bot->sendMessage(chat_id, "🔄 원격 펌웨어 업데이트를 시작합니다...",
                         "");

        // [중요 버그 수정] OTA 도중 기기가 재부팅되면 남아있는 메시지 큐 때문에
        // 부팅 직후 또 다시 /update가 반복실행되는 이른바 '유령 메시지' 현상을
        // 막기 위해, 즉시 텔레그램 서버로 읽음 확인(Offset) 신호를 전송합니다.
        bot->getUpdates(bot->last_message_received + 1);

        performOTA();
      } else if (text == "/status") {
        bot->sendMessage(chat_id,
                         "✅ 시스템 정상 가동 중\n(버전: " + CURRENT_VERSION +
                             ", 등록된 버튼: " + String(registeredButtonCount) +
                             "개)",
                         "");
      }
    }
  }
}

void sendAlertToApp(long btnCode, int btnIndex) {
  Serial.printf(
      "📡 텔레그램으로 긴급 알림 전송을 시도합니다... (%d번 버튼, 코드: %lu)\n",
      btnIndex, btnCode);
  updateOLED(4, "Sending Alert...");
  char msgBuffer[120];
  snprintf(msgBuffer, sizeof(msgBuffer),
           "🚨 긴급 알림 🚨\n\n가족의 %d번 SOS 버튼이 방금 눌렸습니다! "
           "신속히 살펴봐주세요.\n",
           btnIndex);
  if (bot != nullptr && bot->sendMessage(telegram_chat_id, msgBuffer, "")) {
    Serial.println("✅ 텔레그램 메시지 전송이 완료되었습니다!");
    updateOLED(4, "Alert Sent!");
  } else {
    Serial.println("❌ 텔레그램 메시지 전송에 실패했습니다. 공유기 상태나 토큰 "
                   "값을 확인해 주세요.");
    updateOLED(4, "Alert Failed...");
  }
}

// =========================================================================
// 메인 프로그램 시작
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  // MAC 주소를 정상적으로 읽기 위해 Wi-Fi 모듈을 활성화합니다.
  WiFi.mode(WIFI_STA);

  // OLED 화면을 가장 먼저 켜주어야 에러 아이콘을 출력할 수 있습니다.
  initOLED();
  updateOLED(0, "System Booting...");

  // 1. [보안 검문소] 디지털 라이선스(Anti-Cloning) 검증
  String currentMAC = WiFi.macAddress();
  String expectedKey = generateLicenseKey(currentMAC, SECRET_KEY);

  prefs.begin("security", true);
  String savedKey = prefs.getString("license", "");
  prefs.end();

  if (savedKey == "" || savedKey != expectedKey) {
    Serial.println(
        "🚨 락 해제 실패: 불법 복제 기기 감지 (화면에 라이선스 오류 표출)");
    updateOLED(6); // 자물쇠(라이선스 오류) 아이콘을 OLED에 띄움
    while (true) {
      delay(1000);
    } // 시스템 강제 정지
  }
  Serial.println("✅ 정품 인증 완료!");

  // 2. 시스템 초기화
  loadConfig();

  pinMode(PAIRING_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);

  // 3. WiFiManager (텔레그램 정보 동적 입력)
  WiFiManager wm;
  wm.setDebugOutput(
      true); // 시리얼 모니터에 WiFiManager의 상세 상태(메뉴 진입 등) 표시 허용
  wm.setAPCallback(configModeCallback);

  wm.setShowPassword(true); // 눈 모양 아이콘 추가
  String setupPwd = wm.getWiFiPass();
  if (setupPwd.length() == 0)
    setupPwd = WiFi.psk();
  if (setupPwd.length() > 0) {
    setupPwd.replace("'", "\\'"); // JS 문법 오류 방지 방어코드
    String js = "<script>setTimeout(function(){var "
                "e=document.getElementById('p');if(e)e.value='" +
                setupPwd + "';},100);</script>";
    wm.setCustomHeadElement(js.c_str());
  }

  if (p_custom_bot_token)
    delete p_custom_bot_token;
  if (p_custom_chat_id)
    delete p_custom_chat_id;
  p_custom_bot_token = new WiFiManagerParameter(
      "bot_token", "Telegram Bot Token", telegram_bot_token, 62);
  p_custom_chat_id = new WiFiManagerParameter("chat_id", "Telegram Chat ID",
                                              telegram_chat_id, 30);
  wm.addParameter(p_custom_bot_token);
  wm.addParameter(p_custom_chat_id);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setSaveParamsCallback(saveConfigCallback);

  if (!wm.getWiFiIsSaved()) {
    Serial.println("🌐 Wi-Fi 정보가 없습니다. 관리자 모드(AP: "
                   "SOS_Gateway_Setup)로 진입합니다.");
    ledTicker.attach(0.5, tickLED);
    updateOLED(1);
    if (!wm.startConfigPortal("SOS_Gateway_Setup")) {
      Serial.println("❌ 설정 포털 진입 실패 및 취소. 대기 중...");
      delay(3000);
      ESP.restart();
    }
    // saveConfigCallback에서 재부팅하므로 여기까지 일반적으로 도달하지
    // 않습니다.
    Serial.println("✅ 설정 포털 종료됨 (올바른 Wi-Fi 입력 완료).");
    ledTicker.detach();
    digitalWrite(STATUS_LED_PIN, LOW);
  } else {
    String savedSSID = wm.getWiFiSSID();
    if (savedSSID.length() == 0)
      savedSSID = WiFi.SSID();
    bool isConnected = false;
    bool forceConfig = false;
    int failCount = 0;

    // 무한 루프나 긴 대기 시간 중 물리 버튼(1.5초 유지) 감지를 수행하는 타이머
    // 인터럽트성 람다 함수
    auto waitAndCheckBtn = [](int waitMs) -> bool {
      unsigned long startMs = millis();
      while (millis() - startMs < waitMs) {
        if (digitalRead(PAIRING_BUTTON_PIN) == LOW) {
          unsigned long pressMs = millis();
          while (digitalRead(PAIRING_BUTTON_PIN) == LOW) {
            if (millis() - pressMs > 1500)
              return true; // 1.5초 꾹 누름 감지 시 탈출
            delay(10);
          }
        }
        delay(20);
      }
      return false;
    };

    while (!isConnected && !forceConfig) {
      Serial.printf("📡 와이파이 접속 시도 중... (SSID: %s)\n",
                    savedSSID.c_str());
      char connMsg[30];
      snprintf(connMsg, sizeof(connMsg), "SSID: %s", savedSSID.c_str());
      updateOLED(1, connMsg);

      // 2초간 화면 유지 및 버튼 감시
      if (waitAndCheckBtn(2000)) {
        forceConfig = true;
        break;
      }

      WiFi.begin();
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        // 원래 0.5초 대기 시에도 버튼 누름 여부 감시
        if (waitAndCheckBtn(500)) {
          forceConfig = true;
          break;
        }
        Serial.print(".");
        attempts++;
      }
      Serial.println();

      if (forceConfig)
        break;

      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("✅ 와이파이 접속 성공! (IP: %s)\n",
                      WiFi.localIP().toString().c_str());
        isConnected = true;
      } else {
        failCount++;
        if (failCount >= 3) {
          Serial.println("\n❌ 3회 연속 접속 실패! 멈춤 방지를 위해 설정 "
                         "모드(AP)로 자동 전환합니다.");
          forceConfig = true;
          break;
        }

        Serial.printf("❌ 와이파이('%s') 접속 오류 발생! 비밀번호 변경이나 "
                      "공유기 꺼짐 상태를 확인해주세요.\n",
                      savedSSID.c_str());
        Serial.println("🔄 5초 대기... (이 기간 동안 물리 패어링 버튼을 "
                       "1.5초간 꾹 누르면 설정 모드로 강제 이동합니다)");
        char errMsg[30];
        snprintf(errMsg, sizeof(errMsg), "SSID: %s", savedSSID.c_str());
        updateOLED(5, errMsg);

        // 5초간 에러화면 유지 및 강제진입 버튼 대기
        if (waitAndCheckBtn(5000)) {
          forceConfig = true;
          break;
        }
      }
    }

    // 버튼 강제 탈출 또는 3회 실패 자동 진입 발동 시
    if (forceConfig) {
      while (digitalRead(PAIRING_BUTTON_PIN) == LOW)
        delay(10); // 손을 뗄 때까지 확실히 대기 (후속 오작동 방지)

      Serial.println("🌐 [재설정 진입] 관리자 모드(AP: SOS_Gateway_Setup) "
                     "포털을 개방합니다.");
      ledTicker.attach(0.5, tickLED);
      updateOLED(1, "WIFI Setup Mode");

      // 3분(180초) 동안 재설계 입력이 없으면, 튕겨서 재부팅하고 와이파이를
      // 무식하게 다시 물도록 함 (정전 복구 지연 대비)
      wm.setConfigPortalTimeout(180);

      if (!wm.startConfigPortal("SOS_Gateway_Setup")) {
        Serial.println("⏳ 3분 타임아웃 경과(입력 없음). 상태 리셋을 위해 "
                       "시스템을 완전 재시작합니다!");
        delay(3000);
        ESP.restart();
      }

      Serial.println("✅ 포털을 통해 새로운 와이파이 설정정보가 "
                     "입력되었습니다. 재부팅을 준비합니다...");
      ledTicker.detach();
      digitalWrite(STATUS_LED_PIN, LOW);
    }
  }

  // 4. 텔레그램 & RF 수신기 세팅
  secured_client.setInsecure(); // 메모리 확보
  secured_client.setTimeout(
      6); // 서버 응답 대기 시간을 최대 6초로 제한 (단위: 초)
  secured_client.setHandshakeTimeout(
      6); // 암호화 통신 연결(TLS) 대기 시간도 최대 6초로 제한
  bot = new UniversalTelegramBot(telegram_bot_token, secured_client);

  mySwitch.enableReceive(digitalPinToInterrupt(RF_RECEIVER_PIN));
  mySwitch.setReceiveTolerance(80); // QT-01A 수신 감도 최적화 패치 적용

  if (registeredButtonCount > 0) {
    char buf[20];
    snprintf(buf, sizeof(buf), "Reg: %d/20", registeredButtonCount);
    updateOLED(3, buf);
  } else {
    updateOLED(3, "No pair!");
  }
}

void loop() {
  // 1. 물리 버튼 (페어링 및 초기화)
  if (digitalRead(PAIRING_BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(PAIRING_BUTTON_PIN) == LOW) {
      unsigned long pressTime = millis();
      bool isLongPress = false;
      bool oledChanged = false;
      Serial.println("🔘 물리 버튼 누름 감지! (짧게 누름: 스마트폰 SOS 버튼 "
                     "페어링 / 3초 유지: Wi-Fi 설정 활성화)");
      updateOLED(2, "", "Hold 3s: WIFI", registeredButtonCount);

      while (digitalRead(PAIRING_BUTTON_PIN) == LOW) {
        if (millis() - pressTime > 3000) {
          isLongPress = true;
          if (!oledChanged) {
            Serial.println("⏳ 3초 경과: Wi-Fi 설정 모드 진입 조건이 "
                           "충족되었습니다! (버튼에서 손을 떼주세요)");
            updateOLED(1);
            oledChanged = true;
          }
          break;
        }
        delay(10);
      }
      while (digitalRead(PAIRING_BUTTON_PIN) == LOW) {
        delay(10);
      }
      digitalWrite(STATUS_LED_PIN, LOW);

      if (isLongPress) {
        Serial.println("🌐 [버튼 입력] Wi-Fi 설정 모드로 초기화 및 진입합니다. "
                       "(AP: SOS_Gateway_Setup)");
        updateOLED(1);
        ledTicker.attach(0.5, tickLED);
        WiFiManager wm;
        wm.setDebugOutput(true);
        wm.setAPCallback(configModeCallback);

        wm.setShowPassword(true); // 눈 모양 아이콘 추가
        String loopPwd = wm.getWiFiPass();
        if (loopPwd.length() == 0)
          loopPwd = WiFi.psk();
        if (loopPwd.length() > 0) {
          loopPwd.replace("'", "\\'"); // 자바스크립트 에러 방지
          String js = "<script>setTimeout(function(){var "
                      "e=document.getElementById('p');if(e)e.value='" +
                      loopPwd + "';},100);</script>";
          wm.setCustomHeadElement(js.c_str());
        }

        if (p_custom_bot_token)
          delete p_custom_bot_token;
        if (p_custom_chat_id)
          delete p_custom_chat_id;
        p_custom_bot_token = new WiFiManagerParameter(
            "bot_token", "Telegram Bot Token", telegram_bot_token, 62);
        p_custom_chat_id = new WiFiManagerParameter(
            "chat_id", "Telegram Chat ID", telegram_chat_id, 30);
        wm.addParameter(p_custom_bot_token);
        wm.addParameter(p_custom_chat_id);
        wm.setSaveConfigCallback(saveConfigCallback);
        wm.setSaveParamsCallback(saveConfigCallback);

        Serial.println("📡 설정 페이지 여는 중...");
        wm.startConfigPortal("SOS_Gateway_Setup");
        Serial.println("✅ 설정 포털 종료됨. (저장 후 자동으로 재부팅됩니다)");
        delay(2000);
        ESP.restart();
      } else {
        Serial.println("🎯 RF 버튼 페어링 모드로 진입합니다. (최대 20초 대기)");
        backupButtonCount = registeredButtonCount;
        for (int i = 0; i < MAX_BUTTONS; i++)
          backupButtonCodes[i] = savedButtonCodes[i];

        isPairingMode = true;
        pairingStartTime = millis();
        registeredButtonCount = 0;
        for (int i = 0; i < MAX_BUTTONS; i++)
          savedButtonCodes[i] = 0;
        ledTicker.attach(0.1, tickLED);
        updateOLED(2, "", "Wait signal", registeredButtonCount);
      }
    }
  }

  if (isPairingMode && (millis() - pairingStartTime > PAIRING_TIMEOUT)) {
    isPairingMode = false;
    ledTicker.detach();
    digitalWrite(STATUS_LED_PIN, LOW);

    if (registeredButtonCount == 0 && backupButtonCount > 0) {
      registeredButtonCount = backupButtonCount;
      for (int i = 0; i < MAX_BUTTONS; i++)
        savedButtonCodes[i] = backupButtonCodes[i];
    } else {
      saveConfig();
    }

    char buf[20];
    snprintf(buf, sizeof(buf), "Reg: %d/20", registeredButtonCount);
    updateOLED(3, buf);
  }

  // 2. RF 신호 처리 (필터링 강화)
  if (mySwitch.available()) {
    unsigned long receivedCode = mySwitch.getReceivedValue();
    unsigned int receivedBitlength = mySwitch.getReceivedBitlength();
    unsigned int receivedProtocol = mySwitch.getReceivedProtocol();

    // 1527 프로토콜 & 24비트 규격 엄격 검사
    if (receivedCode != 0 && receivedBitlength == 24 && receivedProtocol == 1) {
      if (isPairingMode) {
        bool isDuplicate = false;
        for (int i = 0; i < registeredButtonCount; i++) {
          if (savedButtonCodes[i] == receivedCode) {
            isDuplicate = true;
            break;
          }
        }
        if (!isDuplicate && registeredButtonCount < MAX_BUTTONS) {
          savedButtonCodes[registeredButtonCount++] = receivedCode;
          Serial.printf("✅ 새 버튼 인식 및 등록 완료! (식별 코드: %lu) - 현재 "
                        "등재 현황: %d/20\n",
                        receivedCode, registeredButtonCount);
          char pairMsg[20];
          snprintf(pairMsg, sizeof(pairMsg), "Button: %d/20",
                   registeredButtonCount);
          updateOLED(2, "", pairMsg, registeredButtonCount);
          ledTicker.detach();
          digitalWrite(STATUS_LED_PIN, HIGH);
          delay(500);
          ledTicker.attach(0.1, tickLED);

          if (registeredButtonCount >= MAX_BUTTONS) {
            isPairingMode = false;
            ledTicker.detach();
            digitalWrite(STATUS_LED_PIN, LOW);
            saveConfig();
            char buf[20];
            snprintf(buf, sizeof(buf), "Reg: %d/20", registeredButtonCount);
            updateOLED(3, buf);
          }
        }
      } else {
        bool isRegistered = false;
        int buttonIndex = -1;
        for (int i = 0; i < registeredButtonCount; i++) {
          if (receivedCode == savedButtonCodes[i]) {
            isRegistered = true;
            buttonIndex = i + 1; // 1번부터 시작하는 순번
            break;
          }
        }
        if (isRegistered) {
          Serial.printf("🚨 SOS 위급 상황! 등록된 %d번 버튼 신호 수신됨 (버튼 "
                        "코드: %lu)\n",
                        buttonIndex, receivedCode);
          if (millis() > alertDisplayExpireTime) {
            if (WiFi.status() == WL_CONNECTED)
              sendAlertToApp(receivedCode, buttonIndex);
            else
              updateOLED(4, "WiFi Offline!");

            alertDisplayExpireTime = millis() + 2000;
          }
        }
      }
    }
    mySwitch.resetAvailable();
  }

  // 3. 텔레그램 원격 명령 수신 (10초 주기 폴링)
  if (!isPairingMode && millis() - lastBotCheckTime > BOT_CHECK_INTERVAL) {
    if (WiFi.status() == WL_CONNECTED && bot != nullptr) {
      int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
      while (numNewMessages) {
        handleTelegramCommands(numNewMessages);
        numNewMessages = bot->getUpdates(bot->last_message_received + 1);
      }
    }
    lastBotCheckTime = millis();
  }

  // 4. 대기 애니메이션 (RF 방해 없는 비동기 업데이트)
  if (!isPairingMode && millis() > alertDisplayExpireTime) {
    if (millis() - lastStandbyAnimTime > 150) {
      lastStandbyAnimTime = millis();
      standbyAnimFrame += standbyAnimDir;
      if (standbyAnimFrame >= 4) {
        standbyAnimFrame = 4;
        standbyAnimDir = -1;
      } else if (standbyAnimFrame <= 0) {
        standbyAnimFrame = 0;
        standbyAnimDir = 1;
      }
      char buf[20];
      snprintf(buf, sizeof(buf), "Reg: %d/20", registeredButtonCount);
      updateOLED(3, buf);
    }
  }
}
