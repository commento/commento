# Commento

Primo prototipo JUCE di uno strumento touch per Raspberry Pi 5 basato su
memorie MIDI e audio libere, senza BPM e senza griglia.

Il nome **Commento** e' provvisorio.

## Stato del prototipo

- un basso monofonico live sul MIDI 5 in tutti i quattordici scenari, attivabile
  dal touchscreen;
- tre memorie MIDI indipendenti sui canali 2, 3 e 4;
- quattro motori sonori interni, con basso separato e tre voci ambient;
- nessun MIDI thru esterno: live e loop alimentano direttamente le quattro voci;
- registrazione e riproduzione continua degli eventi MIDI;
- quattordici scenari che ri-orchestrano i loop senza cancellarli;
- passaggio tra scenari in otto secondi, senza azzerare loop, delay o riverberi;
- pluck dilatati tramite delay asincroni e riverberi per singola voce;
- una memoria audio stereo circolare fino a 120 secondi per il sax;
- prima registrazione audio libera e overdub successivo con headroom, decadimento
  e saturazione morbida;
- quattordici trattamenti sax con delay, modulazione, tremolo, tono e spazi
  differenti;
- nello scenario COSMOS, una rilettura del loop sax con quattro testine
  asincrone e lentamente divergenti, ispirata alle memorie drifting;
- pagina CONNESSIONI hardware-generica con profili Model 12, stereo e
  personalizzato, controlli grandi e applicazione esplicita della configurazione;
- router configurabile a cinque bus logici: ambiente stereo, basso mono e sax
  stereo, con ingresso e uscite fisiche scegliibili in base all'interfaccia
  collegata;
- rilevamento automatico simultaneo del Keystep Pro, della porta MIDI standard
  della Model 12 e di un this.is.NOISE NM2 con 18 pad momentanei equivalenti,
  senza MIDI thru esterno;
- MIDI Learn persistente per associare un footswitch al comando di registrazione
  di RESPIRO, indipendentemente dalla card selezionata;
- interfaccia touch a quattro organismi MIDI e una fascia RESPIRO, con stati
  espliciti, meter del sax e controlli grandi;
- un livello indipendente e anti-click per canale I, MIDI 2/3/4 e sax, richiamato
  selezionando la relativa card;
- due gesti globali a ingresso e uscita lenti: GRANA/downsample e FUZZ;
- DERIVA opzionale e rara: una sola ombra reverse oppure +12 alla volta, sempre
  sommata al loop originale;
- cinque gesti live a costo contenuto: GELO, ECO THROW e CODA LIBERA sulla card
  selezionata, piu' ASCOLTO e DIRADA per articolare il letto ambient;
- PAUSA/PLAY indipendenti nella pagina GESTI per fermare e riprendere la sola
  memoria selezionata, comprese MAREA, RADICE, SCINTILLA e RESPIRO;
- build macOS e Raspberry Pi OS 64 bit dallo stesso codice.

Questa versione non salva ancora i loop su disco. Salva invece l'ultimo scenario
selezionato, il livello di GRANA, i cinque livelli di performance, l'ultima
configurazione audio applicata e l'eventuale associazione del pedale sax. GELO,
ECO THROW e CODA LIBERA sono momentanei; ASCOLTO e DIRADA ripartono spenti a ogni
avvio. Anche le pause non vengono salvate: tutte le memorie ripartono in PLAY.
I parametri sonori sono scelti per il live e non espongono ancora un pannello di
sintesi dettagliato.

## Flusso del sistema autonomo

Con il profilo **MODEL 12** la mappa iniziale e':

```text
Keystep MIDI 5 -> basso live -> canale AUDIO 5 Model 12
Keystep MIDI 2/3/4 -> loop e voci ambient -> canali AUDIO 1/2 Model 12
Sax -> ingressi AUDIO 7/8 -> RESPIRO + effetti -> ritorno AUDIO 7/8 Model 12
```

All'avvio Commento legge i sistemi e i dispositivi audio disponibili, ripristina
l'ultima configurazione riuscita e cerca il Keystep Pro, la porta MIDI standard
della Model 12 e un NM2. Le tre sorgenti possono restare abilitate insieme; gli
endpoint della Model 12 che nel nome contengono `DAW` o `CONTROL` vengono
esclusi. Per NM2 Commento abilita un solo endpoint e preferisce quello USB a
quello Bluetooth/BLE, se entrambi sono esposti dal sistema. Alla prima
esecuzione prova il profilo MODEL 12; se non trova un dispositivo adatto, ripiega
sul profilo stereo generico. Il riepilogo mostra il dispositivo realmente
aperto, frequenza, buffer, numero di ingressi/uscite attivi e xrun.

