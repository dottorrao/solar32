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

// Server NTP e fuso orario italiano (passaggio automatico ora legale/solare)
const char* ntpServer = "pool.ntp.org";
const char* tzItalia = "CET-1CEST,M3.5.0,M10.5.0/3";

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
int codiceMeteo[7];          // Codice condizione meteo WMO di Open-Meteo per ogni giorno

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

// --- MAPPA IL CODICE METEO WMO DI OPEN-METEO IN UNA CATEGORIA PER LE ICONE ---
// 0=Sole, 1=Sole velato, 2=Nuvoloso, 3=Nebbia, 4=Pioggia, 5=Neve, 6=Temporale
int categoriaMeteo(int codice) {
  if (codice == 0) return 0;
  if (codice == 1 || codice == 2) return 1;
  if (codice == 3) return 2;
  if (codice == 45 || codice == 48) return 3;
  if (codice == 71 || codice == 73 || codice == 75 || codice == 77 || codice == 85 || codice == 86) return 5;
  if (codice == 95 || codice == 96 || codice == 99) return 6;
  return 4; // drizzle/pioggia/rovesci: tutti i restanti codici di precipitazione
}

// --- DISEGNA UNA GRIGLIA DI PIXEL (13x11) SUL DISPLAY LCD, PIXEL PER PIXEL ---
// Ogni cella della griglia e' una lettera: S=sole, C=grigio chiaro, G=grigio,
// A=azzurro, N=nero, .=trasparente (pixel non disegnato, resta lo sfondo).
void disegnaGrigliaPixel(const char* const righe[11], int x, int y) {
  for (int r = 0; r < 11; r++) {
    for (int c = 0; c < 13; c++) {
      char ch = righe[r][c];
      uint16_t colore;
      switch (ch) {
        case 'S': colore = tft.color565(255, 204, 77);  break;
        case 'C': colore = tft.color565(204, 214, 221); break;
        case 'G': colore = tft.color565(153, 170, 181); break;
        case 'A': colore = tft.color565(85, 172, 238);  break;
        case 'N': colore = ST7735_BLACK; break;
        default: continue; // '.' = non disegnare, lascia lo sfondo
      }
      tft.drawPixel(x + c, y + r, colore);
    }
  }
}

