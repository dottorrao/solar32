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
String orientamento = "Est";
int    inclinazione = 30;

// Dati Meteo & Produzione (Indice 0 = Oggi, 1..6 = Giorni successivi)
float radiazioneIstantanea = 0.0;
float previsioni7Giorni[7]; // Valori in kWh totali stimati per la giornata
String nomiGiorni[7];        // Nomi brevi dei giorni (es. Dom, Lun...)

unsigned long ultimoAggiornamentoMeteo = 0;
const unsigned long intervalloMeteo = 300000; // Aggiorna ogni 5 minuti

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

// --- FUNZIONE PER CALCOLARE IL FATTORE DI CONVERSIONE K PERSONALIZZATO ---
float calcolaFattoreK() {
  float fattoreGeometrico = 0.92;
  if (orientamento.equalsIgnoreCase("Est")) fattoreGeometrico = 0.92;
  else if (orientamento.equalsIgnoreCase("Sud")) fattoreGeometrico = 1.0;
  else if (orientamento.equalsIgnoreCase("Ovest")) fattoreGeometrico = 0.92;

  // Fattore di inclinazione: penalizza lo scostamento dall'inclinazione ottimale (~30°)
  const float inclinazioneOttimale = 30.0;
  float scostamento = inclinazione - inclinazioneOttimale;
  float fattoreInclinazione = 1.0 - 0.00009 * scostamento * scostamento;
  if (fattoreInclinazione < 0.5) fattoreInclinazione = 0.5;

  // Moltiplicatore complessivo: kWp * fattore geometrico * fattore inclinazione * Performance Ratio (PR 0.947)
  return kWpImpianto * fattoreGeometrico * fattoreInclinazione * 0.947;
}