### Pagina CONNESSIONI

La pagina **CONNESSIONI** usa selettori grandi con frecce, adatti al touchscreen.
Permette di scegliere:

- profilo, sistema audio, dispositivo di ingresso e dispositivo di uscita;
- frequenza di campionamento e buffer;
- ingresso fisico del sax;
- uscite fisiche dei bus ambiente, canale I e sax;
- percorso diagnostico del sax e tono di prova a 997 Hz.

I profili sono punti di partenza, non vincoli:

- **MODEL 12** cerca una Tascam con almeno 12 ingressi e 10 uscite e prepara sax
  IN 7/8, ambiente OUT 1/2, canale I OUT 5 e sax OUT 7/8 a 48 kHz / 512 campioni;
- **STEREO GENERICO** usa IN 1/2 e fa convergere ambiente, canale I e sax su OUT
  1/2, con headroom automatico quando piu' bus condividono la stessa uscita;
- **PERSONALIZZATO** lascia scegliere liberamente dispositivi e canali esposti
  dall'hardware collegato.

Le frecce modificano soltanto una bozza. Il dispositivo viene riaperto e il
routing diventa effettivo solo premendo **APPLICA AUDIO**; se l'apertura fallisce,
Commento conserva la configurazione precedente. **RILEGGI DISPOSITIVI** aggiorna
l'elenco e **RILEGGI MIDI** ripete la ricerca di KeyStep Pro, Model 12 e NM2.

Nella stessa pagina, **IMPARA PEDALE SAX** mette Commento in ascolto del prossimo
Control Change MIDI con valore almeno 64. Fermare temporaneamente sequenze e
automazioni MIDI, poi premere una volta il footswitch: l'associazione mostrata a
schermo viene salvata e, da quel momento, il pedale aziona sempre il grande
pulsante di RESPIRO (`SEMINA`, `CHIUDI IL CICLO`, `NUTRI / OVERDUB` o
`FERMA NUTRI`) anche quando e' selezionata un'altra card. **RIMUOVI** cancella
soltanto questa associazione. Il messaggio associato viene consumato dal comando
RESPIRO e non viene registrato nelle memorie MIDI. Una nuova scansione MIDI o
l'apertura di CONNESSIONI interrompe un apprendimento in corso e rilascia un
pedale rimasto
premuto, ma non cancella l'associazione gia' salvata.

La pagina **GESTI** ripete i controlli di apprendimento e mostra anche un monitor
MIDI dedicato. Se una nota compare ma `CC64` non compare mai, la porta USB e il
software stanno ricevendo correttamente la tastiera e il problema e' nel pedale
o nella sua configurazione. Quando Commento vede sia il valore alto sia quello
basso mostra `CICLO 127-0 OK`; questo permette di verificare pressione, rilascio
e polarita' prima di associare il comando.

Per collegare il pedale sono previste due possibilita':

- **KeyStep Pro:** collegare un pedale momentaneo al jack Sustain prima di
  accendere la tastiera, senza tenerlo premuto durante l'accensione. Il KeyStep
  rileva la polarita' all'avvio: se il pedale non produce `CC64`, spegnere la
  tastiera, lasciare il pedale collegato e riaccenderla senza premerlo. Se necessario
  usare la MIDI Console di Arturia MIDI Control Center per verificare che venga
  trasmesso un CC momentaneo, tipicamente con valori 127/0, poi usare
  **IMPARA PEDALE SAX**. Se il firmware espone soltanto comandi MMC/trasporto,
  questi non vengono appresi per evitare conflitti con PLAY/STOP;
- **Model 12:** usare un foot controller che generi MIDI, collegarlo al MIDI IN
  DIN della Model 12 e collegare la Model 12 via USB al Raspberry Pi. Configurare
  il controller in modalita' momentanea/gate con un CC libero e valori 127/0.
  Commento ascolta la porta MIDI standard, non la seconda porta DAW/control
  (`MIDIIN2` su Windows).

Il jack `FOOTSWITCH` passivo della Model 12 comanda funzioni interne del mixer e
non espone il pedale come messaggio MIDI a Commento: non puo' quindi essere usato
direttamente per questo MIDI Learn.

### this.is.NOISE NM2: 18 pad, un gesto per scena

