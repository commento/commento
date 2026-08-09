# Commento

Primo prototipo JUCE di uno strumento touch per Raspberry Pi 5 basato su
memorie MIDI e audio libere, senza BPM e senza griglia.

Il nome **Commento** e' provvisorio.

## Stato del prototipo

- un basso monofonico sempre live sul MIDI 5, attivabile dal touchscreen;
- tre memorie MIDI indipendenti sui canali 2, 3 e 4;
- quattro motori sonori interni, con basso separato e tre voci ambient;
- nessun MIDI thru esterno: live e loop alimentano direttamente le quattro voci;
- registrazione e riproduzione continua degli eventi MIDI;
- dieci scenari che ri-orchestrano i loop senza cancellarli;
- pluck dilatati tramite delay asincroni e riverberi per singola voce;
- una memoria audio stereo circolare fino a 120 secondi per il sax;
- prima registrazione audio libera e overdub successivo con headroom, decadimento
  e saturazione morbida;
- dieci trattamenti sax con delay, modulazione, tremolo, tono e spazi differenti;
- profilo appliance dedicato alla Tascam Model 12, aperta realmente come
  dispositivo USB 12-in/10-out;
- router hardware a cinque bus: ambiente sui canali audio 1/2 della Model 12,
  basso sul canale audio 5 e sax sui canali audio 7/8;
- rilevamento automatico del Keystep Pro, senza MIDI thru o menu audio generici;
- interfaccia touch a quattro organismi MIDI e una fascia RESPIRO, con stati
  espliciti, meter del sax e controlli grandi;
- build macOS e Raspberry Pi OS 64 bit dallo stesso codice.

Questa versione non salva ancora i loop su disco. Salva invece l'ultimo scenario
selezionato e il livello di GRANA. I parametri sonori sono scelti per il live e
non espongono ancora un pannello di sintesi dettagliato.

## Flusso del sistema autonomo

```text
Keystep MIDI 5 -> basso live Commento -> canale AUDIO 5 Model 12
Keystep MIDI 2/3/4 -> loop e voci ambient -> canali AUDIO 1/2 Model 12
Sax -> ingressi AUDIO 7/8 -> RESPIRO + effetti -> ritorno AUDIO 7/8 Model 12
```

All'avvio Commento:

1. cerca una Model 12 che esponga almeno 12 ingressi, 10 uscite e 48 kHz;
2. apre insieme tutti i 12 ingressi e le 10 uscite con buffer 512;
3. preleva i canali fisici 7/8 per RESPIRO;
4. mappa i bus logici su audio 1/2, 5 e 7/8, azzerando ogni altra uscita;
5. cerca il Keystep Pro e abilita un solo ingresso MIDI;
6. mostra lo stato effettivo di entrambi nell'intestazione.

La pagina **CONNESSIONI** non contiene menu o checkbox piccoli. Offre soltanto
due pulsanti grandi, **RIPROVA MODEL 12** e **RIPROVA KEYSTEP PRO**, insieme a
rate, buffer, xrun e mappa dei canali. Modificare input e output separatamente
con il selettore generico JUCE poteva produrre una configurazione ALSA
intermedia non valida e portare entrambi su `none`.

Sulla Model 12 impostare:

- canali 1/2 su `PC` per le tre voci ambient;
- canale 5 su `PC` per il basso generato dal MIDI 5;
- canale stereo 7/8 su `PC` per monitor ed effetti del sax;
- `USB AUDIO: MULTI INPUT`;
- `MTR/USB SEND POINT: PRE COMP`.

Il punto `PRE COMP` permette di inviare al Raspberry l'ingresso analogico del
sax prima del ritorno PC. Partire con fader 7/8 e casse bassi: se il meter sax
non reagisce o il livello cresce da solo, abbassare immediatamente 7/8 e
ricontrollare queste due impostazioni.

Poiche' 7/8 e' su `PC`, Commento rimanda anche il sax live sul proprio bus
dedicato. A 48 kHz, 512 campioni equivalgono a 10,67 ms per periodo; il monitor
software avra' quindi una latenza maggiore del monitor diretto, ma offre piu'
margine contro gli xrun. Il buffer non modifica la saturazione o l'aliasing
generati dal DSP.

La Model 12 lavora via USB 2.0 fino a 24 bit / 48 kHz. Commento usa 48 kHz e
un buffer di 512 campioni. E' alimentata dal proprio adattatore,
quindi la connessione USB al Raspberry trasporta dati senza gravare in modo
significativo sull'alimentazione del Pi.

La Model 12 dispone di ingressi microfonici XLR e phantom power. Attivare la
phantom soltanto se il microfono del sax la richiede e con fader/monitor abbassati
durante il collegamento.