// --- ICONE METEO PER LCD: griglie 13x11 riprodotte pixel per pixel dalle icone originali ---
void disegnaIconaMeteoLCD(int categoria, int x, int y) {
  static const char* const griglioSole[11] = {
    ".S....S....S.",
    "..S.......S..",
    ".....SSS.....",
    "....SSSSS....",
    "...SSSSSSS...",
    "SS.SSSSSSS.SS",
    "...SSSSSSS...",
    "....SSSSS....",
    ".....SSS.....",
    "..S.......S..",
    ".S....S....S."
  };
  static const char* const griglioSoleVelato[11] = {
    "........S....",
    ".....S.....S.",
    ".......SSS...",
    "..GGG.SSSSS..",
    ".GGGGGSSSSS.S",
    "GGGGGGGSSSS..",
    "GGGGGGGGGG...",
    "GGGGGGGGGGGS.",
    "GGGGGGGGGGG..",
    ".GGGGGGGGG...",
    "............."
  };
  static const char* const griglioNuvoloso[11] = {
    ".............",
    "....GGGG.....",
    "...GGGGGG....",
    "..GGGGGGG....",
    ".GGGGGGGGGG..",
    ".GGGGGGGGGGG.",
    ".GGGGGGGGGGG.",
    ".GGGGGGGGGGG.",
    "..GGGGGGGGG..",
    ".............",
    "............."
  };
  static const char* const griglioNebbia[11] = {
    ".............",
    "....GGGG.....",
    ".............",
    "..GGGGGGG....",
    ".............",
    ".GGGGGGGGGGG.",
    ".............",
    ".GGGGGGGGGGG.",
    ".............",
    "....GGGGGG...",
    "............."
  };
  static const char* const griglioPioggia[11] = {
    ".............",
    ".....GGG.....",
    "...GGGGGG....",
    "..GGGGGGGGG..",
    "..GGGGGGGGGG.",
    "..GGGGGGGGGG.",
    "...GGGGGGGG..",
    "....A...A....",
    "..A.A.A.A.A..",
    "..A...A...A..",
    "............."
  };
  static const char* const griglioNeve[11] = { // nessuna icona di riferimento fornita: riuso la nuvola con fiocchi al posto della pioggia
    ".............",
    "....GGGG.....",
    "...GGGGGG....",
    "..GGGGGGG....",
    ".GGGGGGGGGG..",
    ".GGGGGGGGGGG.",
    ".GGGGGGGGGGG.",
    ".GGGGGGGGGGG.",
    "..GGGGGGGGG..",
    ".A...A...A...",
    "............."
  };
  static const char* const griglioTemporale[11] = {
    ".............",
    ".....GGG.....",
    "...GGGGGG....",
    "..GGGGGSGGG..",
    ".GGGGGSSGGGG.",
    ".GGGGSSGGGGG.",
    "...GGSSSGGG..",
    ".A....SS.A...",
    ".A.A.SS..A.A.",
    "...A.S.....A.",
    "............."
  };

  switch (categoria) {
    case 0: disegnaGrigliaPixel(griglioSole, x, y); break;
    case 1: disegnaGrigliaPixel(griglioSoleVelato, x, y); break;
    case 2: disegnaGrigliaPixel(griglioNuvoloso, x, y); break;
    case 3: disegnaGrigliaPixel(griglioNebbia, x, y); break;
    case 5: disegnaGrigliaPixel(griglioNeve, x, y); break;
    case 6: disegnaGrigliaPixel(griglioTemporale, x, y); break;
    default: disegnaGrigliaPixel(griglioPioggia, x, y); break;
  }
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
                 "&daily=weather_code" +
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
          codiceMeteo[i] = doc["daily"]["weather_code"][i];
        }
        Serial.println("Dati meteo e stime produzione aggiornati!");
      }
    }
    http.end();
  }
}

// --- CALLBACK PER CONFIGURAZIONE WIFI ---
void configModeCallback(WiFiManager *myWiFiManager) {
  tft.fillScreen(ST7735_WHITE);

  // Logo: la "o" di "Solar32" sostituita da un'icona sole (stesso logo delle altre schermate)
  tft.setCursor(4, 6);
  tft.setTextColor(ST7735_BLACK);
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
  tft.setTextColor(ST7735_BLACK);
  tft.setTextSize(2);
  tft.println("lar32");

  tft.drawFastHLine(0, 32, 160, tft.color565(224, 224, 224));

  tft.setTextSize(1);
  tft.setCursor(4, 42);
  tft.setTextColor(tft.color565(33, 33, 33));
  tft.println("Configurazione Wi-Fi");

  tft.setCursor(4, 58);
  tft.setTextColor(tft.color565(33, 33, 33));
  tft.println("Connettiti alla rete:");

  tft.setCursor(4, 72);
  tft.setTextColor(tft.color565(1, 87, 155));
  tft.println("Solar32-AP");
}

