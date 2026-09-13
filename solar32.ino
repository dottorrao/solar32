#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "time.h"

// --- PINOUT TFT SPI ---
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    2
#define PIN_RESET_WIFI 0

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
Preferences preferences;

// Server NTP per l'orario italiano
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;      // UTC +1 (Italia)
const int   daylightOffset_sec = 3600; // Ora legale (+1 ora)

// Parametri Impianto & Coordinate predefinite
String latitude = "43.92";
String longitude = "11.03";
float  kWpImpianto = 6.0;
float  orientamentoGradi = 90.0; // Gradi bussola: 0=Nord, 90=Est, 180=Sud, 270=Ovest
int    inclinazione = 30;

// Dati Meteo & Produzione (Indice 0 = Oggi, 1..6 = Giorni successivi)
float radiazioneIstantanea = 0.0;
float previsioni7Giorni[7]; // Valori in kWh totali stimati per la giornata
String nomiGiorni[7];        // Nomi brevi dei giorni (es. Dom, Lun...)

unsigned long ultimoAggiornamentoMeteo = 0;
const unsigned long intervalloMeteo = 300000; // Aggiorna ogni 5 minuti

// Schermata "Info Prodotto" temporanea sul display LCD
bool mostraInfoLCD = false;
unsigned long timestampInfoLCD = 0;
const unsigned long durataInfoLCD = 6000; // 6 secondi

// --- OTTIENI L'ORA CORRENTE FORMATTATA ---
String getOraAttuale() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "--:--";
  }
  char timeStringBuff[10];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
  return String(timeStringBuff);
}

// --- CONVERTE I GRADI BUSSOLA NELL'AZIMUTH RICHIESTO DA OPEN-METEO ---
// Bussola: 0 = Nord, 90 = Est, 180 = Sud, 270 = Ovest (senso orario)
// Open-Meteo: 0 = Sud, -90 = Est, 90 = Ovest, ±180 = Nord
float calcolaAzimuth() {
  float azimuth = orientamentoGradi - 180.0;
  if (azimuth < -180.0) azimuth += 360.0;
  if (azimuth > 180.0) azimuth -= 360.0;
  return azimuth;
}

// --- FUNZIONE PER CALCOLARE IL FATTORE DI CONVERSIONE K PERSONALIZZATO ---
// Orientamento e inclinazione sono già considerati da Open-Meteo tramite i
// parametri tilt/azimuth (vedi aggiornaDatiSolari): qui restano solo le
// caratteristiche dell'impianto che l'API non può conoscere.
float calcolaFattoreK() {
  // kWp * Performance Ratio (PR 0.947)
  return kWpImpianto * 0.947;
}

// --- FUNZIONE PER LEGGERE L'API OPEN-METEO (7 GIORNI, IRRADIANZA SUL PIANO INCLINATO) ---
void aggiornaDatiSolari() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    String url = "http://api.open-meteo.com/v1/forecast?latitude=" + latitude +
                 "&longitude=" + longitude +
                 "&current=global_tilted_irradiance_instant" +
                 "&hourly=global_tilted_irradiance" +
                 "&tilt=" + String(inclinazione) +
                 "&azimuth=" + String(calcolaAzimuth(), 0) +
                 "&forecast_days=7&timezone=auto";

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        // 1. Dato istantaneo in W/m² sul piano inclinato del pannello
        radiazioneIstantanea = doc["current"]["global_tilted_irradiance_instant"];

        // 2. Previsioni per 7 giorni: si somma l'irradianza oraria (24 valori
        //    per giorno, in W/m² medi sull'ora = Wh/m² per quell'ora) per
        //    ottenere il totale giornaliero in kWh/m² sul piano del pannello
        struct tm timeinfo;
        getLocalTime(&timeinfo);
        int giornoSettimanaOggi = timeinfo.tm_wday;
        const char* giorniBrevi[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};

        float fattoreK = calcolaFattoreK();

        for (int i = 0; i < 7; i++) {
          float totaleWhm2 = 0;
          for (int h = 0; h < 24; h++) {
            float valOrario = doc["hourly"]["global_tilted_irradiance"][i * 24 + h];
            totaleWhm2 += valOrario;
          }
          float radiazioneKWhm2 = totaleWhm2 / 1000.0; // Conversione Wh/m² -> kWh/m²

          // Stima complessiva totale della produzione giornaliera in kWh
          previsioni7Giorni[i] = radiazioneKWhm2 * fattoreK;

          nomiGiorni[i] = giorniBrevi[(giornoSettimanaOggi + i) % 7];
        }
        Serial.println("Dati meteo e stime produzione aggiornati!");
      }
    }
    http.end();
  }
}

