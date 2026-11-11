# Numara: cattura completa e lavoro da proseguire

11 novembre 2026. Ramo: `steprant/v2.0-dev`.

Questa modifica amplia la diagnostica. Non corregge ancora l'acqua e non
dichiara miglioramenti misurati sul telefono. Revisionarla e includerla nella
prossima APK di prova in bozza, mantenendo gli interventi già presenti sul ramo.

## Evidenza della sessione 00:52, build 2.0.1b

Nel frame 7438 il resolve 5 conserva la scena pulita. Il draw 770 azzera le
viste colore tramite un riempimento di profondità. Il trasferimento successivo
da classe 1 a 8888 indica un azzeramento ancora in attesa nella sorgente.
I draw 771–776 producono la mappa di distorsione. Il draw 777 la campiona
insieme alla scena. Il resolve 7 mostra la deformazione e le copie spostate.

Le sagome nella mappa possono dipendere dal test di profondità. Da sole non
dimostrano che il buffer contenga colori residui. La quarta componente viene
usata dalla distorsione ma le vecchie anteprime la sostituivano con un valore
opaco: non consentivano di ricostruire l'intero vettore di spostamento.

La nuova cattura aggiunge immagini RGBA e una vista alpha in grigio, il valore
degli azzeramenti e sorgente/destinazione dei primi due trasferimenti larghi
verso 8888. Le copie vengono dopo la scelta del percorso di trasferimento,
per non consumare in anticipo gli azzeramenti che ne determinano il percorso.
Sono anteprime a 8 bit, eventualmente ridotte, non parole EDRAM grezze.

## Attenzione ai confronti precedenti

Il commit `c8aafbe` corregge gli interruttori A/B che nella 2.0.1b non
disattivavano effettivamente i pacchetti interessati. Ripetere i confronti su
una nuova APK e verificare i valori applicati nel log. La precedente
esclusione di frame graph, copie differite, area di render e target compatti
non è quindi dimostrata da quelle prove.

## Fluidità: nessun guadagno dimostrato

Escludendo menu, caricamento e cattura, gli intervalli 108,0–236,4 s e
278,5–301,7 s del log danno 4.540 campioni, p95 34,151 ms, p99 37,341 ms,
93,63% entro ±1 ms dai 33,333 ms e tre intervalli oltre 50 ms in 151,58 s.
Il riferimento fornito per la 2.0 Final è p95 34,0 ms e 96% entro ±1 ms.
I percorsi non costituiscono un confronto controllato identico.

L'unico avviso di blocco audio, 57 ms, è durante il menu. Non attribuire
automaticamente menu, cattura o caricamenti alla fluidità del gioco.

## Prossimo intervento

Revisionare la modifica, compilare la prossima APK diagnostica in bozza e
verificarne SHA e certificato di firma. Nessuna pubblicazione automatica.
Confrontare lo stesso punto del canale con SMAA a 900p e a 720p, 30 secondi
per prova, e con SGSR2. Richiedere quattro frame per ciascuna configurazione
ed esportare ogni prova prima di iniziare la successiva.

Usare le nuove immagini e gli azzeramenti registrati per stabilire se il
target parte dal contenuto corretto. Correggere la transizione EDRAM solo
quando l'evidenza lo giustifica. Non modificare gli shader tradotti o il
contratto del pack. Chiudere la versione solo dopo verifica sul telefono.

## Verifiche locali

Il renderer completo supera il controllo di sintassi C++20. La regressione
dei trasferimenti passa 389.438 controlli. Passano i test degli interruttori,
della continuazione SGSR2 e del confronto PNG/frame. Una verifica temporanea
dell'esatto ciclo di esportazione preserva i byte RGBA e i livelli alpha
0, 85, 170 e 255. La nuova cattura richiede ancora verifica sul telefono.

Nessun dato del gioco, shader del gioco, archivio diagnostico privato,
segreto o collegamento privato è incluso in questo intervento.