// --- STILE E LOGO PERSONALIZZATI PER LE PAGINE WEB DI WIFIMANAGER ---
// WiFiManager non permette di ridisegnare il layout delle sue pagine, ma
// setCustomHeadElement() consente di iniettare CSS/JS per adattarne i colori
// e aggiungere il nostro logo, senza modificare la libreria.
String wifiManagerCustomHtml() {
  return "<style>"
         "body { background:#eceff1 !important; font-family:Arial,sans-serif !important; }"
         "button, input[type=submit] { background:#00897b !important; color:white !important; border:none !important; border-radius:6px !important; }"
         "h1, h2 { color:#00897b !important; }"
         "a { color:#00897b !important; }"
         "</style>"
         "<script>"
         "document.addEventListener('DOMContentLoaded', function() {"
         "var el = document.createElement('div');"
         "el.style.textAlign = 'center';"
         "el.style.padding = '10px 0';"
         "el.innerHTML = \"<svg width='132' height='32' viewBox='0 0 132 32'>"
         "<text x='2' y='25' font-size='26' font-weight='bold' fill='#00897b'>S</text>"
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
         "<text x='44' y='25' font-size='26' font-weight='bold' fill='#00897b'>lar32</text>"
         "</svg>\";"
         "document.body.insertBefore(el, document.body.firstChild);"
         "});"
         "</script>";
}