## Uso

- usare le frecce grandi in alto per scegliere uno dei dieci scenari;
- usare **GRANA** per scegliere `PULITA`, `LEGGERA`, `MEDIA` o `PIENA`; il valore
  iniziale e' PULITA e viene ricordato al riavvio;
- toccare una card per selezionare BASSO LIVE, MAREA, RADICE, SCINTILLA o RESPIRO;
- su BASSO LIVE, usare **ATTIVA BASSO** / **SPEGNI BASSO**; questa parte non
  viene mai registrata nel looper;
- premere **SEMINA** per iniziare a registrare;
- premere **CHIUDI IL CICLO** per stabilire la durata libera;
- tenere premuto **TIENI PER DISSOLVERE** per 1,1 secondi per cancellare;
- su RESPIRO, premere **NUTRI** per sovraincidere;
- scegliere **MONO DA INGRESSO 7** per un microfono singolo oppure
  **STEREO 7/8** per una sorgente stereo;
- **PERSISTENZA DEL RESPIRO** decide quanto materiale precedente sopravvive a ogni
  overdub (`1.000` conserva tutto, valori inferiori dissolvono il passato).

Il percorso PULITA e' lineare ai livelli normali. Le protezioni intervengono solo
vicino al fondo scala; GRANA reintroduce gradualmente la saturazione prevista
dallo scenario. Dopo un aggiornamento da una versione precedente, cancellare un
loop RESPIRO gia' distorto: la distorsione era memorizzata nel buffer e non puo'
essere rimossa retroattivamente.

### Se il sax diventa digitale o cresce da solo

1. cancellare RESPIRO e riavviare Commento;
2. verificare `MTR/USB SEND POINT: PRE COMP`, non `POST COMP` o `POST EQ`;
3. scegliere `GRANA: PULITA` e non attivare NUTRI;
4. regolare il preamplificatore per leggere circa -18/-12 dB sul sax;
5. controllare nella pagina CONNESSIONI che `xrun` resti a zero.

Se il problema sparisce disabilitando gli ingressi, ma ritorna appena 7/8 e'
attivo, controllare per prima cosa il feedback USB. Con 7/8 su `PC`, soltanto
`PRE COMP` garantisce che al Raspberry arrivino gli ingressi analogici invece
del ritorno proveniente dal computer. Partire sempre con il fader 7/8 basso.
Se l'ingresso resta quasi a fondo scala per circa 180 ms, Commento interrompe
automaticamente il ritorno e mostra `FEEDBACK SAX`; torna gradualmente attivo
dopo un secondo di silenzio. Questa protezione evita il picco, ma non sostituisce
la configurazione PRE COMP corretta.

## I dieci scenari

| Scenario | Carattere | Sax |
|---|---|---|
| ABISSO | sub profondo, pad scuri, vetro sommerso | eco lungo e filtrato |
| GOCCE | tre pluck dilatati e luminosi | ping-pong liquido |
| NASTRO | timbri caldi e leggermente instabili | eco modulato e saturo |
| CATTEDRALE | organi, cori e campane | grande ambiente con coda lunga |
| AURORA | pluck chiari, aria e cristallo | alone brillante e mobile |
| MAREA | onde lente, legno e rumore morbido | ritardi larghi come una risacca |
| RADICE | basso e transienti di legno | stanza calda e corta |
| ORBITA | impulsi sospesi e satelliti | ellisse stereo fuori tempo |
| POLVERE | suoni opachi e fragili | radio lontana, scura e consumata |
| VUOTO | pochi elementi con code molto lunghe | un solo eco lontanissimo |

Le note dei loop restano le stesse quando si cambia scenario: vengono suonate
di nuovo con i nuovi strumenti. Il basso MIDI 5 cambia timbro, ma resta live e
continua a uscire esclusivamente dal canale audio 5 della Model 12.

## Build macOS

Il progetto usa JUCE 8.0.13. Se esiste gia' un checkout JUCE locale:

```sh
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Debug \
  -DCOMMENTO_JUCE_SOURCE_DIR=/percorso/a/JUCE
cmake --build build-mac --parallel 4
open build-mac/Commento_artefacts/Debug/Commento.app --args --windowed
```

Senza `COMMENTO_JUCE_SOURCE_DIR`, CMake scarica automaticamente la versione
JUCE fissata dal progetto.

## Build Raspberry Pi OS 64 bit

Su Raspberry Pi OS 64 bit, Lite o Desktop, installare prima le dipendenze di
compilazione:

```sh
sudo apt update
sudo apt install -y build-essential cmake git pkg-config alsa-utils \
  libasound2-dev libjack-jackd2-dev ladspa-sdk \
  libfreetype6-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev
```

