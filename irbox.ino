#include <WiFi.h>
#include <WebServer.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

const uint16_t RECV_PIN = 15;
const uint16_t SEND_PIN = 4;
const uint16_t FREQ_KHZ = 38;
const unsigned long LEARN_TIMEOUT = 15000;

IRrecv irrecv(RECV_PIN, 1024, 15, true);
IRsend irsend(SEND_PIN);
decode_results results;
WebServer server(80);

uint16_t *lastRaw = nullptr;
uint16_t lastLen = 0;
String lastProtocol = "none";
String lastHex = "";
bool learning = false;
unsigned long learnStart = 0;

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IR Box</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;padding:14px}
button{padding:12px;margin:6px 0;width:100%;font-size:16px;border:0;border-radius:8px;background:#2d6cdf;color:#fff}
.box{background:#1d1d1d;padding:12px;border-radius:10px;margin-bottom:12px}
textarea{width:100%;height:150px;background:#222;color:#0f0;font-size:12px}
</style></head><body>
<div class="box">
<button onclick="cmd('/learn')">Learn (press remote within 15s)</button>
<button onclick="cmd('/send')">Send last captured</button>
</div>
<div class="box">
Status: <b id="st">-</b><br>
Protocol: <b id="pr">-</b><br>
Hex: <b id="hx">-</b><br>
Pulses: <b id="pc">0</b>
</div>
<div class="box">
<div>Raw timings (microseconds, alternating mark/space):</div>
<textarea id="raw" readonly></textarea>
</div>
<script>
let lastText = '';
function cmd(path){ fetch(path).then(refresh); }
async function refresh(){
  try{
    const d = await (await fetch('/data')).json();
    document.getElementById('st').textContent = d.learning ? 'Waiting for remote...' : 'Idle';
    document.getElementById('pr').textContent = d.protocol;
    document.getElementById('hx').textContent = d.hex;
    document.getElementById('pc').textContent = d.raw.length;
    const txt = d.raw.join(',');
    if (txt !== lastText){ document.getElementById('raw').value = txt; lastText = txt; }
  }catch(e){
    document.getElementById('st').textContent = 'Connection lost';
  }
}
setInterval(refresh, 1000);
refresh();
</script></body></html>
)rawliteral";

void storeCapture() {
  if (lastRaw) { free(lastRaw); lastRaw = nullptr; }
  lastLen = getCorrectedRawLength(&results);
  lastRaw = resultToRawArray(&results);
  lastProtocol = typeToString(results.decode_type, results.repeat);
  lastHex = resultToHexidecimal(&results);
}

String buildJson() {
  String s = "{";
  s += "\"learning\":" + String(learning ? "true" : "false") + ",";
  s += "\"protocol\":\"" + lastProtocol + "\",";
  s += "\"hex\":\"" + lastHex + "\",";
  s += "\"freq\":" + String(FREQ_KHZ) + ",";
  s += "\"raw\":[";
  for (uint16_t i = 0; i < lastLen; i++) {
    if (i) s += ",";
    s += String(lastRaw[i]);
  }
  s += "]}";
  return s;
}

void setup() {
  Serial.begin(115200);
  irsend.begin();
  pinMode(RECV_PIN, INPUT);

  WiFi.softAP("IR-Box", "12345678");
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/data", []() { server.send(200, "application/json", buildJson()); });
  server.on("/learn", []() {
    irrecv.enableIRIn();
    learning = true;
    learnStart = millis();
    server.send(200, "text/plain", "OK");
  });
  server.on("/send", []() {
    if (lastRaw && lastLen > 0) {
      irsend.sendRaw(lastRaw, lastLen, FREQ_KHZ);
      server.send(200, "text/plain", "sent");
    } else {
      server.send(400, "text/plain", "nothing captured");
    }
  });
  server.begin();
}

void loop() {
  server.handleClient();

  if (learning) {
    if (irrecv.decode(&results)) {
      storeCapture();
      irrecv.disableIRIn();
      irrecv.resume();
      learning = false;
    } else if (millis() - learnStart > LEARN_TIMEOUT) {
      irrecv.disableIRIn();
      learning = false;
    }
  }
}