// --- RENDERING SCHERMO TFT CON GRIGLIA (stile card, coerente con la dashboard web) ---
void aggiornaDisplay() {
  uint16_t colBg        = ST7735_WHITE;                // sfondo bianco
  uint16_t colTopBarTx  = tft.color565(33, 33, 33);    // ora/IP: testo scuro leggibile sul bianco
  uint16_t colDivider   = tft.color565(224, 224, 224); // linea separatrice, grigio chiaro
  uint16_t colNowBg     = tft.color565(255, 241, 118); // cerchio NOW: giallo chiaro
  uint16_t colNowBorder = tft.color565(184, 134, 11);  // bordo NOW: giallo scuro
  uint16_t colOggiBg    = tft.color565(179, 229, 252); // cerchio OGGI: celeste chiaro
  uint16_t colOggiBorder= tft.color565(1, 87, 155);    // bordo OGGI: azzurro scuro
  uint16_t colDayLabelTx= tft.color565(33, 33, 33);    // nomi giorno: scuro, leggibile sul bianco
  uint16_t colDayValueTx= tft.color565(0, 137, 123);   // valori kWh: teal acceso, leggibile sul bianco

  tft.fillScreen(colBg);

  // 1. RIGA SUPERIORE: ORA E IP
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

  tft.drawFastHLine(0, 14, 160, colDivider);

  // 2. DATI GIORNO CORRENTE (due card rettangolari a bordi stondati: istantaneo e totale oggi)
  int cardX1 = 2, cardX2 = 82, cardY = 16, cardW = 76, cardH = 48, cardR = 8;
  tft.fillRoundRect(cardX1, cardY, cardW, cardH, cardR, colNowBg);
  tft.drawRoundRect(cardX1, cardY, cardW, cardH, cardR, colNowBorder);
  tft.fillRoundRect(cardX2, cardY, cardW, cardH, cardR, colOggiBg);
  tft.drawRoundRect(cardX2, cardY, cardW, cardH, cardR, colOggiBorder);

  int cxNow = cardX1 + cardW / 2;
  int cxOggi = cardX2 + cardW / 2;

  // --- CARD SINISTRA: ISTANTANEO ---
  // Etichetta in alto, dentro il riquadro, in grassetto (nero)
  String strNowLabel = "Irraggiam.";
  int xNowLabel = cxNow - (int)(strNowLabel.length() * 3);
  tft.setCursor(xNowLabel, cardY + 4);
  tft.setTextColor(ST7735_BLACK);
  tft.setTextSize(1);
  tft.print(strNowLabel);
  tft.setCursor(xNowLabel + 1, cardY + 4);
  tft.print(strNowLabel);

  String strNowVal = String((int)radiazioneIstantanea);
  int sizeNowVal = (strNowVal.length() <= 6) ? 2 : 1;
  int xNowVal = cxNow - (int)(strNowVal.length() * (sizeNowVal == 2 ? 6 : 3));
  tft.setCursor(xNowVal, cardY + 16);
  tft.setTextColor(colNowBorder);
  tft.setTextSize(sizeNowVal);
  tft.print(strNowVal);

  String unitNow = "W/m2";
  tft.setCursor(cxNow - (int)(unitNow.length() * 3), cardY + 34);
  tft.setTextColor(colNowBorder);
  tft.setTextSize(1);
  tft.print(unitNow);

  // --- CARD DESTRA: TOTALE OGGI ---
  // Etichetta in alto, dentro il riquadro, in grassetto (nero)
  String strOggiLabel = "Prod. oggi";
  int xOggiLabel = cxOggi - (int)(strOggiLabel.length() * 3);
  tft.setCursor(xOggiLabel, cardY + 4);
  tft.setTextColor(ST7735_BLACK);
  tft.setTextSize(1);
  tft.print(strOggiLabel);
  tft.setCursor(xOggiLabel + 1, cardY + 4);
  tft.print(strOggiLabel);

  String strOggiVal = String(previsioni7Giorni[0], 1);
  int sizeOggiVal = (strOggiVal.length() <= 6) ? 2 : 1;
  int xOggiVal = cxOggi - (int)(strOggiVal.length() * (sizeOggiVal == 2 ? 6 : 3));
  tft.setCursor(xOggiVal, cardY + 16);
  tft.setTextColor(colOggiBorder);
  tft.setTextSize(sizeOggiVal);
  tft.print(strOggiVal);

  String unitOggi = "kWh";
  tft.setCursor(cxOggi - (int)(unitOggi.length() * 3), cardY + 34);
  tft.setTextColor(colOggiBorder);
  tft.setTextSize(1);
  tft.print(unitOggi);

  // 3. PREVISIONI PROSSIMI 6 GIORNI (GRIGLIA 2x3, accento circolare + testo, righe ben distanziate)
  int startY = 70;
  int colWidth = 53;
  int rowHeight = 27;

  for (int i = 1; i <= 6; i++) {
    int idx = i - 1;
    int col = idx % 3;
    int row = idx / 3;

    int x = col * colWidth + 4;
    int y = startY + (row * rowHeight);

    disegnaIconaMeteoLCD(categoriaMeteo(codiceMeteo[i]), x, y);

    // Nome giorno in grassetto (font base senza bold: si simula ridisegnando con 1px di scarto)
    tft.setCursor(x + 16, y + 2);
    tft.setTextColor(colDayLabelTx);
    tft.setTextSize(1);
    tft.print(nomiGiorni[i]);
    tft.setCursor(x + 17, y + 2);
    tft.print(nomiGiorni[i]);

    tft.setCursor(x + 2, y + 14);
    tft.setTextColor(colDayValueTx);
    tft.setTextSize(1);
    tft.print(previsioni7Giorni[i], 1);
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
  tft.drawFastHLine(0, 32, 160, ST7735_BLUE);

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

// --- ICONE SVG RIUTILIZZABILI PER LO STILE "BADGE CIRCOLARI" (app di monitoraggio) ---
String sunIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<circle cx='12' cy='12' r='5' fill='#FFCC4D'/>"
         "<g stroke='#FFCC4D' stroke-width='1.5' stroke-linecap='round'>"
         "<line x1='12' y1='1' x2='12' y2='3'/><line x1='12' y1='21' x2='12' y2='23'/>"
         "<line x1='1' y1='12' x2='3' y2='12'/><line x1='21' y1='12' x2='23' y2='12'/>"
         "<line x1='4.2' y1='4.2' x2='5.6' y2='5.6'/><line x1='18.4' y1='18.4' x2='19.8' y2='19.8'/>"
         "<line x1='4.2' y1='19.8' x2='5.6' y2='18.4'/><line x1='18.4' y1='5.6' x2='19.8' y2='4.2'/>"
         "</g></svg>";
}

String cloudSunIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<circle cx='9' cy='8' r='4' fill='#FFCC4D'/>"
         "<g stroke='#FFCC4D' stroke-width='1.3' stroke-linecap='round'>"
         "<line x1='9' y1='1' x2='9' y2='2.5'/><line x1='2' y1='8' x2='3.5' y2='8'/>"
         "<line x1='3.5' y1='3.5' x2='4.5' y2='4.5'/><line x1='14.5' y1='3.5' x2='13.5' y2='4.5'/>"
         "</g>"
         "<rect x='6' y='13' width='16' height='7' rx='3.5' fill='#CCD6DD'/>"
         "<circle cx='10' cy='12' r='3.5' fill='#CCD6DD'/>"
         "<circle cx='15' cy='10' r='4.5' fill='#CCD6DD'/>"
         "<circle cx='19' cy='12' r='3.5' fill='#CCD6DD'/>"
         "</svg>";
}

String cloudIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<rect x='3' y='12' width='18' height='8' rx='4' fill='#99AAB5'/>"
         "<circle cx='8' cy='11' r='4' fill='#99AAB5'/>"
         "<circle cx='14' cy='8' r='5.5' fill='#99AAB5'/>"
         "<circle cx='19' cy='11' r='4' fill='#99AAB5'/>"
         "</svg>";
}

String fogIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<g stroke='#CCD6DD' stroke-width='2' stroke-linecap='round'>"
         "<line x1='3' y1='7' x2='21' y2='7'/>"
         "<line x1='3' y1='12' x2='21' y2='12'/>"
         "<line x1='3' y1='17' x2='21' y2='17'/>"
         "</g></svg>";
}

String rainIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<rect x='3' y='6' width='18' height='7' rx='3.5' fill='#99AAB5'/>"
         "<circle cx='8' cy='5' r='3.5' fill='#99AAB5'/>"
         "<circle cx='14' cy='3' r='5' fill='#99AAB5'/>"
         "<circle cx='19' cy='5' r='3.5' fill='#99AAB5'/>"
         "<g stroke='#55ACEE' stroke-width='2' stroke-linecap='round'>"
         "<line x1='8' y1='16' x2='7' y2='20'/>"
         "<line x1='13' y1='16' x2='12' y2='20'/>"
         "<line x1='18' y1='16' x2='17' y2='20'/>"
         "</g></svg>";
}

String snowIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<rect x='3' y='6' width='18' height='7' rx='3.5' fill='#99AAB5'/>"
         "<circle cx='8' cy='5' r='3.5' fill='#99AAB5'/>"
         "<circle cx='14' cy='3' r='5' fill='#99AAB5'/>"
         "<circle cx='19' cy='5' r='3.5' fill='#99AAB5'/>"
         "<g fill='#55ACEE'>"
         "<circle cx='8' cy='18' r='1.4'/><circle cx='13' cy='19' r='1.4'/><circle cx='18' cy='18' r='1.4'/>"
         "</g></svg>";
}

String stormIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<rect x='3' y='4' width='18' height='7' rx='3.5' fill='#99AAB5'/>"
         "<circle cx='8' cy='3' r='3.5' fill='#99AAB5'/>"
         "<circle cx='14' cy='1.5' r='5' fill='#99AAB5'/>"
         "<circle cx='19' cy='3' r='3.5' fill='#99AAB5'/>"
         "<polygon points='12,12 7,19 10,19 8,23 14,16 11,16' fill='#FFCC4D'/>"
         "<circle cx='18' cy='20' r='2' fill='#55ACEE'/>"
         "<polygon points='18,14 15,20 21,20' fill='#55ACEE'/>"
         "</svg>";
}

// --- SELEZIONA L'ICONA METEO GIUSTA IN BASE ALLA CATEGORIA (vedi categoriaMeteo) ---
String weatherIconSvg(int categoria, int size) {
  if (categoria == 0) return sunIconSvg(size);
  if (categoria == 1) return cloudSunIconSvg(size);
  if (categoria == 2) return cloudIconSvg(size);
  if (categoria == 3) return fogIconSvg(size);
  if (categoria == 5) return snowIconSvg(size);
  if (categoria == 6) return stormIconSvg(size);
  return rainIconSvg(size); // categoria 4
}

