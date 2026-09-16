**Titolo suggerito:** SOLAR32 — monitor e previsione produzione fotovoltaica con ESP32 (open source, in fase di test)

---

Ciao a tutti,

vi presento un piccolo progetto che ho realizzato negli ultimi giorni: **SOLAR32**, un dispositivo basato su ESP32 che mostra su un display LCD e su una pagina web l'irraggiamento istantaneo e la stima di produzione del proprio impianto fotovoltaico, con previsioni per i prossimi 7 giorni.

⚠️ **È ancora in fase di test**: lo sto validando da un paio di giorni sul mio impianto, i primi risultati sono incoraggianti (vedi sotto) ma non ho ancora dati su condizioni meteo diverse (pioggia, nuvolosità variabile, stagioni diverse). Lo condivido comunque perché penso possa interessare, e più feedback/test da configurazioni diverse ricevo, meglio riesco a validarlo.

## Cosa fa

- Mostra l'irraggiamento solare istantaneo (W/m²) sul piano reale del pannello
- Stima la produzione di oggi e dei prossimi 6 giorni (kWh)
- Mostra un'icona meteo per ogni giorno (sole, nuvoloso, pioggia, neve, temporale, nebbia)
- Dashboard web accessibile da qualsiasi dispositivo in rete locale, per configurare impianto e vedere i dati con più comodità
- Tutto configurabile via web: coordinate, potenza di picco (kWp), inclinazione e orientamento del tetto (bussola)

## Come funziona (per i curiosi)

Non legge dati dall'inverter — è una stima basata sui dati meteo di [Open-Meteo](https://open-meteo.com), un servizio gratuito e open. La parte interessante è che non usa la semplice radiazione orizzontale: interroga l'API specificando **inclinazione e orientamento reali del tetto** (parametri `tilt`/`azimuth`), ottenendo l'irraggiamento calcolato proprio sul piano del pannello — molto più preciso di un generico dato meteo "a occhio".

Sto confrontando le stime con la produzione reale del mio impianto: con l'orientamento configurato correttamente (bussola alla mano, non "a stima"), i primi due giorni di test hanno dato uno scarto del **2-3%** — dato preliminare, ma incoraggiante per un dispositivo da poche decine di euro di componenti. Continuerò a monitorarlo nei prossimi giorni/settimane per capire quanto regge su condizioni meteo diverse.

## Hardware

- ESP32
- Display TFT ST7735 (SPI)
- Nessun altro sensore: tutto il dato viene da Open-Meteo via WiFi

## Software

- Configurazione WiFi tramite WiFiManager (crea un access point "Solar32-AP" al primo avvio)
- Dashboard web con sezione Monitoraggio e Impostazioni
- Codice Arduino, disponibile su GitHub: **[link al repository]**
- Licenza Creative Commons BY-NC-SA: libero per uso personale, cito la fonte se lo modificate/condividete, non è consentita la vendita di dispositivi/kit senza permesso

## Foto

[allegare foto del display e della dashboard web]

---

Se qualcuno lo prova o ha suggerimenti/domande, sono qui! È un progetto in evoluzione, ogni feedback è benvenuto.