// --- CALLBACK PER CONFIGURAZIONE WIFI ---
void configModeCallback(WiFiManager *myWiFiManager) {
  tft.fillScreen(ST7735_BLUE);
  tft.setCursor(5, 10);
  tft.setTextColor(ST7735_YELLOW);
  tft.setTextSize(1);
  tft.println("--- CONFIG WIFI ---");
  tft.setCursor(5, 30);
  tft.setTextColor(ST7735_WHITE);
  tft.println("Connettiti alla rete:");
  tft.setTextColor(ST7735_GREEN);
  tft.println("ESP32-Control-AP");
}

// --- RENDERING SCHERMO TFT CON GRIGLIA (stile card, coerente con la dashboard web) ---
void aggiornaDisplay() {
  uint16_t colBg        = tft.color565(18, 18, 18);    // sfondo nero carbone (#121212)
  uint16_t colTopBarTx  = tft.color565(120, 144, 156); // ora/IP: grigio ardesia neutro (#78909C)
  uint16_t colNowBg     = tft.color565(129, 199, 132); // box NOW: verde salvia (#81C784)
  uint16_t colOggiBg    = tft.color565(100, 181, 246); // box OGGI TOT: azzurro polvere (#64B5F6)
  uint16_t colLightBoxTx= tft.color565(33, 33, 33);    // testo scuro leggibile sulle card chiare
  uint16_t colDayBg     = tft.color565(30, 41, 59);    // box giorni: grigio scuro opaco (#1E293B)
  uint16_t colDayLabelTx= tft.color565(176, 190, 197); // etichette grigio chiaro sui box giorni
  uint16_t colDayValueTx= tft.color565(253, 224, 71);  // valori kWh in giallo tenue (#FDE047)
  uint16_t colDayBorder = tft.color565(60, 70, 80);    // bordo sottile per separare i box giorni dal nero

  tft.fillScreen(colBg);

  // 1. RIGA SUPERIORE: ORA E IP (testo neutro, informazioni di servizio in secondo piano)
  tft.setCursor(4, 4);
  tft.setTextColor(colTopBarTx);
  tft.setTextSize(1);
  tft.print(getOraAttuale());

  String ipStr = WiFi.localIP().toString();
  int xIP = 160 - (int)(ipStr.length() * 6) - 2; // allineato a destra, con margine
  if (xIP < 40) xIP = 40; // non sovrapporre mai l'orario a sinistra
  tft.setCursor(xIP, 4);
  tft.setTextColor(colTopBarTx);
  tft.print(ipStr);

  tft.drawFastHLine(0, 14, 160, colTopBarTx);

  // 2. DATI GIORNO CORRENTE (due card affiancate: istantaneo e totale oggi)
  tft.fillRoundRect(2, 16, 76, 38, 5, colNowBg);
  tft.fillRoundRect(82, 16, 76, 38, 5, colOggiBg);

  // --- CARD SINISTRA: ISTANTANEO (NOW) ---
  String strNowTitle = "NOW";
  int xNowTitle = 2 + (76 - (int)(strNowTitle.length() * 6)) / 2;
  tft.setCursor(xNowTitle, 19);
  tft.setTextColor(colLightBoxTx);
  tft.setTextSize(1);
  tft.print(strNowTitle);

  String strNowVal = String((int)radiazioneIstantanea);
  int xNowVal = 2 + (76 - (int)(strNowVal.length() * 12)) / 2;
  tft.setCursor(xNowVal, 28);
  tft.setTextColor(colLightBoxTx);
  tft.setTextSize(2);
  tft.print(strNowVal);

  String strNowUnit = "W/m2";
  int xNowUnit = 2 + (76 - (int)(strNowUnit.length() * 6)) / 2;
  tft.setCursor(xNowUnit, 45);
  tft.setTextSize(1);
  tft.setTextColor(colLightBoxTx);
  tft.print(strNowUnit);

  // --- CARD DESTRA: TOTALE OGGI ---
  String strOggiTitle = "OGGI TOT";
  int xOggiTitle = 82 + (76 - (int)(strOggiTitle.length() * 6)) / 2;
  tft.setCursor(xOggiTitle, 19);
  tft.setTextColor(colLightBoxTx);
  tft.setTextSize(1);
  tft.print(strOggiTitle);

  String strOggiVal = String(previsioni7Giorni[0], 1);
  int xOggiVal = 82 + (76 - (int)(strOggiVal.length() * 12)) / 2;
  tft.setCursor(xOggiVal, 28);
  tft.setTextColor(colLightBoxTx);
  tft.setTextSize(2);
  tft.print(strOggiVal);

  String strOggiUnit = "kWh";
  int xOggiUnit = 82 + (76 - (int)(strOggiUnit.length() * 6)) / 2;
  tft.setCursor(xOggiUnit, 45);
  tft.setTextSize(1);
  tft.setTextColor(colLightBoxTx);
  tft.print(strOggiUnit);

  // 3. PREVISIONI PROSSIMI 6 GIORNI IN kWh TOTALI (GRIGLIA 2x3)
  int startY = 57;
  int colWidth = 53;
  int rowHeight = 33;

  for (int i = 1; i <= 6; i++) {
    int idx = i - 1;
    int col = idx % 3;
    int row = idx / 3;

    int x = col * colWidth + 2;
    int y = startY + (row * rowHeight);

    tft.fillRoundRect(x, y, colWidth - 4, rowHeight - 4, 4, colDayBg);
    tft.drawRoundRect(x, y, colWidth - 4, rowHeight - 4, 4, colDayBorder);

    // Nome giorno in grassetto (font base senza bold: si simula ridisegnando con 1px di scarto)
    tft.setCursor(x + 4, y + 3);
    tft.setTextColor(colDayLabelTx);
    tft.setTextSize(1);
    tft.print(nomiGiorni[i]);
    tft.setCursor(x + 5, y + 3);
    tft.print(nomiGiorni[i]);

    tft.setCursor(x + 4, y + 13);
    tft.setTextColor(colDayValueTx);
    tft.setTextSize(1);
    tft.print(previsioni7Giorni[i], 1);
    tft.setTextColor(colDayLabelTx);
    tft.print("kWh");
  }
}