String chartIconSvg(int size) {
  return "<svg width='" + String(size) + "' height='" + String(size) + "' viewBox='0 0 24 24'>"
         "<rect x='4' y='12' width='4' height='9' rx='1' fill='#1976d2'/>"
         "<rect x='10' y='7' width='4' height='14' rx='1' fill='#1976d2'/>"
         "<rect x='16' y='3' width='4' height='18' rx='1' fill='#1976d2'/>"
         "</svg>";
}

String checkIconSvg() {
  return "<svg width='16' height='16' viewBox='0 0 24 24'><path d='M5 13l4 4L19 7' stroke='#2e7d32' stroke-width='3' fill='none' stroke-linecap='round' stroke-linejoin='round'/></svg>";
}

String clockIconSvg() {
  return "<svg width='16' height='16' viewBox='0 0 24 24'><circle cx='12' cy='12' r='9' stroke='#666' stroke-width='2' fill='none'/><path d='M12 7v5l3 3' stroke='#666' stroke-width='2' fill='none' stroke-linecap='round'/></svg>";
}

String infoIconSvg() {
  return "<svg width='18' height='18' viewBox='0 0 24 24'><circle cx='12' cy='12' r='10' fill='none' stroke='#37474f' stroke-width='2'/><rect x='11' y='10' width='2' height='7' fill='#37474f'/><rect x='11' y='6' width='2' height='2' fill='#37474f'/></svg>";
}