Commento riconosce come NM2 dedicato un endpoint MIDI il cui nome contiene
`NM2`, `THIS IS NOISE`, `THIS.IS.NOISE` o `THISISNOISE`. Il controller deve
usare la mappa di fabbrica: canale MIDI 1 e note cromatiche da 60 a 77. La
[tabella MIDI ufficiale NM2](https://thisisnoiseinc.com/en-ca/blogs/nm2-manual/midi-values)
numera i pulsanti da sinistra a destra, prima la riga superiore, poi quella
centrale e infine quella inferiore.

Le diciotto note sono equivalenti: la scena corrente sceglie un solo gesto e
qualunque pad lo tiene attivo. Questo permette di suonare senza ricordare una
mappa e senza guardare il controller montato sul sax.

| Scena | Gesto NM2 |
| --- | --- |
| ABISSO | SPROFONDA |
| GOCCE | RIMBALZO |
| NASTRO | CONSUMA |
| CATTEDRALE | NAVATA |
| AURORA | SCINTILLE |
| MAREA | RISACCA |
| RADICE | CORTECCIA |
| ORBITA | SATELLITE |
| POLVERE | FRAMMENTA |
| VUOTO | SCOMPARE |
| DRONE | TRATTIENI |
| FERRO | LAMINA |
| SCIAME | SCATTO |
| COSMOS | SOSPENDI |

Il primo `Note On` tra 60 e 77 cattura il gesto della scena. Premere altri pad
mentre il primo e' ancora giu' non riavvia l'effetto: lo mantiene semplicemente
attivo. Il rilascio di un solo pad non lo interrompe se ce ne sono altri
premuti; termina soltanto quando viene rilasciato l'ultimo. `Note Off` e
`Note On` con velocity zero sono entrambi rilasci validi, anche via BLE MIDI.

`SCINTILLE` e' il gesto piu' evidente: raccoglie brevi frammenti recenti di
sax e RESPIRO, li rilegge a velocita' doppia come piccole copie a +12
semitoni e li alterna nello stereo. Un filtro anti-alias a quattro poli,
attacco e rilascio morbidi impediscono il bordo metallico del vecchio pitch
shifter continuo; una parte della scintilla entra nel delay e nel riverbero
della scena. Il gesto esiste solo in `AURORA` e richiede `FX SCENA`.

Se parte un cambio scena mentre uno o piu' pad sono premuti, il gesto gia'
catturato resta stabile fino all'ultimo rilascio. Una nuova pressione usa poi il
gesto della scena di destinazione. La pagina **GESTI** mostra il suo nome, la
scena catturata e il numero di pad attualmente tenuti.

Poiche' il controller e' pensato per essere montato sul sax, il gesto elabora
principalmente **SAX + RESPIRO** e non segue la memoria selezionata sul touch.
I controlli momentanei touch continuano invece a seguire la memoria scelta.
Questa semplificazione riguarda soltanto NM2: i controlli touchscreen
**GRANA** e **FUZZ** restano indipendenti e mantengono il comportamento
persistente gia' disponibile.

Il bus sax contiene sia l'ingresso live sia l'eventuale memoria RESPIRO, quindi
il gesto NM2 trasforma entrambi. Separare completamente il soffio presente
dal loop richiederebbe un secondo percorso effetti; questa versione evita quel
costo aggiuntivo sul Raspberry Pi e mantiene intatti i tre loop MIDI e il basso.

I profili di scena riusano il DSP del sax gia' preparato e non aggiungono
buffer, linee di delay, riverberi, voci o thread nel callback realtime.

Le note 60-77 sul canale 1 vengono consumate soltanto quando arrivano
dall'endpoint riconosciuto come NM2: non vengono registrate nei loop MIDI, non
suonano una voce e non possono essere apprese come pedale RESPIRO. Una tastiera
diversa sul canale 1 non attiva accidentalmente i gesti. Una nuova scansione,
la scomparsa dell'endpoint o la chiusura dell'app rilasciano tutti i pad come
panic, anche se si e' perso un `Note Off`.

Per il live e' consigliato il collegamento USB-C, piu' semplice da verificare e
meno esposto a una perdita di pacchetti. NM2 puo' inviare MIDI via USB e
Bluetooth contemporaneamente, ma Commento abilita deliberatamente un solo
endpoint NM2 e, quando riesce a distinguerli dal nome, preferisce USB: cosi' una
pressione non arriva due volte. Per usare BLE, eseguire prima pairing e
connessione MIDI nel sistema operativo del Raspberry Pi, verificare che
l'endpoint compaia in `aconnect -l`. Commento adotta automaticamente una porta
NM2 che compare dopo l'avvio; **RILEGGI MIDI** resta disponibile come panic e
riserva manuale. La
[guida ufficiale di collegamento](https://thisisnoiseinc.com/blogs/nm2-manual/how-to-connect)
descrive entrambe le modalita'.

Nella
[NM2 Web App](https://thisisnoiseinc.com/blogs/nm2-manual/customize-midi)
lasciare invariati canale e note dei 18 pulsanti. I controlli continui non usati
restano ignorati: conviene disabilitarli o assegnarli a messaggi che Commento
non usa, cosi' non generano traffico inutile, soprattutto via BLE. Le
caratteristiche dei controlli e l'uso simultaneo USB/Bluetooth sono riepilogati
anche nella
[pagina ufficiale NM2](https://thisisnoiseinc.com/blogs/nm2-manual/general-info).

`NESSUNO - CAPTURE OFF` nel campo dispositivo di ingresso apre davvero solo
l'uscita audio: nessun flusso di cattura viene richiesto al driver. E' diverso da
`NESSUNO - ROUTE MUTA` in CANALE SAX IN, che lascia il dispositivo di ingresso
aperto ma non inoltra alcun canale al motore. Questa distinzione e' utile per
capire se un disturbo nasce dal full-duplex/driver oppure dal routing del sax.

Quando si usa la Model 12, impostare manualmente sul mixer:

- canali 1/2 su `PC` per le tre voci ambient;
- canale 5 su `PC` per il basso generato dal MIDI 5;
- canale stereo 7/8 su `PC` per monitor ed effetti del sax;
- `USB AUDIO: MULTI INPUT`;
- `MTR/USB SEND POINT: PRE COMP`.

Il punto `PRE COMP` permette di inviare al Raspberry l'ingresso analogico del
sax prima del ritorno PC. E' una regolazione manuale della Model 12: Commento non
puo' leggerla ne' confermarla via USB. Partire con fader 7/8 e casse bassi: se il
meter sax non reagisce o il livello cresce da solo, abbassare immediatamente 7/8
e ricontrollare le impostazioni sul mixer.

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

- usare le frecce grandi in alto per scegliere uno dei quattordici scenari;
- usare **GRANA** per scegliere `PULITA`, `LEGGERA`, `MEDIA` o `PIENA`; il valore
  iniziale e' PULITA e viene ricordato al riavvio. Oltre a muovere drive,
  rumore, detune e instabilita' degli scenari, ora miscela realmente una
  riduzione a circa 6 kHz e sei bit. Il solo percorso trattato viene addolcito
  da un passa-basso a circa 4,2 kHz, mentre il dry resta intatto. Entra in 1,5
  secondi e si ritira in 3 secondi, quindi non crea un bordo netto;
- usare **FUZZ** per introdurre una saturazione dura ma contenuta. Anche FUZZ
  entra in 1,5 secondi e si dissolve in 3 secondi; riparte spento a ogni avvio;
- aprire **GESTI** per accedere ai trasformatori senza comprimere la pagina
  delle memorie. La pagina separa trasformazioni globali, pad momentanei e
  comportamenti automatici; il riquadro BERSAGLIO mostra sempre quale memoria
  verra' catturata alla pressione;
- usare **DERIVA: RARA** quando si desiderano variazioni non programmate. Parte
  spenta: una volta attiva, aggiunge occasionalmente una sola ombra a volume
  ridotto senza sostituire il materiale originale. Sui loop MIDI segue una
  sola linea e sceglie fra reverse e una copia esclusivamente `+12` (mai
  `-12`); su RESPIRO usa un frammento reverse oppure una testina a velocita'
  doppia, cioe' un'ottava-nastro, con ingresso di 400 ms e uscita di 1 secondo.
  Il primo evento arriva dopo circa 8-12 secondi, poi lascia 18-35 secondi di
  respiro fra un evento e il successivo. La card mostra `OMBRA +12` o
  `OMBRA REVERSE` durante il giro interessato. Spegnendo DERIVA non partono
  nuovi eventi; l'ombra eventualmente in corso termina con la propria
  dissolvenza invece di essere troncata;
- attivare **DIRADA** per aprire ogni tanto uno spazio nel tessuto: una sola fra
  MAREA, RADICE e SCINTILLA trascorre un giro completo in silenzio e rientra
  con una dissolvenza. La card interessata mostra `RESPIRA`. BASSO LIVE e
  RESPIRO non vengono mai diradati, il materiale non viene cancellato e la
  funzione riparte spenta a ogni avvio. Spegnendola durante `RESPIRA`, il giro
  gia' iniziato termina al proprio confine prima del rientro: cosi' non perde
  note iniziate nella parte silenziosa e non sposta la fase del loop;
- toccare una card per selezionare BASSO LIVE, MAREA, RADICE, SCINTILLA o
  RESPIRO;
- regolare il grande controllo **LIVELLO** della card selezionata; i cinque
  valori sono indipendenti, partono da -6 dB e vengono ricordati al riavvio;
- tenere premuto **GELO** per trattenere la coda della card selezionata. Riusa il
  delay e il riverbero gia' attivi: non crea copie del buffer e torna al
  comportamento dello scenario quando si rilascia. E' escluso dal BASSO LIVE;
- tenere premuto **ECO THROW** per richiamare il contenuto del delay della card
  selezionata e catturare il suono eseguito durante la pressione. Wet e feedback
  salgono senza modificare il valore DELAY salvato; dopo il rilascio rientrano
  lentamente in quattro secondi, lasciando parlare la coda;
- tenere premuto **CODA LIBERA** per sfumare il segnale diretto della card
  selezionata e lasciare decadere liberamente delay e riverbero gia' presenti.
  Il bersaglio viene catturato alla pressione e resta lo stesso fino al rilascio,
  anche se nel frattempo cambia la selezione. E' disponibile per MAREA, RADICE e
  SCINTILLA, oltre che per RESPIRO nel percorso EFFETTI SCENA; non e' disponibile
  per BASSO LIVE e non crea nuovi buffer o riverberi;
- usare **PAUSA LOOP** nella pagina GESTI per fermare soltanto la memoria
  selezionata e **PLAY LOOP** per farla ripartire. MAREA, RADICE, SCINTILLA e
  RESPIRO hanno stati indipendenti; BASSO LIVE non e' un loop e il controllo e'
  disabilitato. Per i loop MIDI resta disabilitato mentre la memoria e' armata
  o in registrazione; su RESPIRO, durante NUTRI, diventa **FERMA NUTRI E PAUSA**
  ed esegue entrambe le azioni con un solo tocco. La card mostra `IN PAUSA`, la fase resta congelata
  e PLAY riprende dallo stesso punto. La pausa non cancella il materiale e non
  viene salvata al riavvio. Su RESPIRO si ferma soltanto il loop registrato: il
  monitor del sax live continua; nel percorso FX SCENA anche la coda condivisa
  sfuma in 100 ms, cosi' non maschera lo stop, e rientra dolcemente con PLAY.
  SEMINA, NUTRI / OVERDUB e DIMENTICA riportano automaticamente quella memoria
  in PLAY;
- attivare **ASCOLTO** per far arretrare gradualmente soltanto il bus AMBIENTE
  quando il sax live entra, fino a circa 7 dB. BASSO LIVE, RESPIRO e routing
  fisico restano invariati; il follower riusa il meter sax del blocco precedente
  e non aggiunge una seconda analisi dell'ingresso;
- **GELO** entra in 80 ms e si ritira in 350 ms. Il congelamento riguarda il
  delay; il riverbero continua a decadere naturalmente, evitando lo switch
  binario che poteva creare un bordo su code dense. ECO THROW, CODA LIBERA,
  ASCOLTO e DIRADA conservano le proprie rampe progressive;
- un controller MIDI configurato sul Keystep puo' inviare `CC80` per GELO,
  `CC81` per ECO THROW, `CC82` per ASCOLTO, `CC83` per CODA LIBERA e `CC84`
  per DIRADA. CC80, CC81 e CC83 sono momentanei (`0-63` rilascia, `64-127`
  preme); CC82 controlla direttamente l'intensita' `0-127`. CC84 imposta uno
  stato assoluto: usare `127` per accendere DIRADA e `0` per spegnerla. Questi
  cinque CC sono consumati dal motore, non finiscono nelle memorie MIDI e non
  possono essere appresi come pedale RESPIRO. Il sustain `CC64` resta libero
  finche' non viene appreso esplicitamente come pedale RESPIRO; dopo
  l'associazione viene consumato dal comando del looper. Se si perde un rilascio
  MIDI, aprire **CONNESSIONI** o usare **RILEGGI MIDI** come panic; anche
  `CC120/123` rilasciano i tre gesti momentanei;
- su BASSO LIVE usare **MUTA BASSO LIVE** / **RIATTIVA BASSO LIVE**; e' un mute
  rapido e il MIDI 5 suona il basso senza essere registrato in una memoria MIDI;
- su MAREA, RADICE e SCINTILLA, premere **SEMINA** per armare la registrazione:
  il comando scatta al contatto (mouse-down/touch-down) e arma subito la
  memoria; l'interfaccia mostra **ATTENDO NOTA**. Il primo `note-on` avvia
  realmente il ciclo e diventa il timestamp zero, evitando il
  silenzio tra il tocco di SEMINA e la prima nota. Gli eventi successivi
  conservano il proprio timestamp all'interno del blocco audio; premere di
  nuovo **ATTENDO NOTA** prima di suonare annulla l'armamento senza creare un
  loop;
- premere **CHIUDI IL CICLO** per stabilire la durata libera;
- su MAREA, RADICE e SCINTILLA gia' popolate, premere **NUTRI / OVERDUB**:
  il loop originale continua a suonare e il playhead non riparte da zero. Le
  nuove note, il sustain e i controlli espressivi vengono collocati sulla fase
  corrente senza modificare la durata del ciclo; **FERMA OVERDUB** fonde la
  nuova take con quelle precedenti. Se due take sovrappongono la stessa nota,
  Commento la mantiene come un unico gate continuo fino all'ultimo note-off,
  evitando che una take tronchi l'altra. Un riavvio del dispositivo audio
  durante NUTRI scarta soltanto la take non ancora chiusa e conserva la memoria
  base;
- tenere premuto **TIENI PER DISSOLVERE** per 1,1 secondi per cancellare;
- su RESPIRO, premere **SEMINA** per iniziare la registrazione audio oppure
  **NUTRI / OVERDUB** per aggiungere nuovo suono mentre la memoria precedente
  viene consumata lentamente; questa registrazione parte direttamente dal
  pulsante e non attende una nota MIDI;
- il sax parte in stereo dalla coppia configurata; con il profilo MODEL 12 la
  coppia predefinita e' 7/8. Il pulsante grande di RESPIRO permette comunque di
  passare a mono dal solo canale sinistro quando si usa un unico ingresso;
- **PERSISTENZA DEL RESPIRO** decide quanto materiale precedente sopravvive a ogni
  overdub (`1.000` seleziona la persistenza massima; internamente resta un
  margine di sicurezza dello 0,5%, mentre valori inferiori dissolvono piu'
  rapidamente il passato).

### COSMOS: quattro testine per RESPIRO

COSMOS e' un comportamento originale ispirato all'idea di memoria drifting di
SOMA COSMOS, non un'emulazione del dispositivo. In questo scenario il playback
automatico di RESPIRO viene ricombinato da quattro testine asincrone, con
lunghezze e derive differenti, per attenuare la sensazione di un ciclo identico.
Il sax live e questa memoria in deriva restano sul bus SAX, quindi sul profilo
Model 12 escono da AUDIO 7/8.

Per usarla, selezionare **RESPIRO**, registrare il sax con **SEMINA** e chiudere
il ciclo; **NUTRI / OVERDUB** continua ad aggiungere nuovo suono e a consumare
lentamente la memoria senza ridefinirne la durata. Il MIDI 5 resta indipendente
e continua sempre a pilotare BASSO LIVE sul canale AUDIO 5. In COSMOS il basso
usa due denti di sega band-limited con un detune molto stretto e lento, piu' un
sub sinusoidale un'ottava sotto; attacco e rilascio sono piu' morbidi rispetto
alla precedente coppia di onde quadre.
I nuovi gesti non cambiano questo routing o il comportamento automatico di
COSMOS; intervengono soltanto mentre vengono azionati esplicitamente.

Il percorso PULITA e' lineare ai livelli normali. Le protezioni intervengono solo
vicino al fondo scala; GRANA reintroduce gradualmente saturazione, rumore e
instabilita' previsti dallo scenario. Dopo un aggiornamento da una versione
precedente, cancellare un
loop RESPIRO gia' distorto: la distorsione era memorizzata nel buffer e non puo'
essere rimossa retroattivamente.

### Diagnostica graduale del sax

Se il sax sembra sottocampionato, distorto o instabile, non cambiare molti
parametri insieme. Abbassare prima casse e fader, disattivare NUTRI, cancellare
un eventuale loop RESPIRO gia' distorto e usare questa sequenza nella pagina
CONNESSIONI. Dopo ogni modifica premere **APPLICA AUDIO** e controllare XRUN,
**DSP** e **PICCHI**.

1. **CAPTURE OFF + tono 997 Hz.** Scegliere `NESSUNO - CAPTURE OFF`, attivare il
   tono sul bus SAX e ascoltare la sua uscita configurata. Se anche il tono e'
   sporco, il problema e' a valle del sax: uscita, frequenza/clock, buffer,
   driver o conversione del mixer.
2. **Capture attiva + MUTO.** Riaprire l'ingresso corretto, lasciare il tono
   acceso e scegliere `MUTO - CAPTURE RESTA APERTA`. Se il difetto appare solo
   ora, e' legato all'apertura full-duplex, al driver/clock o agli xrun, non al
   looper o agli effetti del sax.
3. **DIRETTO PROTETTO -4,7 dB.** Spegnere il tono e scegliere questo percorso:
   l'ingresso viene inviato linearmente al bus sax, attenuato per sicurezza,
   senza looper e senza effetti. La protezione dai livelli quasi a fondo scala
   resta attiva e viene segnalata nell'header. Un difetto qui indica prima di
   tutto canale fisico errato, livello/preamplificatore, flusso USB o
   impostazioni manuali del mixer.
4. **LOOPER PULITO.** Questo percorso aggiunge la memoria RESPIRO, ma esclude il
   trattamento dello scenario. Se DIRETTO PROTETTO e' pulito e questo no, cancellare
   il loop e verificare registrazione/overdub.
5. **EFFETTI SCENA (FX).** E' il percorso musicale completo. Se i primi quattro
   test precedenti sono puliti e il difetto compare qui, cercare la causa nel trattamento
   sax, nello scenario o nella quantita' di GRANA.
6. **Tono sui tre bus.** Senza suonare, spostare il test 997 Hz tra `AMBIENTE`,
   `BASSO` e `SAX`: ogni tono deve arrivare all'uscita fisica scelta per quel
   bus. Questo controlla la mappa anche con una scheda diversa dalla Model 12.

Sulla Model 12 verificare inoltre manualmente `MTR/USB SEND POINT: PRE COMP`, non
`POST COMP` o `POST EQ`, e regolare il preamplificatore per leggere tra -18 e
-12 dB sul sax. Con 7/8 su `PC`, partire sempre con il fader 7/8 basso. Se
l'ingresso resta quasi a fondo scala per circa 180 ms, Commento interrompe
automaticamente il ritorno e mostra `PROTEZIONE SAX`; torna gradualmente attivo
dopo un secondo di silenzio. Questa protezione evita il picco, ma non certifica
la configurazione PRE COMP e non diagnostica da sola disturbi di clock o driver.

## I quattordici scenari

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
| DRONE | masse lente, pedali e deriva profonda | colonna d'aria ampia e lenta |
| FERRO | urti metallici e risonanze corte e taglienti | lastra corta, brillante e mobile |
| SCIAME | rumore vivo, scatti e traiettorie instabili | ronzio granuloso e nervoso |
| COSMOS | anelli armonici e respiro sospeso | quattro testine asincrone |

Le note dei loop restano le stesse quando si cambia scenario: vengono suonate
di nuovo con i nuovi strumenti. Timbro, tono, spazi e PERSISTENZA passano alla
nuova scena gradualmente in otto secondi; richieste molto rapide vengono messe
in coda e resta valida l'ultima, evitando di spezzare un delay nel mezzo. Il
basso MIDI 5 cambia timbro, resta live in
tutti gli scenari e usa il bus del canale I; nel profilo Model 12 quel bus parte
dall'uscita audio 5. Il sax live e la memoria RESPIRO continuano a usare il bus
SAX su audio 7/8.

Il morph e' timbrico, non riscrive l'armonia: un loop in Re dorico conserva le
proprie note. Una futura variazione armonica dovra' essere un gesto separato sui
soli loop, cosi' il basso live puo' continuare a seguire liberamente i beat.

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

Su Linux e' attivo per default il workaround
`COMMENTO_ALSA_BYPASS_PCM_LINK`: evita che JUCE avvii insieme capture e
playback prima di avere riempito il buffer di uscita. Nel journal, quando viene
aperta una configurazione full-duplex, deve comparire:

```text
Commento ALSA: snd_pcm_link bypass attivo; capture e playback partono indipendenti
Commento ALSA: playback prefill attivo; avvio dopo 2 periodi
```

Poiche' i due stream partono indipendenti, Commento porta inoltre la soglia di
avvio della sola uscita da uno a due periodi. Con un solo periodo, una callback
piu' pesante di quella iniziale poteva raggiungere il bordo del ring ALSA pur
mostrando `DSP` basso e `XRUN 0`; il secondo periodo fornisce 10,7 ms di riserva
a 48 kHz/512. Il costo e' un periodo aggiuntivo di latenza di uscita, senza
ridurre polifonia o modificare il suono dei loop.

Per una prova A/B, ricompilare la versione JUCE originale in una cartella
separata disabilitando il workaround:

```sh
cmake -S . -B build-pi-linked \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DCOMMENTO_ALSA_BYPASS_PCM_LINK=OFF
cmake --build build-pi-linked --parallel 3
```

La build normale (`build-pi`) mantiene invece il workaround attivo. Le due
versioni permettono di ripetere lo stesso test con capture attiva, percorso
MUTO e tono 997 Hz senza cambiare routing o livelli del mixer.

Prima di avviare Commento, verificare che l'hardware USB sia visibile:

```sh
lsusb
aplay -l
arecord -l
aconnect -l
```

Impostare le quattro parti del Keystep Pro, nell'ordine, sui canali MIDI
5, 2, 3 e 4. CONNESSIONI permette di scegliere l'hardware audio e verificare la
configurazione effettiva; KeyStep, Model 12 e NM2 vengono cercati
automaticamente. Se si collega a caldo un controller MIDI, usare **RILEGGI
MIDI**; per una scheda audio usare **RILEGGI DISPOSITIVI**. Se ALSA non espone
ancora il nuovo endpoint al processo, riavviare l'applicazione.

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

Il servizio concede a Commento memoria bloccabile e priorita' realtime. L'app
blocca in RAM loop e delay e porta esplicitamente il callback ALSA su
`SCHED_FIFO`: JUCE su Linux non applica da solo una policy realtime quando usa
la generica priorita' `high`. In CONNESSIONI devono quindi comparire
**PRIORITA AUDIO OK**, un valore **DSP** ben sotto il 90% e **PICCHI 0**. Se la
priorita' non e' attiva, avviare Commento tramite il servizio reinstallato,
non direttamente da una shell con limiti utente differenti. Il journal indica
anche esplicitamente se la memoria audio residente e' stata attivata.

La riga effettiva mostra inoltre **INTERVALLO** e **RITARDI**. `PICCHI` aumenta
quando il calcolo dentro il callback si avvicina alla propria scadenza;
`RITARDI` aumenta invece quando il callback comincia con oltre il 35% di ritardo,
anche se il suo calcolo interno e' veloce. Per un crackle raro annotare entrambi:
`PICCHI 0` con `RITARDI` in crescita indica prima di tutto scheduler/USB, mentre
entrambi fermi orientano verso una discontinuita' del segnale o un problema a
valle dell'app. Un **INTERVALLO** vicino al 100% e' normale: significa che i
callback iniziano a distanza di un periodo l'uno dall'altro.

I loop audio sono contenuti in RAM e non vengono letti continuamente dalla
microSD. Spostarli su SSD non riduce il crackling; un SSD puo' essere utile per
il sistema e per futuri salvataggi, ma non accelera questo percorso realtime.

Il launcher attende che udev completi l'enumerazione dei dispositivi gia'
collegati, ma non impone piu' il nome Model 12: la pagina CONNESSIONI puo' quindi
usare qualunque interfaccia ALSA. Poiche' la scansione ALSA di JUCE avviene una
sola volta per processo, dopo aver collegato a caldo una scheda assente all'avvio
puo' essere necessario riavviare il servizio. Se un'installazione deve partire
solo in presenza di una scheda specifica, si puo' aggiungere alla unit systemd,
per esempio, `Environment="COMMENTO_AUDIO_CARD_PATTERN=MODEL ?12|TASCAM"`.

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
- `Model12AudioRouter`: adattatore configurabile tra i canali fisici
  dell'interfaccia scelta e i cinque bus logici;
- `Scenarios`: quattordici orchestrazioni per canale I, tre layer ambient e sax;
- `SaxProcessor`: tono, delay, modulazione, riverbero e protezione del bus sax;
- `MidiMemory`: eventi MIDI con posizione in campioni e durata indipendente;
- `AudioMemory`: buffer circolare stereo con overdub, decadimento e, in COSMOS,
  quattro testine asincrone;
- `MainComponent`: interfaccia touch, routing device e animazione.

GELO ricircola i delay gia' allocati con una matrice di feedback normalizzata;
ECO THROW smussa l'inviluppo una volta per blocco e riusa il DSP DELAY esistente;
CODA LIBERA e DIRADA lavorano sui guadagni gia' preparati, mentre ASCOLTO opera
una volta per blocco sul bus ambient gia' sommato. Nessuno dei cinque alloca
buffer, crea riverberi o aggiunge voci nel callback realtime.

Il callback classifica KeyStep, porta standard Model 12 e NM2 dal nome
dell'endpoint. Le 18 note dedicate dell'NM2 alimentano lo stesso gate
momentaneo, associato al profilo della scena; gli altri messaggi dello stesso
endpoint sono scartati, cosi' controlli e preset personalizzati non possono
entrare nei loop. Gli eventi destinati alle voci e alle memorie passano invece
in una FIFO preallocata. Le memorie vengono modificate dal thread audio; in caso
di overflow viene inviato un panic automatico e il contatore compare
nell'indicatore MIDI.

Le specifiche USB e il routing MULTI INPUT sono descritti nel
[manuale ufficiale Tascam Model 12](https://www.tascam.eu/en/docs/Model12_OM_EFS_RevH3.pdf);
il percorso `PRE COMP` e i ritorni PC 1-10 sono visibili nel
[diagramma a blocchi ufficiale](https://www.tascam.eu/en/docs/Model12_SettingsPanel-V2_block-diagram.pdf).
