# Lost Odyssey Steprant · Micro scatti a 30 FPS

Indagine del 11 novembre 2026. Il ramo di sviluppo è `steprant/v2.0-dev`
(`07f22a2b` alla partenza). La build di test più recente è `steprant/v2.0.5`
(`1d6400c7`), su una linea divergente. Entrambe hanno la stessa sequenza
timer → presentazione e la stessa politica di riancoraggio quando l'acquire
supera 4 ms. La 2.0.5 ha già rimosso l'esperimento `VK_GOOGLE_display_timing`
e modifica la priorità del worker audio quando il suo carico varia. La PR
parte dallo sviluppo per esporre una diff di solo pacing e diagnostica.

## Dove si accumulano le attese

- In `PM4_XE_SWAP`, `renderer::PreparePresent` e `Flush` sottomettono la scena
  sulla coda grafica. `FramePacer` calcola una scadenza monotona. Il thread
  `GPU CmdProc` attende quella scadenza e solo dopo prepara la swapchain,
  aspetta il fence della presentazione precedente, acquisisce un'immagine,
  registra la composizione e chiama submit e present. La preparazione e le
  attese lato WSI si sommano quindi al timer. Le submission della scena e
  della presentazione condividono la coda grafica e il relativo mutex.
- Il renderer ricicla già più slot di comandi. La presentazione ha una sola
  command list, un fence e un semaforo di acquire. Il fence protegge il riuso
  di quella list, degli upload, dei descriptor set, delle query e delle
  risorse temporanee di `Presentation`. Il semaforo atteso da
  `vkQueuePresentKHR` è già per immagine e viene riusato con il suo acquire.
  La chiamata WSI può aspettare un'immagine libera anche quando il fence
  precedente è completo. Un present accettato descrive la coda lato app.
- Nelle sessioni in città dell'8 novembre, le note del ramo riportano
  40–55 ms nella fase present con circa 0,2 ms nel fence e altri 3–20 ms di
  pacing. Nelle sessioni successive in TV, 18 frame con `present-fence` come
  fase prevalente in circa 4.300 s e attese `cp_idle` di circa 250 ms a
  inizio battaglia. Il tempo GPU medio di 15,7 ms della TV descrive quelle
  sessioni. I due ZIP 2.0.1 dell'11 novembre riportano p95 39,60/40,48 ms e
  p99 51,91/80,35 ms dei present accettati, su percorsi diversi, con quattro
  switch modificati insieme e una pausa lunga. Servono come indizi e non
  costituiscono un A/B controllato per questo intervento.
- `LO_DISPLAY_TIMING=1` ha già trattenuto immagini nella coda WSI e portato
  l'acquire a 30–70 ms. L'ADPF non ha migliorato lo stutter in città nei test
  riportati. Guest Main, `GPU CmdProc` e alcuni thread audio preferiscono già
  i core ad alte prestazioni. Il pinning prime/big è un esperimento esistente.
  I log XMA e audio mostrano anche stalli con poca contesa misurata sul lock
  XMA. Per distinguere attese guest, I/O, tempi in coda CPU e competizione
  fra audio e grafica servono gli eventi `sched` e le frequenze di Perfetto.
- A 30 FPS servono due refresh a 60 Hz o quattro a 120 Hz. A 59,94 Hz due
  refresh durano circa 33,37 ms. Vanno registrati refresh effettivo, modalità
  HDMI e cambi di superficie. Una fase sbagliata rispetto al refresh può
  produrre alternanza fra intervalli corti e lunghi anche con una media di 30.

