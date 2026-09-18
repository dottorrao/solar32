# Accuratezza previsioni SOLAR32

Confronto tra la produzione stimata e quella reale registrata dall'impianto, giorno per giorno.

## Colonne

- **Previsto**: il valore effettivamente mostrato dal dispositivo quel giorno (registro storico).
  È quello letto in giornata, non quello del giorno prima: le previsioni si aggiornano di continuo
  e quella più recente è la più affidabile.
- **Ricalcolato**: cosa avrebbe stimato il modello *attuale* usando l'irradianza **reale**
  (archivio di rianalisi Open-Meteo, che incorpora le osservazioni satellitari delle nuvole).
  Serve a isolare la qualità del modello dall'errore della previsione meteo: sono due problemi
  distinti e si risolvono in modi diversi.
- **Rilevato**: produzione reale letta dall'app dell'impianto.

*% Differenza calcolata come `|stima - rilevato| / rilevato * 100`.*

## Storico

| Giorno | Previsto (kWh) | Ricalcolato (kWh) | Rilevato (kWh) | % Diff. previsto | % Diff. ricalcolato |
|---|---|---|---|---|---|
| 2026-09-14 | 31.8 | 30.75 | 30.97 | 2.7% | 0.7% |
| 2026-09-15 | 35.7 | 32.57 | 33.30 | 7.2% | 2.2% |
| 2026-09-16 | 25.9 | 30.53 | 28.91 | 10.4% | 5.6% |
| 2026-09-17 | 20.2 | 16.21 | 11.69 | 72.8% | 38.7% |

## Note

**17/09/2026 — giornata anomala.** Unica giornata perturbata (temporali) del campione.
Per questo giorno previsione e rianalisi coincidono quasi perfettamente (3554 vs 3559 Wh/m²):
il dato di irradianza era corretto, quindi lo scarto non viene dal meteo ma dal fatto che con
luce molto scarsa il rendimento reale dell'impianto crolla più di quanto la curva del PR preveda
(0.55 reale contro lo 0.80-0.86 delle giornate serene). La curva non è stata irripidita perché
con un solo campione si rischierebbe di adattare il modello al rumore. Da rivedere quando ci
saranno più giornate coperte.

**16/09/2026 — errore di previsione meteo.** L'irradianza prevista era del 24% più bassa di
quella poi realmente misurata (4.56 contro 6.02 kWh/m²): è un limite della previsione, non del
modello di conversione.

**Ricalibrazione del PR (17/09/2026).** I tre giorni sereni hanno mostrato un rendimento reale
di 0.855 / 0.864 / 0.800, contro lo 0.947 fisso che il codice usava. Il Performance Ratio è stato
quindi abbassato e reso variabile in funzione dell'irradianza oraria (vedi `performanceRatio()`
in `solar32.ino`). I valori nella colonna "Ricalcolato" usano questa nuova curva.