// --- SCHERMATA TEMPORANEA "INFO PRODOTTO" SUL DISPLAY LCD ---
void mostraInfoProdottoLCD() {
  tft.fillScreen(ST7735_BLACK);

  // Logo: la "o" di "Solar32" sostituita da un'icona sole (stesso logo delle pagine web)
  tft.setCursor(4, 6);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(2);
  tft.print("S");

  int cx = 28, cy = 14, r = 7;
  tft.fillCircle(cx, cy, r, ST7735_YELLOW);
  for (int a = 0; a < 360; a += 45) {
    float rad = a * 3.14159 / 180.0;
    int x1 = cx + (r + 2) * cos(rad);
    int y1 = cy + (r + 2) * sin(rad);
    int x2 = cx + (r + 6) * cos(rad);
    int y2 = cy + (r + 6) * sin(rad);
    tft.drawLine(x1, y1, x2, y2, ST7735_YELLOW);
  }

  tft.setCursor(42, 6);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(2);
  tft.println("lar32");

  tft.setTextSize(1);
  tft.setCursor(4, 28);
  tft.setTextColor(ST7735_YELLOW);
  tft.println("di Marco Ruggeri");

  tft.drawFastHLine(0, 40, 160, ST7735_BLUE);

  tft.setCursor(4, 48);
  tft.setTextColor(ST7735_WHITE);
  tft.println("Monitoraggio e previsione");
  tft.setCursor(4, 58);
  tft.println("produzione fotovoltaica");
  tft.setCursor(4, 68);
  tft.println("con dati Open-Meteo,");
  tft.setCursor(4, 78);
  tft.println("adattati a orientamento,");
  tft.setCursor(4, 88);
  tft.println("inclinazione e potenza");
  tft.setCursor(4, 98);
  tft.println("del tuo impianto.");
}