Entrare nella cartella del progetto e compilare una Release senza i test, per
ridurre tempo e memoria usati durante la build:

```sh
cd ~/commento
cmake -S . -B build-pi \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-pi --parallel 3
```

La prima configurazione scarica JUCE 8.0.13 da GitHub e puo' richiedere diversi
minuti. Se la compilazione viene terminata per memoria insufficiente, ripetere
con `--parallel 2` oppure `--parallel 1`.

Prima di avviare Commento, verificare che l'hardware USB sia visibile:

```sh
lsusb
aplay -l
arecord -l
aconnect -l
```

Impostare le quattro parti del Keystep Pro, nell'ordine, sui canali MIDI
5, 2, 3 e 4. Audio e MIDI vengono trovati automaticamente; CONNESSIONI serve
per controllarne lo stato e riprovare un'apertura fallita. In avvio manuale,
se la Model 12 viene collegata dopo Commento, riavviare l'applicazione; il kiosk
attende invece il mixer prima di lanciare il processo.

Su Raspberry Pi OS Desktop si puo' avviare manualmente con:

```sh
./build-pi/Commento_artefacts/Release/Commento --windowed
```

Su Raspberry Pi OS Lite seguire invece la sezione kiosk qui sotto.

### Raspberry Pi OS Lite: avvio kiosk senza desktop

Commento non richiede un desktop environment. Dopo la build Release, installare
il server X minimale e il servizio kiosk con:

```sh
cd ~/commento
sudo ./deploy/raspberry-pi/install-kiosk-service.sh
sudo systemctl reboot
```

Su Raspberry Pi 5 il pacchetto `gldriver-test`, installato automaticamente dallo
script, prepara Xorg per il controller grafico RP1. Senza questo pacchetto Xorg
su Raspberry Pi OS Lite puo' terminare con `Cannot run in framebuffer mode`.

Al riavvio Xorg viene avviato direttamente su `tty1` e Commento occupa il
touchscreen a 1920x1200. Da SSH:

```sh
sudo systemctl start commento-kiosk.service
sudo systemctl stop commento-kiosk.service
sudo systemctl restart commento-kiosk.service
systemctl status commento-kiosk.service
journalctl -u commento-kiosk.service -f
```

Il launcher aspetta che la Model 12 compaia in `/proc/asound/cards` prima di
avviare Commento. Questo e' necessario perche' la scansione ALSA di JUCE avviene
una sola volta per processo. Se il mixer manca o viene scollegato, systemd
aspetta che torni disponibile e riavvia automaticamente Commento.

### Diagnostica Model 12

La build Linux abilita per default `COMMENTO_ALSA_DIAGNOSTICS`, quindi i dettagli
ALSA e l'errore esatto compaiono nel journal. Se MODEL 12 resta rossa:

```sh
cat /proc/asound/cards
aplay -l
arecord -l
aplay -L
cat /proc/asound/card*/stream0
sudo fuser -v /dev/snd/*
journalctl -u commento-kiosk.service -n 200 --no-pager
```

Per disabilitare i log ALSA verbosi in una build successiva usare
`-DCOMMENTO_ALSA_DIAGNOSTICS=OFF` durante la configurazione CMake.

Per rimuovere il kiosk e ripristinare il login testuale su `tty1`:

```sh
sudo ./deploy/raspberry-pi/remove-kiosk-service.sh
sudo systemctl reboot
```

## Architettura

- `EcosystemEngine`: callback audio realtime e coordinamento delle memorie;
- `Model12AudioRouter`: adattatore tra i 12/10 canali fisici e cinque bus logici;
- `Scenarios`: dieci orchestrazioni per basso, tre layer ambient e sax;
- `SaxProcessor`: tono, delay, modulazione, riverbero e protezione del bus sax;
- `MidiMemory`: eventi MIDI con posizione in campioni e durata indipendente;
- `AudioMemory`: buffer circolare stereo con overdub e decadimento;
- `MainComponent`: interfaccia touch, routing device e animazione.

Il callback MIDI del solo Keystep scrive in una FIFO preallocata. Le memorie
vengono modificate dal thread audio; in caso di overflow viene inviato un panic
automatico e il contatore compare nell'indicatore MIDI.

Le specifiche USB e il routing MULTI INPUT sono descritti nel
[manuale ufficiale Tascam Model 12](https://www.tascam.eu/en/docs/Model12_OM_EFS_RevH3.pdf);
il percorso `PRE COMP` e i ritorni PC 1-10 sono visibili nel
[diagramma a blocchi ufficiale](https://www.tascam.eu/en/docs/Model12_SettingsPanel-V2_block-diagram.pdf).