// --- FUNZIONE PER LEGGERE L'API OPEN-METEO (7 GIORNI) ---
void aggiornaDatiSolari() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    String url = "http://api.open-meteo.com/v1/forecast?latitude=" + latitude + 
                 "&longitude=" + longitude + 
                 "&current=shortwave_radiation&daily=shortwave_radiation_sum&forecast_days=7&timezone=auto";
    
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        // 1. Dato istantaneo in W/m²
        radiazioneIstantanea = doc["current"]["shortwave_radiation"];

        // 2. Previsioni per 7 giorni convertite in kWh totali di produzione impianto
        struct tm timeinfo;
        getLocalTime(&timeinfo);
        int giornoSettimanaOggi = timeinfo.tm_wday;
        const char* giorniBrevi[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};

        float fattoreK = calcolaFattoreK();

        for (int i = 0; i < 7; i++) {
          float radMJ = doc["daily"]["shortwave_radiation_sum"][i];
          float radiazioneKWhm2 = radMJ / 3.6; // Conversione in kWh/m²
          
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

// --- RENDERING SCHERMO TFT CON GRIGLIA ---
void aggiornaDisplay() {
  tft.fillScreen(ST7735_BLACK);
  
  // 1. RIGA SUPERIORE: ORA E IP
  tft.setCursor(4, 4);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.print(getOraAttuale());

  tft.setCursor(85, 4);
  tft.setTextColor(ST7735_MAGENTA);
  tft.print("IP:.");
  tft.print(WiFi.localIP()[3]);
  
  tft.drawFastHLine(0, 14, 160, ST7735_BLUE);

  // 2. DATI GIORNO CORRENTE (DIVISO IN DUE COLONNE CENTRATE: 0-80 e 80-160 px)
  
  // --- COLONNA SINISTRA: ISTANTANEO (NOW) ---
  String strNowTitle = "NOW";
  int xNowTitle = (80 - (strNowTitle.length() * 6)) / 2;
  tft.setCursor(xNowTitle, 17);
  tft.setTextColor(ST7735_YELLOW);
  tft.setTextSize(1);
  tft.print(strNowTitle);
  
  String strNowVal = String((int)radiazioneIstantanea);
  int xNowVal = (80 - (strNowVal.length() * 12)) / 2;
  tft.setCursor(xNowVal, 27);
  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(2);
  tft.print(strNowVal);
  
  String strNowUnit = "W/m2";
  int xNowUnit = (80 - (strNowUnit.length() * 6)) / 2;
  tft.setCursor(xNowUnit, 43);
  tft.setTextSize(1);
  tft.print(strNowUnit);

  // Separatore verticale centrale
  tft.drawFastVLine(80, 15, 37, ST7735_BLUE);

  // --- COLONNA DESTRA: TOTALE OGGI ---
  String strOggiTitle = "OGGI TOT";
  int xOggiTitle = 80 + (80 - (strOggiTitle.length() * 6)) / 2;
  tft.setCursor(xOggiTitle, 17);
  tft.setTextColor(ST7735_YELLOW);
  tft.setTextSize(1);
  tft.print(strOggiTitle);

  String strOggiVal = String(previsioni7Giorni[0], 1);
  int xOggiVal = 80 + (80 - (strOggiVal.length() * 12)) / 2;
  tft.setCursor(xOggiVal, 27);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(2);
  tft.print(strOggiVal);

  String strOggiUnit = "kWh";
  int xOggiUnit = 80 + (80 - (strOggiUnit.length() * 6)) / 2;
  tft.setCursor(xOggiUnit, 43);
  tft.setTextSize(1);
  tft.print(strOggiUnit);

  tft.drawFastHLine(0, 53, 160, ST7735_BLUE);

  // 3. PREVISIONI PROSSIMI 6 GIORNI IN kWh TOTALI (GRIGLIA 2x3, partendo da indice 1)
  int startY = 57;
  int colWidth = 53;
  int rowHeight = 35;

  for (int i = 1; i <= 6; i++) {
    int idx = i - 1;
    int col = idx % 3;
    int row = idx / 3;

    int x = col * colWidth + 2;
    int y = startY + (row * rowHeight);

    // Titolo Giorno
    tft.setCursor(x, y);
    tft.setTextColor(ST7735_WHITE);
    tft.setTextSize(1);
    tft.print(nomiGiorni[i]);

    // Valore previsto in kWh totali stimati
    tft.setCursor(x, y + 10);
    tft.setTextColor(ST7735_YELLOW);
    tft.setTextSize(1);
    tft.print(previsioni7Giorni[i], 1);
    
    tft.setTextColor(ST7735_WHITE);
    tft.setTextSize(1);
    tft.print("kWh");
  }
}

// --- PAGINA WEB DASHBOARD ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Solar Forecast ESP32</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; background: #eceff1; margin:0; padding:15px; }";
  html += ".card { background: white; padding: 20px; border-radius: 12px; max-width: 450px; margin: auto; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }";
  html += ".flex-container { display: flex; justify-content: space-around; margin: 15px 0; }";
  html += ".box-val { background: #f1f8e9; padding: 10px; border-radius: 8px; width: 45%; }";
  html += ".val { font-size: 26px; color: #2e7d32; font-weight: bold; margin-top: 5px; }";
  html += ".val-tot { font-size: 26px; color: #0277bd; font-weight: bold; margin-top: 5px; }";
  html += ".grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; margin: 15px 0; }";
  html += ".day-box { background: #f0f4c3; padding: 10px; border-radius: 8px; font-size: 14px; }";
  html += "input[type=text], select { width: 40%; padding: 8px; margin: 5px; border: 1px solid #ccc; border-radius: 6px; }";
  html += "button { background: #00897b; color: white; border: none; padding: 10px 20px; border-radius: 6px; font-size: 16px; cursor: pointer; margin-top: 10px; }";
  html += "button.info { background: #37474f; }";
  html += "button.danger { background: #d32f2f; }";
  html += "</style></head><body>";

  html += "<div class='card'>";
  html += "<h2>Monitor & Previsioni Solari</h2>";
  html += "<p style='margin:0; color:#666;'>Ora locale: <b>" + getOraAttuale() + "</b></p>";

  // Box affiancati per oggi: Istantaneo e Totale Giornaliero
  html += "<div class='flex-container'>";
  html += "<div class='box-val'><div style='font-size:12px; color:#555;'>Istantaneo</div><div class='val'>" + String((int)radiazioneIstantanea) + " W/m&sup2;</div></div>";
  html += "<div class='box-val'><div style='font-size:12px; color:#555;'>Previsto Oggi</div><div class='val-tot'>" + String(previsioni7Giorni[0], 1) + " kWh</div></div>";
  html += "</div>";

  // Griglia Previsioni Web in kWh totali (Prossimi 6 giorni)
  html += "<h3>Previsione Prossimi Giorni (kWh)</h3>";
  html += "<div class='grid'>";
  for (int i = 1; i <= 6; i++) {
    html += "<div class='day-box'><b>" + nomiGiorni[i] + "</b><br>" + String(previsioni7Giorni[i], 1) + " kWh</div>";
  }
  html += "</div><hr>";

  // Form Configurazione Impianto & Coordinate
  html += "<form action='/setlocation' method='POST'>";
  html += "Lat: <input type='text' name='lat' value='" + latitude + "'> ";
  html += "Lon: <input type='text' name='lon' value='" + longitude + "'><br>";
  html += "kWp: <input type='text' name='kwp' value='" + String(kWpImpianto) + "'> ";
  html += "Incl: <input type='text' name='incl' value='" + String(inclinazione) + "'><br>";
  html += "Orientamento: <select name='orient'>";
  html += "<option value='Est' " + String(orientamento == "Est" ? "selected" : "") + ">Est</option>";
  html += "<option value='Sud' " + String(orientamento == "Sud" ? "selected" : "") + ">Sud</option>";
  html += "<option value='Ovest' " + String(orientamento == "Ovest" ? "selected" : "") + ">Ovest</option>";
  html += "</select><br>";
  html += "<button type='submit'>Salva Configurazione</button>";
  html += "</form><hr>";

  // Pulsante Presentazione Prodotto
  html += "<a href='/about'><button class='info'>Info Prodotto</button></a><br>";
  html += "<a href='/resetwifi' onclick=\"return confirm('Reset Wi-Fi?');\"><button class='danger'>Reset Wi-Fi</button></a>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// --- PAGINA PRESENTAZIONE PRODOTTO ---
void handleAbout() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>SOLAR32 - Presentazione</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; background: #eceff1; margin:0; padding:20px; }";
  html += ".card { background: white; padding: 30px; border-radius: 12px; max-width: 450px; margin: auto; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #00897b; margin-bottom: 5px; }";
  html += ".author { font-size: 16px; color: #555; margin-bottom: 20px; font-weight: bold; }";
  html += "p { color: #444; font-size: 15px; line-height: 1.6; text-align: left; }";
  html += "button { background: #00897b; color: white; border: none; padding: 10px 20px; border-radius: 6px; font-size: 16px; cursor: pointer; margin-top: 20px; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h1>SOLAR32</h1>";
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
    if (server.hasArg("orient")) orientamento = server.arg("orient");

    preferences.begin("solar_cfg", false);
    preferences.putString("lat", latitude);
    preferences.putString("lon", longitude);
    preferences.putFloat("kwp", kWpImpianto);
    preferences.putInt("incl", inclinazione);
    preferences.putString("orient", orientamento);
    preferences.end();

    aggiornaDatiSolari();
    aggiornaDisplay();
  }
  server.sendHeader("Location", "/");
  server.send(333, "text/plain", "");
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
  orientamento = preferences.getString("orient", "Est");
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

  // Polling automatico ogni 5 minuti
  unsigned long corrente = millis();
  if (corrente - ultimoAggiornamentoMeteo >= intervalloMeteo) {
    ultimoAggiornamentoMeteo = corrente;
    aggiornaDatiSolari();
    aggiornaDisplay();
  }
}