// --- LOGO SVG ("Solar32" con la "o" sostituita da un sole) RIUSATO NELLE PAGINE WEB ---
String logoSvgHtml() {
  return "<svg width='108' height='26' viewBox='0 0 132 32' style='vertical-align:middle;margin-right:6px;'>"
         "<text x='2' y='25' font-size='26' font-weight='bold' fill='currentColor'>S</text>"
         "<circle cx='30' cy='15' r='8' fill='#f5a623'/>"
         "<g stroke='#f5a623' stroke-width='1.5' stroke-linecap='round'>"
         "<line x1='40' y1='15' x2='44' y2='15'/>"
         "<line x1='37' y1='22' x2='40' y2='25'/>"
         "<line x1='30' y1='25' x2='30' y2='29'/>"
         "<line x1='23' y1='22' x2='20' y2='25'/>"
         "<line x1='20' y1='15' x2='16' y2='15'/>"
         "<line x1='23' y1='8' x2='20' y2='5'/>"
         "<line x1='30' y1='5' x2='30' y2='1'/>"
         "<line x1='37' y1='8' x2='40' y2='5'/>"
         "</g>"
         "<text x='44' y='25' font-size='26' font-weight='bold' fill='currentColor'>lar32</text>"
         "</svg>";
}

// --- PAGINA WEB DASHBOARD ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Solar Forecast ESP32</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; }";
  html += "body { font-family: Arial, sans-serif; background: #eceff1; margin:0; padding:0; color:#333; }";
  html += ".layout { display: flex; min-height: 100vh; }";
  html += ".sidebar { width: 170px; background: #263238; color: white; display: flex; flex-direction: column; flex-shrink: 0; }";
  html += ".sidebar .brand { padding: 20px 15px; font-size: 20px; font-weight: bold; color: #4db6ac; border-bottom: 1px solid #37474f; display: flex; align-items: center; }";
  html += ".navbtn { background: none; border: none; color: #cfd8dc; text-align: left; padding: 14px 18px; font-size: 15px; cursor: pointer; border-left: 4px solid transparent; }";
  html += ".navbtn:hover { background: #37474f; }";
  html += ".navbtn.active { background: #37474f; color: white; border-left: 4px solid #00897b; font-weight: bold; }";
  html += ".content { flex: 1; padding: 20px; max-width: 480px; }";
  html += ".section { display: none; }";
  html += ".section.active { display: block; }";
  html += "h2 { margin-top:0; }";
  html += ".card-block { background: white; padding: 16px; border-radius: 12px; margin-bottom: 15px; box-shadow: 0 2px 6px rgba(0,0,0,0.08); }";
  html += ".flex-container { display: flex; justify-content: space-between; gap: 10px; margin: 5px 0; }";
  html += ".box-val { background: #f1f8e9; padding: 12px; border-radius: 8px; width: 50%; text-align:center; }";
  html += ".val { font-size: 26px; color: #2e7d32; font-weight: bold; margin-top: 5px; }";
  html += ".val-tot { font-size: 26px; color: #0277bd; font-weight: bold; margin-top: 5px; }";
  html += ".grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }";
  html += ".day-box { background: #f0f4c3; padding: 10px; border-radius: 8px; font-size: 14px; text-align:center; }";
  html += "label { display:block; font-size: 13px; color:#555; margin-top:10px; }";
  html += "input[type=text], select { width: 100%; padding: 8px; margin-top:4px; border: 1px solid #ccc; border-radius: 6px; }";
  html += "button.submit { background: #00897b; color: white; border: none; padding: 10px 20px; border-radius: 6px; font-size: 16px; cursor: pointer; margin-top: 16px; width:100%; }";
  html += "button.info { background: #37474f; color:white; border:none; padding:10px 20px; border-radius:6px; font-size:16px; cursor:pointer; width:100%; margin-top:10px; }";
  html += "button.danger { background: #d32f2f; color:white; border:none; padding:10px 20px; border-radius:6px; font-size:16px; cursor:pointer; width:100%; margin-top:10px; }";
  html += "a { text-decoration:none; }";
  html += "@media (max-width: 600px) {";
  html += "  .layout { flex-direction: column; }";
  html += "  .sidebar { width: 100%; flex-direction: row; overflow-x:auto; }";
  html += "  .sidebar .brand { display:none; }";
  html += "  .navbtn { flex:1; text-align:center; border-left:none; border-bottom: 4px solid transparent; }";
  html += "  .navbtn.active { border-left:none; border-bottom: 4px solid #00897b; }";
  html += "  .content { max-width: 100%; }";
  html += "}";
  html += "</style></head><body>";

  html += "<div class='layout'>";
  html += "<div class='sidebar'>";
  html += "<div class='brand'>" + logoSvgHtml() + "</div>";
  html += "<button class='navbtn active' id='btn-monitor' onclick=\"showSection('monitor')\">Monitoraggio</button>";
  html += "<button class='navbtn' id='btn-config' onclick=\"showSection('config')\">Impostazioni</button>";
  html += "</div>";

  html += "<div class='content'>";

  // SEZIONE 1: MONITORAGGIO (dati istantanei e previsioni)
  html += "<section id='monitor' class='section active'>";
  html += "<h2>Monitor & Previsioni</h2>";
  html += "<p style='margin:0 0 10px; color:#666;'>Ora locale: <b>" + getOraAttuale() + "</b></p>";
  html += "<div class='card-block'>";
  html += "<div class='flex-container'>";
  html += "<div class='box-val'><div style='font-size:12px; color:#555;'>Istantaneo</div><div class='val'>" + String((int)radiazioneIstantanea) + " W/m&sup2;</div></div>";
  html += "<div class='box-val'><div style='font-size:12px; color:#555;'>Previsto Oggi</div><div class='val-tot'>" + String(previsioni7Giorni[0], 1) + " kWh</div></div>";
  html += "</div></div>";
  html += "<div class='card-block'>";
  html += "<h3 style='margin-top:0;'>Prossimi Giorni (kWh)</h3>";
  html += "<div class='grid'>";
  for (int i = 1; i <= 6; i++) {
    html += "<div class='day-box'><b>" + nomiGiorni[i] + "</b><br>" + String(previsioni7Giorni[i], 1) + " kWh</div>";
  }
  html += "</div></div>";
  html += "</section>";

  // SEZIONE 2: IMPOSTAZIONI (configurazione impianto e azioni)
  html += "<section id='config' class='section'>";
  html += "<h2>Impostazioni</h2>";
  html += "<div class='card-block'>";
  html += "<form action='/setlocation' method='POST'>";
  html += "<label>Latitudine</label><input type='text' name='lat' value='" + latitude + "'>";
  html += "<label>Longitudine</label><input type='text' name='lon' value='" + longitude + "'>";
  html += "<label>Potenza Impianto (kWp)</label><input type='text' name='kwp' value='" + String(kWpImpianto) + "'>";
  html += "<label>Inclinazione (&deg;)</label><input type='text' name='incl' value='" + String(inclinazione) + "'>";
  html += "<label>Orientamento bussola (&deg;: 0=Nord, 90=Est, 180=Sud, 270=Ovest)</label>";
  html += "<input type='number' name='orient' min='0' max='360' step='1' value='" + String(orientamentoGradi, 0) + "'>";
  html += "<button type='submit' class='submit'>Salva Configurazione</button>";
  html += "</form>";
  html += "</div>";
  html += "<a href='/about'><button class='info'>Info Prodotto</button></a>";
  html += "<a href='/resetwifi' onclick=\"return confirm('Reset Wi-Fi?');\"><button class='danger'>Reset Wi-Fi</button></a>";
  html += "</section>";

  html += "</div>"; // fine .content
  html += "</div>"; // fine .layout

  html += "<script>";
  html += "function showSection(id){";
  html += "document.querySelectorAll('.section').forEach(function(s){s.classList.remove('active');});";
  html += "document.getElementById(id).classList.add('active');";
  html += "document.querySelectorAll('.navbtn').forEach(function(b){b.classList.remove('active');});";
  html += "document.getElementById('btn-'+id).classList.add('active');";
  html += "}";
  html += "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

