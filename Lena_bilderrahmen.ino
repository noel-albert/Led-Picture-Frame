#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WS2812FX.h>
#include <WiFiManager.h> 
#include <EEPROM.h>

// --- HARDWARE KONFIGURATION ---
#define LED_PIN      D1
#define LED_COUNT    88

// --- BENUTZERDATEN ---
const char* www_username = "lena";
const char* www_password = "rahmen"; 
const char* hostname = "bilderrahmen"; 
const char* ap_ssid = "Lenas Bilderrahmen";
const char* ap_pass = "lichtan123";

// --- ZONEN ---
#define ZONE_WALL_START  0
#define ZONE_WALL_STOP   35
#define ZONE_PIC_START   36
#define ZONE_PIC_STOP    87

WS2812FX ws2812fx = WS2812FX(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
ESP8266WebServer server(80);

// --- GLOBALE VARIABLEN ---
uint8_t masterBri = 128;
uint8_t zoneBri[2] = {255, 255}; 
// Wir merken uns, ob eine Zone gerade im "Custom Gradient" Modus ist
bool isCustomGradient[2] = {false, false};
// Speicher für die 3 Gradienten-Farben pro Zone
uint32_t gradColors[2][3] = {
  {0xFF0000, 0x00FF00, 0x0000FF}, // Zone 0 defaults
  {0xFF0000, 0x00FF00, 0x0000FF}  // Zone 1 defaults
};

// Struktur zum Speichern der Presets
struct PresetData {
  uint8_t valid; // Marker ob Daten da sind (0xA5)
  uint8_t masterBri;
  struct ZoneData {
    uint8_t mode;
    uint32_t color;
    uint8_t bri;
    uint16_t speed;
    bool isGrad;
    uint32_t gC[3];
  } zones[2];
};

// --- HELPER FUNKTIONEN ---

// Hex String zu uint32
uint32_t hexToColor(String hex) {
  return (uint32_t) strtol(hex.c_str(), NULL, 16);
}

// Lineare Interpolation zwischen zwei Farben
uint32_t interpolateColor(uint32_t c1, uint32_t c2, float factor) {
  uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
  uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
  
  uint8_t r = r1 + (r2 - r1) * factor;
  uint8_t g = g1 + (g2 - g1) * factor;
  uint8_t b = b1 + (b2 - b1) * factor;
  
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Malt den Gradienten auf die LEDs (Statisch)
void renderGradient(int zone) {
  int start = (zone == 0) ? ZONE_WALL_START : ZONE_PIC_START;
  int stop  = (zone == 0) ? ZONE_WALL_STOP  : ZONE_PIC_STOP;
  int count = stop - start + 1;

  // Stoppe den WS2812FX Effekt für diese Zone, damit wir malen können
  ws2812fx.setMode(zone, FX_MODE_STATIC);
  
  // Zonen-Helligkeit beachten (Master macht die Lib automatisch)
  // Um es einfach zu halten, berechnen wir die Farben voll und lassen die Lib dimmen
  
  for(int i = 0; i < count; i++) {
    float pos = (float)i / (count - 1);
    uint32_t color;
    
    if (pos < 0.5) {
      // Erste Hälfte: Farbe 1 zu Farbe 2
      float localPos = pos * 2.0;
      color = interpolateColor(gradColors[zone][0], gradColors[zone][1], localPos);
    } else {
      // Zweite Hälfte: Farbe 2 zu Farbe 3
      float localPos = (pos - 0.5) * 2.0;
      color = interpolateColor(gradColors[zone][1], gradColors[zone][2], localPos);
    }
    
    // Pixel setzen (Globale Indexierung)
    ws2812fx.setPixelColor(start + i, color);
  }
  ws2812fx.show();
}

// Helligkeit verrechnen für normale Modi
uint32_t dimColor(uint32_t color, uint8_t brightness) {
  uint8_t r = (color >> 16) & 0xFF; uint8_t g = (color >> 8) & 0xFF; uint8_t b = color & 0xFF;
  r = (r * brightness) / 255; g = (g * brightness) / 255; b = (b * brightness) / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void applySettings(int zone) {
  if (isCustomGradient[zone]) {
    renderGradient(zone);
  } else {
    // Wenn normaler Modus, Farbe aktualisieren (gedimmt)
    uint32_t c = ws2812fx.getColor(zone); // Aktuelle Basis-Farbe holen? Schwierig.
    // Wir lassen den Effekt laufen, rufen nur trigger auf falls nötig
    ws2812fx.trigger(); 
  }
}


// --- HTML ---
const char index_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>Lenas Bilderrahmen</title>
  <style>
    body { font-family: 'Segoe UI', sans-serif; background-color: #1a1a1a; color: #eee; text-align: center; margin: 0; padding: 0; padding-bottom: 50px; }
    .header { background: #2d2d2d; padding: 15px; display: flex; justify-content: space-between; align-items: center; box-shadow: 0 2px 10px rgba(0,0,0,0.5); }
    .header h1 { margin: 0; font-size: 1.2rem; color: #ff79c6; }
    .settings-btn { font-size: 1.5rem; text-decoration: none; color: #888; border: none; background: none; cursor: pointer; }
    
    .tabs { display: flex; width: 100%; background: #333; }
    .tab { flex: 1; padding: 15px; background: #333; border: none; color: #888; font-size: 1rem; cursor: pointer; border-bottom: 3px solid transparent; }
    .tab.active { color: #fff; background: #444; border-bottom: 3px solid #bd93f9; }
    
    .card { background: #2d2d2d; margin: 15px; padding: 15px; border-radius: 15px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
    h3 { margin-top: 0; color: #8be9fd; font-size: 1rem; text-align: left; }
    
    /* Controls Layout */
    .controls { display: flex; justify-content: center; align-items: center; gap: 15px; padding: 10px; }
    #colorWheel { width: 180px; height: 180px; background: transparent; border-radius: 50%; cursor: crosshair; touch-action: none; }
    
    .slider-wrapper { display: flex; flex-direction: column; align-items: center; height: 180px; justify-content: center; }
    .v-slider { -webkit-appearance: none; width: 160px; height: 15px; background: #444; border-radius: 10px; outline: none; transform: rotate(-90deg); margin: 70px -70px; }
    .v-slider::-webkit-slider-thumb { -webkit-appearance: none; width: 25px; height: 25px; border-radius: 50%; background: #bd93f9; cursor: pointer; }

    select, input[type=color] { width: 100%; padding: 10px; background: #444; color: white; border: none; border-radius: 8px; font-size: 1rem; margin-bottom: 10px; height: 45px; }
    .h-slider { -webkit-appearance: none; width: 100%; height: 10px; background: #444; border-radius: 5px; outline: none; margin-top: 10px; }
    .h-slider::-webkit-slider-thumb { -webkit-appearance: none; width: 20px; height: 20px; border-radius: 50%; background: #50fa7b; cursor: pointer; }

    /* Presets */
    .preset-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; margin-top: 10px; }
    .preset-btn { background: #444; color: #fff; border: 1px solid #555; padding: 10px; border-radius: 8px; cursor: pointer; }
    .preset-save { background: #bd93f9; color: #000; font-weight: bold; margin-top: 5px; font-size: 0.8rem; }
    
    /* Gradient Pickers */
    .grad-pickers { display: flex; gap: 5px; }
  </style>
</head>
<body>

  <div class="header">
    <h1>Lenas Bilderrahmen</h1>
    <a href="/settings" class="settings-btn">&#9881;</a>
  </div>

  <div class="tabs">
    <button class="tab active" onclick="setTab(0)">Wand</button>
    <button class="tab" onclick="setTab(1)">Bild</button>
  </div>

  <div class="card">
    <div class="controls">
      <canvas id="colorWheel" width="200" height="200"></canvas>
      <div class="slider-wrapper">
        <input type="range" min="0" max="255" value="255" class="v-slider" id="zoneBri" onchange="updateZoneBri(this.value)">
      </div>
      <div class="slider-wrapper">
        <input type="range" min="0" max="255" value="128" class="v-slider" id="masterBri" style="background:#555" onchange="updateMaster(this.value)">
      </div>
    </div>
    <div style="display:flex; justify-content:center; gap:30px; margin-top:-10px; font-size:0.8rem; color:#888;">
       <span>Farbe</span><span>Zone Hell</span><span>Master Hell</span>
    </div>
  </div>

  <div class="card">
    <h3>Effekte</h3>
    <select id="fxSelect" onchange="setEffect(this.value)">
      <option value="0">Festes Licht</option>
      <option value="99">--- Eigener Farbverlauf ---</option>
      <option value="2">Atmen</option>
      <option value="3">Wischen</option>
      <option value="8">Regenbogen</option>
      <option value="11">Regenbogen Zyklus</option>
      <option value="12">Theater</option>
      <option value="14">Lauflicht Dual</option>
      <option value="42">Feuerwerk</option>
      <option value="45">Feuerflackern</option>
    </select>

    <div id="gradControls" style="display:none;">
      <div class="grad-pickers">
        <input type="color" id="gc1" value="#ff0000" onchange="updateGrad()">
        <input type="color" id="gc2" value="#00ff00" onchange="updateGrad()">
        <input type="color" id="gc3" value="#0000ff" onchange="updateGrad()">
      </div>
      <p style="font-size:0.8rem; color:#aaa; margin:5px 0;">Wähle 3 Farben für den Verlauf</p>
    </div>

    <label style="color:#50fa7b; display:block; text-align:left; margin-top:10px;">Geschwindigkeit:</label>
    <input type="range" min="100" max="5000" value="1000" class="h-slider" style="direction: rtl" onchange="setSpeed(this.value)">
  </div>

  <div class="card">
    <h3>Presets (Szenen)</h3>
    <div class="preset-grid">
      <div><button class="preset-btn" onclick="loadPreset(0)">1</button><button class="preset-btn preset-save" onclick="savePreset(0)">Save</button></div>
      <div><button class="preset-btn" onclick="loadPreset(1)">2</button><button class="preset-btn preset-save" onclick="savePreset(1)">Save</button></div>
      <div><button class="preset-btn" onclick="loadPreset(2)">3</button><button class="preset-btn preset-save" onclick="savePreset(2)">Save</button></div>
      <div><button class="preset-btn" onclick="loadPreset(3)">4</button><button class="preset-btn preset-save" onclick="savePreset(3)">Save</button></div>
    </div>
  </div>

<script>
  let activeZone = 0;
  const canvas = document.getElementById('colorWheel');
  const ctx = canvas.getContext('2d');
  const radius = canvas.width / 2;

  // --- Canvas Drawing & Logic ---
  function drawWheel() {
    const image = ctx.createImageData(200, 200);
    const data = image.data;
    for (let x = -100; x < 100; x++) {
      for (let y = -100; y < 100; y++) {
        const r = Math.sqrt(x*x + y*y);
        const phi = Math.atan2(y, x);
        if (r < 100) {
          const deg = (phi + Math.PI) / (2 * Math.PI) * 360;
          const [red, green, blue] = hsvToRgb(deg, r / 100, 1);
          const index = ((y + 100) * 200 + (x + 100)) * 4;
          data[index] = red; data[index+1] = green; data[index+2] = blue; data[index+3] = 255;
        }
      }
    }
    ctx.putImageData(image, 0, 0);
  }
  
  function hsvToRgb(h, s, v) {
    let r, g, b, i, f, p, q, t;
    h /= 60; i = Math.floor(h); f = h - i; p = v * (1 - s); q = v * (1 - s * f); t = v * (1 - s * (1 - f));
    switch (i % 6) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        case 5: r = v, g = p, b = q; break;
    }
    return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
  }

  function pickColor(e) {
    const rect = canvas.getBoundingClientRect();
    const x = (e.touches ? e.touches[0].clientX : e.clientX) - rect.left - radius;
    const y = (e.touches ? e.touches[0].clientY : e.clientY) - rect.top - radius;
    const r = Math.sqrt(x*x + y*y); if(r > radius) return;
    const phi = Math.atan2(y, x);
    const deg = (phi + Math.PI) / (2 * Math.PI) * 360;
    const sat = r / radius; 
    const [red, green, blue] = hsvToRgb(deg, sat, 1);
    const hex = ((1 << 24) + (red << 16) + (green << 8) + blue).toString(16).slice(1);
    
    // Set Effect to Static automatically
    document.getElementById('fxSelect').value = "0";
    toggleGradControls(false);
    fetch('/set?z=' + activeZone + '&c=' + hex + '&m=0');
  }
  canvas.addEventListener('mousedown', pickColor); canvas.addEventListener('touchstart', pickColor);

  // --- UI Functions ---
  function setTab(zone) {
    activeZone = zone;
    document.querySelectorAll('.tab').forEach((t, i) => { t.classList.toggle('active', i === zone); });
    // Hier könnte man den aktuellen Status vom ESP abfragen (fetch), um die UI zu aktualisieren
  }
  
  function toggleGradControls(show) {
    document.getElementById('gradControls').style.display = show ? 'block' : 'none';
  }

  function setEffect(val) {
    if (val == "99") {
      toggleGradControls(true);
      updateGrad(); // Apply current picker colors
    } else {
      toggleGradControls(false);
      fetch('/set?z=' + activeZone + '&m=' + val);
    }
  }

  function updateGrad() {
    let c1 = document.getElementById('gc1').value.substring(1);
    let c2 = document.getElementById('gc2').value.substring(1);
    let c3 = document.getElementById('gc3').value.substring(1);
    fetch('/setgrad?z=' + activeZone + '&c1=' + c1 + '&c2=' + c2 + '&c3=' + c3);
  }

  function updateZoneBri(val) { fetch('/set?z=' + activeZone + '&zb=' + val); }
  function updateMaster(val) { fetch('/set?mb=' + val); }
  function setSpeed(val) { fetch('/set?z=' + activeZone + '&s=' + val); }
  
  function savePreset(id) { 
    if(confirm('Preset ' + (id+1) + ' speichern?')) fetch('/preset?act=save&id=' + id); 
  }
  function loadPreset(id) { fetch('/preset?act=load&id=' + id); }

  drawWheel();
</script>
</body>
</html>
)=====";

const char settings_html[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="de"><head><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>Einstellungen</title>
<style>body{background:#222;color:#fff;font-family:sans-serif;text-align:center;padding:20px;}
button{padding:15px;width:100%;margin-top:20px;background:#bd93f9;border:none;border-radius:5px;font-size:1.2rem;cursor:pointer;}
.warn{background:#ff5555; color:white;}</style></head>
<body><h1>Einstellungen</h1>
<p>Hier kannst du den Bilderrahmen mit deinem Heim-WLAN verbinden.</p>
<form action="/setup_wifi" method="POST"><button type="submit" class="warn">Mit Heim-WLAN verbinden</button></form>
<br><a href="/" style="color:#888; text-decoration:none; border:1px solid #555; padding:10px 20px; border-radius:5px;">Zurück</a>
</body></html>
)=====";

// --- SETUP & LOOP ---

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);

  // LED Setup
  ws2812fx.init();
  ws2812fx.setBrightness(masterBri);
  ws2812fx.setSegment(0, ZONE_WALL_START, ZONE_WALL_STOP, FX_MODE_STATIC, COLORS(RED), 1000, false);
  ws2812fx.setSegment(1, ZONE_PIC_START, ZONE_PIC_STOP, FX_MODE_STATIC, COLORS(BLUE), 1000, true);
  ws2812fx.start();

  // WLAN Setup
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_pass);
  WiFi.begin(); // Versucht alten Connect

  // MDNS
  if (MDNS.begin(hostname)) Serial.println("MDNS started");

  // --- SERVER ROUTEN ---
  server.on("/", []() {
    if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();
    server.send_P(200, "text/html", index_html);
  });

  server.on("/settings", []() {
    if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();
    server.send_P(200, "text/html", settings_html);
  });

  // PRESETS SPEICHERN / LADEN
  server.on("/preset", []() {
    if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();
    String act = server.arg("act");
    int id = server.arg("id").toInt();
    int addr = id * sizeof(PresetData);

    if (act == "save") {
      PresetData data;
      data.valid = 0xA5;
      data.masterBri = masterBri;
      for(int i=0; i<2; i++) {
        data.zones[i].mode = isCustomGradient[i] ? 99 : ws2812fx.getMode(i);
        data.zones[i].color = ws2812fx.getColor(i); // Note: getColor only returns simple colors
        data.zones[i].bri = zoneBri[i];
        data.zones[i].speed = ws2812fx.getSpeed(i);
        data.zones[i].isGrad = isCustomGradient[i];
        for(int k=0; k<3; k++) data.zones[i].gC[k] = gradColors[i][k];
      }
      EEPROM.put(addr, data);
      EEPROM.commit();
    } 
    else if (act == "load") {
      PresetData data;
      EEPROM.get(addr, data);
      if(data.valid == 0xA5) {
        masterBri = data.masterBri;
        ws2812fx.setBrightness(masterBri);
        for(int i=0; i<2; i++) {
          zoneBri[i] = data.zones[i].bri;
          isCustomGradient[i] = data.zones[i].isGrad;
          for(int k=0; k<3; k++) gradColors[i][k] = data.zones[i].gC[k];
          
          if(data.zones[i].isGrad || data.zones[i].mode == 99) {
            renderGradient(i);
          } else {
             ws2812fx.setMode(i, data.zones[i].mode);
             ws2812fx.setSpeed(i, data.zones[i].speed);
             // Farbe setzen (mit Dimming)
             uint32_t c = dimColor(data.zones[i].color, zoneBri[i]);
             ws2812fx.setColor(i, c); 
          }
        }
      }
    }
    server.send(200, "text/plain", "OK");
  });

  // GRADIENT SETZEN
  server.on("/setgrad", []() {
    if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();
    int z = server.arg("z").toInt();
    gradColors[z][0] = hexToColor(server.arg("c1"));
    gradColors[z][1] = hexToColor(server.arg("c2"));
    gradColors[z][2] = hexToColor(server.arg("c3"));
    isCustomGradient[z] = true;
    renderGradient(z);
    server.send(200, "text/plain", "OK");
  });

  // NORMALE API
  server.on("/set", []() {
    if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();
    int z = server.arg("z").toInt();
    
    if (server.arg("mb").length() > 0) { masterBri = server.arg("mb").toInt(); ws2812fx.setBrightness(masterBri); }
    if (server.arg("zb").length() > 0) {
      zoneBri[z] = server.arg("zb").toInt();
      if(isCustomGradient[z]) renderGradient(z); // Gradient neu zeichnen bei Helligkeitsänderung
      else {
        // Bei Static Mode Farbe updaten für Dimming
        if(ws2812fx.getMode(z) == FX_MODE_STATIC) {
           // Wir nutzen die aktuelle Farbe des Segments, aber das ist schwer auszulesen
           // Vereinfachung: Dimming greift bei nächster Farbänderung oder wir lassen es so.
           // WS2812FX handhabt Brightness global gut, aber per Segment nicht nativ.
        }
      }
    }
    if (server.arg("s").length() > 0) ws2812fx.setSpeed(z, server.arg("s").toInt());
    
    if (server.arg("m").length() > 0) {
      int m = server.arg("m").toInt();
      if (m == 99) { /* handled by client via setgrad calls */ } 
      else {
        isCustomGradient[z] = false;
        ws2812fx.setMode(z, m);
      }
    }
    
    if (server.arg("c").length() > 0) {
      isCustomGradient[z] = false; // Custom Gradient aus
      uint32_t c = hexToColor(server.arg("c"));
      // Hier dimmen wir die Farbe manuell basierend auf ZoneBrightness
      ws2812fx.setColor(z, dimColor(c, zoneBri[z]));
      ws2812fx.setMode(z, FX_MODE_STATIC);
    }
    server.send(200, "text/plain", "OK");
  });

  // WLAN SETUP TRIGGER (FIX)
  server.on("/setup_wifi", HTTP_POST, []() {
    if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();
    
    // 1. Antwort senden DAMIT der Browser nicht hängen bleibt
    server.send(200, "text/html", "<h1>Starte Setup...</h1><p>Bitte verbinde dich jetzt mit dem WLAN 'Lenas Bilderrahmen Setup' um das Heimnetz zu waehlen.</p>");
    delay(500); // Kurz warten bis Daten raus sind

    // 2. WiFiManager starten (blockierend)
    WiFiManager wm;
    wm.setTimeout(180);
    if (!wm.startConfigPortal("Lenas Bilderrahmen Setup")) {
      ESP.restart();
    }
    ESP.restart();
  });

  server.begin();
  MDNS.addService("http", "tcp", 80);
}

void loop() {
  ws2812fx.service();
  server.handleClient();
  MDNS.update();
  
  // Custom Gradient Refresh (falls wir Animation wollen, aktuell statisch, daher leer)
}