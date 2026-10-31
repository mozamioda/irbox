#include <WiFi.h>
#include <WebServer.h>

const int RECV_PIN = 15;
const int MAXN = 300;
const uint32_t GAP_US = 20000;   // أي فترة أكبر من 20 ms تعتبر نهاية الإطار

volatile uint32_t t[MAXN];
volatile uint16_t n = 0;
volatile uint32_t lastT = 0;

void IRAM_ATTR onEdge() {
  uint32_t now = micros();
  if (n < MAXN) {
    t[n] = now - lastT;
    n++;
  }
  lastT = now;
}

WebServer server(80);

void setup() {
  pinMode(RECV_PIN, INPUT);
  WiFi.softAP("IR-Box", "12345678");
  lastT = micros();
  attachInterrupt(digitalPinToInterrupt(RECV_PIN), onEdge, CHANGE);

  server.on("/", []() {
    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#111;color:#eee;padding:14px'>"
      "<a href='/reset'><button style='font-size:20px;padding:14px;width:100%'>Reset</button></a>"
      "<p>Edges: <b id='c'>0</b></p>"
      "<textarea id='r' style='width:100%;height:200px;background:#222;color:#0f0'></textarea>"
      "<script>setInterval(async()=>{const d=await(await fetch('/data')).json();"
      "document.getElementById('c').textContent=d.raw.length;"
      "document.getElementById('r').value=d.raw.join(',');},1000);</script>"
      "</body></html>");
  });

  server.on("/reset", []() {
    n = 0;
    lastT = micros();
    server.sendHeader("Location", "/");
    server.send(303);
  });

  // يرسل الإطار الأول فقط، وينتهي بفترة الصمت
  server.on("/data", []() {
    String s = "{\"freq\":38,\"raw\":[";
    uint16_t c = n;
    bool first = true;
    for (uint16_t i = 1; i < c; i++) {
      if (!first) s += ",";
      first = false;
      s += String(t[i]);
      if (i > 1 && t[i] > GAP_US) break;   // وصلنا لفترة الصمت بعد الإطار
    }
    s += "]}";
    server.send(200, "application/json", s);
  });

  server.begin();
}

void loop() {
  server.handleClient();
}