String wifiIconSvg() {
  return "<svg width='18' height='18' viewBox='0 0 24 24'><path d='M2 8.5a15 15 0 0 1 20 0' stroke='#d32f2f' stroke-width='2' fill='none' stroke-linecap='round'/><path d='M5.5 12.5a10 10 0 0 1 13 0' stroke='#d32f2f' stroke-width='2' fill='none' stroke-linecap='round'/><path d='M9 16.3a5 5 0 0 1 6 0' stroke='#d32f2f' stroke-width='2' fill='none' stroke-linecap='round'/><circle cx='12' cy='19.5' r='1.3' fill='#d32f2f'/></svg>";
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
  html += ".status-pill { display:flex; align-items:center; gap:6px; background:#e8f5e9; color:#2e7d32; font-size:13px; font-weight:bold; padding:6px 12px; border-radius:999px; width:fit-content; margin-bottom:15px; }";
  html += ".flow-row { display:flex; justify-content:center; gap:24px; margin-bottom:15px; }";
  html += ".flow-col { display:flex; flex-direction:column; align-items:center; gap:8px; }";
  html += ".flow-circle { width:88px; height:88px; border-radius:50%; display:flex; flex-direction:column; align-items:center; justify-content:center; }";
  html += ".flow-circle.amber { background:#fff3e0; }";
  html += ".flow-circle.blue { background:#e3f2fd; }";
  html += ".flow-val { font-size:18px; font-weight:bold; margin-top:2px; }";
  html += ".flow-label { font-size:12px; color:#666; }";
  html += ".day-grid-app { display:grid; grid-template-columns: repeat(3, 1fr); gap:10px; margin-bottom:15px; }";
  html += ".day-item { background:white; border-radius:8px; padding:10px 6px; text-align:center; box-shadow: 0 2px 6px rgba(0,0,0,0.06); }";
  html += ".day-icon { width:34px; height:34px; border-radius:50%; background:#fff3e0; display:flex; align-items:center; justify-content:center; margin:0 auto 6px; }";
  html += ".update-row { display:flex; align-items:center; gap:8px; background:white; border-radius:8px; padding:10px 12px; box-shadow: 0 2px 6px rgba(0,0,0,0.06); }";
  html += "label { display:block; font-size: 13px; color:#555; margin-top:10px; }";
  html += "input[type=text], select { width: 100%; padding: 8px; margin-top:4px; border: 1px solid #ccc; border-radius: 6px; }";
  html += "button.submit { background: #00897b; color: white; border: none; padding: 10px 20px; border-radius: 6px; font-size: 16px; cursor: pointer; margin-top: 16px; width:100%; }";
  html += ".action-row { display:flex; align-items:center; gap:10px; background:white; border-radius:8px; padding:12px; box-shadow: 0 2px 6px rgba(0,0,0,0.06); margin-bottom:10px; color:#333; font-size:14px; font-weight:bold; }";
  html += ".action-row.danger { color:#d32f2f; }";
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

  // SEZIONE 1: MONITORAGGIO (dati istantanei e previsioni, stile badge circolari)
  html += "<section id='monitor' class='section active'>";
  html += "<h2>Monitor & Previsioni</h2>";
  html += "<div class='status-pill'>" + checkIconSvg() + "Dati aggiornati</div>";

  html += "<div class='flow-row'>";
  html += "<div class='flow-col'><div class='flow-circle amber'>" + sunIconSvg(22) + "<div class='flow-val' style='color:#e65100;'>" + String((int)radiazioneIstantanea) + "</div></div><div class='flow-label'>Istantaneo, W/m&sup2;</div></div>";
  html += "<div class='flow-col'><div class='flow-circle blue'>" + chartIconSvg(22) + "<div class='flow-val' style='color:#1565c0;'>" + String(previsioni7Giorni[0], 1) + "</div></div><div class='flow-label'>Previsto oggi, kWh</div></div>";
  html += "</div>";

  html += "<p style='font-size:13px; font-weight:bold; color:#666; margin:0 0 10px;'>Prossimi giorni</p>";
  html += "<div class='day-grid-app'>";
  for (int i = 1; i <= 6; i++) {
    html += "<div class='day-item'><div class='day-icon'>" + weatherIconSvg(categoriaMeteo(codiceMeteo[i]), 16) + "</div><p style='font-size:12px; font-weight:bold; margin:0;'>" + nomiGiorni[i] + "</p><p style='font-size:12px; color:#666; margin:0;'>" + String(previsioni7Giorni[i], 1) + " kWh</p></div>";
  }
  html += "</div>";

  html += "<div class='update-row'>" + clockIconSvg() + "<div><p style='font-size:11px; color:#999; margin:0;'>Ultimo aggiornamento</p><p style='font-size:13px; font-weight:bold; margin:0;'>Oggi, " + getOraAttuale() + "</p></div></div>";
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
  html += "<a class='action-row' href='/about'>" + infoIconSvg() + "<span>Info Prodotto</span></a>";
  html += "<a class='action-row danger' href='/resetwifi' onclick=\"return confirm('Reset Wi-Fi?');\">" + wifiIconSvg() + "<span>Reset Wi-Fi</span></a>";
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
  html += "h1 { color: #00897b; margin-bottom: 20px; display: flex; align-items: center; justify-content: center; }";
  html += "p { color: #444; font-size: 15px; line-height: 1.6; text-align: left; }";
  html += "button { background: #00897b; color: white; border: none; padding: 10px 20px; border-radius: 6px; font-size: 16px; cursor: pointer; margin-top: 20px; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h1>" + logoSvgHtml() + "</h1>";
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
  wm.setTitle("Solar32");
  String wmHead = wifiManagerCustomHtml();
  wm.setCustomHeadElement(wmHead.c_str());

  if (digitalRead(PIN_RESET_WIFI) == LOW) {
    wm.resetSettings();
    delay(1000);
  }

  bool res = wm.autoConnect("Solar32-AP");

  if (!res) {
    ESP.restart();
  }

  // Sincronizzazione Orologio via NTP (con cambio automatico ora legale/solare)
  configTzTime(tzItalia, ntpServer);

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