// --- PAGINA PRESENTAZIONE PRODOTTO ---
void handleAbout() {
  // Mostra le informazioni anche sul display LCD fisico per qualche secondo
  mostraInfoProdottoLCD();
  mostraInfoLCD = true;
  timestampInfoLCD = millis();

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>SOLAR32 - Presentazione</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; background: #eceff1; margin:0; padding:20px; }";
  html += ".card { background: white; padding: 30px; border-radius: 12px; max-width: 450px; margin: auto; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #00897b; margin-bottom: 5px; display: flex; align-items: center; justify-content: center; }";
  html += ".author { font-size: 16px; color: #555; margin-bottom: 20px; font-weight: bold; }";
  html += "p { color: #444; font-size: 15px; line-height: 1.6; text-align: left; }";
  html += "button { background: #00897b; color: white; border: none; padding: 10px 20px; border-radius: 6px; font-size: 16px; cursor: pointer; margin-top: 20px; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h1>" + logoSvgHtml() + "</h1>";
  html += "<div class='author'>di Marco Ruggeri</div>";
  html += "<p><b>SOLAR32</b> e un sistema IoT avanzato basato su ESP32 progettato per il monitoraggio in tempo reale e la previsione intelligente della produzione di impianti fotovoltaici.</p>";
  html += "<p>Integrando i dati meteorologici open-source ad alta precisione di <i>Open-Meteo</i> e adattandoli dinamicamente alla geometria specifica del tetto (potenza di picco, orientamento e inclinazione), offre stime accurate della produzione energetica direttamente su display locale e interfaccia web.</p>";
  html += "<a href='/'><button>Torna alla Dashboard</button></a>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleSetLocation() {
  if (server.hasArg("lat") && server.hasArg("lon")) {
    latitude = server.arg("lat");
    longitude = server.arg("lon");
    if (server.hasArg("kwp")) kWpImpianto = server.arg("kwp").toFloat();
    if (server.hasArg("incl")) inclinazione = server.arg("incl").toInt();
    if (server.hasArg("orient")) orientamentoGradi = server.arg("orient").toFloat();

    preferences.begin("solar_cfg", false);
    preferences.putString("lat", latitude);
    preferences.putString("lon", longitude);
    preferences.putFloat("kwp", kWpImpianto);
    preferences.putInt("incl", inclinazione);
    preferences.putFloat("orientDeg", orientamentoGradi);
    preferences.end();

    aggiornaDatiSolari();
    aggiornaDisplay();
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void handleResetWiFi() {
  server.send(200, "text/html", "<h2>Reset Wi-Fi in corso...</h2>");
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RESET_WIFI, INPUT_PULLUP);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST7735_BLACK);

  // Carica configurazione memorizzata
  preferences.begin("solar_cfg", true);
  latitude = preferences.getString("lat", "43.92");
  longitude = preferences.getString("lon", "11.03");
  kWpImpianto = preferences.getFloat("kwp", 6.0);
  inclinazione = preferences.getInt("incl", 30);
  orientamentoGradi = preferences.getFloat("orientDeg", 90.0);
  preferences.end();

  WiFiManager wm;
  wm.setAPCallback(configModeCallback);

  if (digitalRead(PIN_RESET_WIFI) == LOW) {
    wm.resetSettings();
    delay(1000);
  }

  bool res = wm.autoConnect("ESP32-Control-AP");

  if (!res) {
    ESP.restart();
  }

  // Sincronizzazione Orologio via NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Configurazione Server Web
  server.on("/", handleRoot);
  server.on("/about", handleAbout);
  server.on("/setlocation", HTTP_POST, handleSetLocation);
  server.on("/resetwifi", handleResetWiFi);
  server.begin();

  // Primo download e rendering
  aggiornaDatiSolari();
  aggiornaDisplay();
}

void loop() {
  server.handleClient();

  // Ripristina il display normale dopo che la schermata "Info Prodotto" è scaduta
  if (mostraInfoLCD && (millis() - timestampInfoLCD >= durataInfoLCD)) {
    mostraInfoLCD = false;
    aggiornaDisplay();
  }

  // Polling automatico ogni 5 minuti
  unsigned long corrente = millis();
  if (corrente - ultimoAggiornamentoMeteo >= intervalloMeteo) {
    ultimoAggiornamentoMeteo = corrente;
    aggiornaDatiSolari();
    if (!mostraInfoLCD) aggiornaDisplay();
  }
}