Fonti del progetto [ottimizzazione Android](android-optimization-20261108.md),
[stato della roadmap](android-2.0-roadmap-status.md) e [flusso frame/audio](android-2.0-flow.md).
Riferimenti [Khronos, riuso dei semafori WSI](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html),
[Android, frame pacing](https://developer.android.com/games/optimize/vitals/slow-session)
e [Perfetto, scheduling CPU](https://perfetto.dev/docs/data-sources/cpu-scheduling).

## Tre interventi candidati

| Priorità | Causa e modifica | Beneficio atteso | Rischio e verifica |
|---|---|---|---|
| 1. Preparazione WSI prima della scadenza | Spostare acquire, registrazione e submit della composizione GPU dentro il tempo già riservato al pacing. Applicare la stessa scadenza prima di `vkQueuePresentKHR`. Un'immagine preparata alla volta, sullo stesso thread. | Assorbire nel tempo residuo parte di acquire, fence e lavoro GPU. Rendere più regolare l'istante in cui il frame entra nella coda FIFO. | Un'immagine occupata in anticipo può esercitare pressione sulla WSI. Misurare acquire, submit, attesa residua, queue present, scadenze mancate e intervalli del layer, su telefono e HDMI. Misurare anche l'input-to-display. |
| 2. Presentazione con due slot e proprietario WSI dedicato | Separare la preparazione dalla chiamata potenzialmente bloccante di present. Trasferire un pacchetto immutabile con lease delle immagini e duplicare command list, fence, acquire semaphore, descriptor e upload per slot. Mantenere i semafori di present per immagine e limitare la coda CPU a un pacchetto. | Consentire al thread comandi di progredire durante i blocchi WSI e ridurre l'attesa per il riuso dello slot precedente. | Con una sola coda Vulkan il mutex può trasferire il blocco alla submit successiva. Servono misure di contesa e capacità delle queue family, test Vulkan Sync Validation, hotplug, resume, screenshot e latenza. |
| 3. Caricamento e audio isolati dalle attese guest | Le letture guest ordinarie passano ancora per I/O sincrono sotto un lock per handle. Correlare le attese `cp_idle` a richieste I/O e decodifica, quindi introdurre una coda limitata di prefetch per le risorse di transizione e un passaggio audio a buffer per il callback. | Ridurre gli stalli in ingresso in battaglia e nelle scene con voci, quando manca lavoro al renderer. | Il prefetch può aumentare memoria e pressione termica. Validare eventi/APC e semantica dei file, misurare p99 delle transizioni e underrun, e verificare voci, filmati, salvataggi e ripresa HDMI. |

La prima variante agisce sul tratto in serie riscontrato nella città e
consente un A/B a parità di binario. Il guest mantiene il cap, il clock e
il vblank virtuale. Il percorso Vulkan usa sempre l'ordinamento della coda
esistente, una sola submission di presentazione pendente e la normale
acquisizione FIFO. Un ritardo di almeno 1 ms oltre la scadenza di un frame
accettato sposta l'ancora al momento del rilascio verso present per evitare
un recupero ravvicinato. La soglia lascia passare il jitter del timer al di
sotto di 1 ms senza accumularlo nella cadenza.

## A/B sullo Xiaomi 14

Usare lo stesso APK di debug per A e B. `RuntimeActivity` inoltra gli extra
`LO_*` solo su un APK debuggable. Nel blocco seguente, `pkg` è l'ID installato
e `mode` vale `0` per A o `1` per B. Attendere il salvataggio prima di chiudere
il gioco e ricaricare lo stesso salvataggio in ogni sessione. Per HDMI usare
ADB wireless, lasciando collegato il cavo e mantenendo lo stesso refresh.

```sh
adb shell am force-stop "$pkg"
adb shell am start -n "$pkg/io.github.freefrank.lostodyssey.RuntimeActivity" \
  --es LO_VK_SUBMIT_BEFORE_PACE "$mode" --es LO_DISPLAY_TIMING 0
adb shell dumpsys SurfaceFlinger --list
adb shell dumpsys display > display.txt
```

Provare almeno tre coppie da cinque minuti, alternando A/B e B/A, per città,
campo e battaglia, sul telefono e in HDMI a 1080p. Scaldare il dispositivo per
lo stesso tempo e registrare temperatura, stato termico, alimentazione, refresh
effettivo, Turnip e impostazioni. Tenere fissi limite di 30 FPS, risoluzione,
SGSR2, SMAA e ombre. Disattivare overlay prestazioni e avanzamento rapido.
Iniziare il campionamento a scena avviata. Usare sessioni separate per cambi
di scena, sospensione e ripresa, ingresso e uscita dal menu e hotplug HDMI.

Indicare il nome esatto del layer `SurfaceView` del gioco dopo ogni riavvio e
verificare l'uscita associata. Il raccoglitore usa i timestamp delle fence di
presentazione fornite da SurfaceFlinger e richiede continuità fra campioni.
Se HyperOS non espone il layer o le fence, il comando fallisce. In quel caso
serve una cattura a refresh noto dell'uscita HDMI o una ripresa ad alto frame
rate del telefono. Evitare di sostituire i dati mancanti con i tempi delle
chiamate `vkQueuePresentKHR`.

```sh
python3 tools/perf/android-present-intervals.py capture --layer "$layer" --seconds 300 --out A-citta.csv
python3 tools/perf/android-present-intervals.py capture --layer "$layer" --seconds 300 --out B-citta.csv
python3 tools/perf/android-present-intervals.py compare A-citta.csv B-citta.csv
```

Confrontare p95/p99, percentuale entro 35 ms e intervalli oltre 50 ms al minuto.
Ispezionare la serie CSV per alternanza periodica degli intervalli. Gli obiettivi
sono almeno il 99% entro 35 ms, meno di uno oltre 50 ms al minuto, cadenza
effettiva vicina a 30 FPS e assenza di scatti ricorrenti. Per controllare se il
polling di `dumpsys` altera frequenze o temperatura, affiancare una sessione
con la sola registrazione Tester e confrontare la pressione termica.

Per attribuire gli stalli, fare coppie diagnostiche brevi a parte. Registrare
Tester con lo stesso profilo in entrambe e confrontare con
`python3 tools/android/compare-tester.py A.zip B.zip`. Il CSV Tester misura i
present accettati lato app. `gpu.csv` misura batch GPU completati e descrive
l'occupazione su una finestra. Le righe `slow frame` danno `cp_idle`,
`wait_reg_mem`, `gpu_fence_wait`, `present_acquire`, `submit_before_pace` e
`late_release_ms` per i picchi campionati.

Per ottenere le distribuzioni delle singole attese, registrare 30 s di Perfetto
per variante, con `sched`, `freq`, `gfx` e l'app `pkg` abilitata in ATrace.
Confrontare il tempo runnable di Guest Main, GPU CmdProc, Audio Driver e XMA
Decoder con la permanenza sui core, le frequenze e le fasi della GPU. Il marker
`LO/Pace after submit` prova che il frame ha percorso la variante B. In Trace
Processor la query seguente restituisce i tempi muro per fase.

```sql
WITH phases AS (
  SELECT name, dur,
         ROW_NUMBER() OVER (PARTITION BY name ORDER BY dur) AS rank,
         COUNT(*) OVER (PARTITION BY name) AS n
  FROM slice
  WHERE name GLOB 'LO/*' AND dur >= 0
)
SELECT name, MAX(n) AS samples,
       MAX(CASE WHEN rank = (95 * n + 99) / 100 THEN dur END) / 1e6 AS p95_ms,
       MAX(CASE WHEN rank = (99 * n + 99) / 100 THEN dur END) / 1e6 AS p99_ms
FROM phases GROUP BY name;
```

Controllare separatamente immagini a camera ferma, anello in battaglia,
voci e sincronizzazione audio/video a Uhra, salvataggio e ricarica, avvio del
filmato, riapertura del gioco e passaggio telefono/HDMI. Per le immagini della
variante usare una cattura dell'uscita o `adb exec-out screencap -p` sul telefono
in una sessione separata. Le catture GPU richieste dal Tester passano dalla
sequenza di presentazione ordinaria.

Fonti [SurfaceFlinger su Android 16](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/android16-release/services/surfaceflinger/SurfaceFlinger.cpp),
[Perfetto, registrazione](https://perfetto.dev/docs/getting-started/system-tracing)
e [Perfetto, supporto di FrameTimeline per i layer](https://perfetto.dev/docs/data-sources/frametimeline).

## Verifiche eseguite

Su host sono passati i test mirati per scadenza, frame pacer, progress,
caricamento, audio, overlay, diagnostica TV e raccolta SurfaceFlinger.
`video.cpp`, `command_processor.cpp` e `frame_timing.cpp` compilano per
Android ARM64 con NDK r28c e l'ABI guest della fixture del repository, sia sul
ramo di sviluppo sia con l'esperimento applicato alla 2.0.5. Per applicarlo alla
2.0.5, la registrazione CMake deve aggiungere solo `LoPresentDeadlineTest`
accanto a `LoFramePacerTest`, dato che lì `LoDisplayPacingTest` è stato rimosso.
La verifica locale produce oggetti nativi e non installa un APK.

ADB non rileva dispositivi collegati. P95, p99, scatti e possibili regressioni
visive, audio, salvataggi e HDMI rimangono da misurare sullo Xiaomi 14.

## Porting a freefrank/LostOdysseyRecomp

La modifica al motore e il test della scadenza sono in un commit a parte.
L'integrazione usa `FramePacer`, `DeadlineWait` e le primitive WSI esistenti.
Le versioni ufficiali senza il riancoraggio P6 possono aggiungere il piccolo
`FramePacer::Reanchor` insieme al gate, usando il risultato della submit riuscita
e il contatore dei present accettati. Le note e il raccoglitore sono indipendenti
dalla configurazione di build Steprant. Il porting va provato su Android Vulkan
prima di aprire una PR al progetto ufficiale.
