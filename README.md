# Commento

Primo prototipo JUCE di uno strumento touch per Raspberry Pi 5 basato su
memorie MIDI e audio libere, senza BPM e senza griglia.

Il nome **Commento** e' provvisorio.

## Stato del prototipo

- quattro memorie MIDI indipendenti associate ai canali 1-4;
- quattro motori sonori interni polifonici, uno per memoria MIDI;
- MIDI thru opzionale verso un eventuale strumento esterno;
- registrazione e riproduzione continua degli eventi MIDI;
- una memoria audio stereo circolare fino a 120 secondi per il sax;
- prima registrazione audio libera e overdub successivo con decadimento;
- supporto ai 12 ingressi e 10 uscite USB della Tascam Model 12;
- pagina CONNESSIONI con selezione audio e MIDI JUCE;
- interfaccia scalabile a cinque orbite, senza waveform e browser di sample;
- build macOS e Raspberry Pi OS 64 bit dallo stesso codice.

Questa versione non salva ancora le memorie su disco. I quattro suoni interni
sono prime voci ambient provvisorie; non contengono ancora pannelli di sintesi,
delay, riverbero, drift o mutazioni generative.

## Flusso del sistema autonomo

```text
Keystep Pro -> ingresso MIDI di Commento -> quattro voci interne
Sax/microfono -> canale 3 Model 12 -> memoria RESPIRO
Commento stereo out USB 1-2 -> canali 1-2 Model 12 -> casse / impianto
```

In CONNESSIONI:

1. scegliere Tascam Model 12 come dispositivo audio;
2. abilitare soltanto l'ingresso USB sul quale arriva il sax, per esempio 3;
3. abilitare le uscite USB 1 e 2 per il ritorno stereo di Commento;
4. scegliere il Keystep Pro come ingresso MIDI;
5. lasciare MIDI Output su `none`, salvo uso volontario di hardware esterno.

Sulla Model 12, impostare i canali 1 e 2 su `PC` per ricevere il ritorno di
Commento e lasciare il canale del sax su `LIVE`. Nel menu USB AUDIO usare
`MULTI INPUT`, affinche' gli ingressi individuali arrivino al Raspberry.

Non usare per il sax uno dei medesimi canali impostati su `PC` per il ritorno:
si rischierebbe un routing ambiguo o un feedback. Il prototipo non monitora
direttamente il sax; il segnale dal vivo viene ascoltato dal mixer Model 12.

La Model 12 lavora via USB 2.0 fino a 24 bit / 48 kHz. Per il primo test usare
48 kHz e un buffer di 128 o 256 campioni. E' alimentata dal proprio adattatore,
quindi la connessione USB al Raspberry trasporta dati senza gravare in modo
significativo sull'alimentazione del Pi.

La Model 12 dispone di ingressi microfonici XLR e phantom power. Attivare la
phantom soltanto se il microfono del sax la richiede e con fader/monitor abbassati
durante il collegamento.

## Uso

- toccare un'orbita per selezionarla;
- premere **SEMINA** per iniziare a registrare;
- premere **CHIUDI LA MEMORIA** per stabilire la durata libera del ciclo;
- premere **DIMENTICA** per svuotare la memoria selezionata;
- sulla memoria RESPIRO, registrare nuovamente per fare overdub;
- **MEMORIA DEL RESPIRO** decide quanto materiale precedente sopravvive a ogni
  overdub (`1.000` conserva tutto, valori inferiori dissolvono il passato).

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

Nella pagina CONNESSIONI scegliere Model 12, 48000 Hz, buffer 256, ingresso sax
e uscite 1-2. Scegliere Keystep Pro tra gli ingressi MIDI e lasciare MIDI Output
su `none`.

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

Al riavvio Xorg viene avviato direttamente su `tty1` e Commento occupa il
touchscreen a 1920x1200. Da SSH:

```sh
sudo systemctl start commento-kiosk.service
sudo systemctl stop commento-kiosk.service
sudo systemctl restart commento-kiosk.service
systemctl status commento-kiosk.service
journalctl -u commento-kiosk.service -f
```

Per rimuovere il kiosk e ripristinare il login testuale su `tty1`:

```sh
sudo ./deploy/raspberry-pi/remove-kiosk-service.sh
sudo systemctl reboot
```

## Architettura

- `EcosystemEngine`: callback audio realtime e coordinamento delle memorie;
- `MidiMemory`: eventi MIDI con posizione in campioni e durata indipendente;
- `AudioMemory`: buffer circolare stereo con overdub e decadimento;
- `MainComponent`: interfaccia touch, routing device e animazione.

I callback MIDI scrivono in una FIFO preallocata. Le memorie vengono modificate
dal thread audio; l'uscita dei loop MIDI viene drenata ogni 2 ms per non legare
il timing alla frequenza di aggiornamento della grafica.
