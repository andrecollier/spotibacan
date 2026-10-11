# Reporte — Fase 5: audio. Bloqueado en la clave, y no parece ser el código

Fecha: 2026-10-10 · Rama: `develop`

## Estado

La cadena de audio está entendida y verificada **hasta un paso antes del final**. El bloqueo es
la clave de audio: el access point la niega con `AesKeyError 0001` en todos los casos probados.

```
metadata (gid + file_id)   OK   -- 4 formatos, Vorbis 96/160/320 + AAC
clave de audio (AP)        FAIL -- AesKeyError 0001, siempre
storage-resolve            OK   -- result=0 (CDN), 3 URLs firmadas por formato
descarga del CDN           sin probar (no tiene sentido sin la clave)
descifrado AES-128-CTR     escrito, sin probar
```

Lo importante: **storage-resolve devuelve URLs firmadas del CDN para los cuatro formatos**, o sea
que la cuenta sí tiene derecho a ese audio. Los bytes cifrados están disponibles; lo único que se
niega es la clave para descifrarlos.

## Lo que se descartó, con evidencia

- **El plan de la cuenta no es el problema.** El propio AP manda un SecretBlock que contiene
  `<type>premium</type>` y `catalogue>premium`. Es Premium, confirmado por Spotify, no por fe.
- **El pedido es byte por byte el de librespot.** `file_id`(20) + `gid`(16) + `seq`(u32 BE) +
  `0x0000`, paquete `0x0c`. Y el `gid` está verificado contra el base62 del URI:
  `3n3Ppam7vgaVa1iaRUc9Lp` → `6ecac90c03f74dcba9e21b408e14719f`, igual al del metadata.
- **No es una ruta obsoleta.** `player.rs:1111` de librespot actual sigue usando
  `session.audio_key().request()`.
- **No es el formato.** Los cuatro (Vorbis 96/160/320 y AAC) dan el mismo `0001`.
- **No es restricción del tema.** El metadata no trae `restriction` ni `alternative`.
- **No es el método de autenticación del AP.** Falla igual entrando con el token OAuth
  (`AUTHENTICATION_SPOTIFY_TOKEN`) y con las credenciales reutilizables
  (`AUTHENTICATION_STORED_SPOTIFY_CREDENTIALS`), que es lo que librespot usa a partir del segundo
  arranque.
- **No es una sesión sucia.** Falla en sesión recién abierta, con y sin drenar la ráfaga inicial
  del AP, y con `seq` 0 y siguientes.
- **No es ese tema en particular.** Un segundo tema falla igual.

## RESUELTO: no es el código. Es un bug conocido de Spotify, sin arreglo

Emilio corrió librespot con la misma cuenta y **falla exactamente igual**:

```
ERROR librespot_core::audio_key] error audio key 0 1
WARN  Unable to load key, continuing without decryption: Service unavailable
ERROR Unable to read audio file: Symphonia Decoder Error: end of stream
ERROR Skipping to next track, unable to load track
DEBUG marking spotify:track:... as unavailable
```

`error audio key 0 1` es literalmente el mismo `0001`. Y lo que parecía reproducción era el
salteo: baja el archivo, no consigue la clave, intenta decodificar bytes cifrados, falla con
"end of stream" y pasa al siguiente. Nunca sonó nada.

Es [librespot#1649](https://github.com/librespot-org/librespot/issues/1649), **abierto desde el
21-nov-2025 y sin arreglo**. Los mantenedores no lo pueden reproducir; kingosticks especuló
"They may be phasing out this way of getting the key", sin confirmar.

Lo decisivo para nosotros es [#1735](https://github.com/librespot-org/librespot/issues/1735),
cerrado como duplicado: **una cuenta Premium falla y otra funciona en la misma máquina**. O sea
que la variable es la cuenta, no el software ni el código. El reportante sospechó de la edad de
la cuenta, pero otro usuario vio el fallo en una vieja y en una nueva, así que la edad por sí
sola no lo explica.

## El test que lo resolvió

Correr **librespot**, que ya está instalado (0.8.0), con la misma cuenta, y hacerle llegar una
orden de reproducción desde el teléfono o la compu:

```bash
librespot --name "prueba-3ds" --backend pipe --verbose 2>&1 | tee /tmp/librespot.log
```

Después, en Spotify del teléfono o de la compu, elegir **prueba-3ds** en la lista de dispositivos
(Connect) y darle play a cualquier tema. Y buscar en el log:

```bash
grep -iE "audio key|aeskey|error" /tmp/librespot.log | head
```

Resultado: librespot también falla. Queda descartado el port.

**La única variable que se sabe que da vuelta el resultado es la cuenta.** Si Emilio consigue
otra cuenta Premium, se prueba en diez minutos: el cliente ya está escrito hasta la clave, y el
resto de la cadena (storage-resolve, CDN, AES-CTR) está entendido y documentado.

## Mi propio bug, encontrado por el camino

La referencia en Python avanzaba el nonce de Shannon **antes** de leer el header, así que un
timeout consumía un nonce sin consumir bytes y desincronizaba el cifrador para siempre: la sesión
quedaba muerta sin ningún error que lo dijera. **El C no tiene este bug** — `ap_recv()` lee el
header primero y recién entonces avanza el nonce, y para eso existe `ap_wait()`. Vale anotarlo
porque es la clase de error que no da síntomas útiles: no falla la lectura, fallan todas las
siguientes.

## Lo que sí quedó listo de la Fase 5

- `storage-resolve` entendido y probado: `GET /storage-resolve/files/audio/interactive/<file_id hex>`
  devuelve `StorageResolveResponse` con `result` (0 = CDN) y hasta 3 URLs firmadas con expiración.
- El pedido de clave y la respuesta: paquetes `0x0c` / `0x0d` / `0x0e` sobre la sesión Shannon.
- El descifrado: AES-128-CTR con el IV fijo
  `72 e0 67 fb dd cb cf 77 eb e8 bc 64 3f 63 0d 93`, contador por bloque desde el offset del
  archivo.
- Que el header de Spotify antes del Ogg **no se hardcodea**: se busca el magic `OggS`, que es
  robusto y además dice cuál es el offset real.

Nada de esto se escribió todavía en C para la consola: habría sido escribir contra un paso que no
se puede probar.

---

# Fase 5b: el decodificador Vorbis y la salida de audio

Fecha: 2026-10-11

Con la clave bloqueada por Spotify, la otra mitad de la Fase 5 sí se puede hacer: decodificar
Vorbis en el ARM11 y sacarlo por NDSP. **Es el último desconocido técnico grande del proyecto** —
es donde el PSP tuvo que recurrir al Media Engine, su segundo procesador, y el 3DS no tiene
equivalente que valga la pena: el decodificador comparte el ARM11 con la red y la interfaz.

## Qué se construyó

- `deps/stb_vorbis.c` v1.22 (dominio público), pineado por SHA-256 igual que mbedTLS y bajado
  por `deps.py`.
- `probe_audio/` — lee `test.ogg` de la tarjeta y hace **dos pasadas separadas**:
  1. **Solo decodificar**, sin audio de por medio. Es la medición que importa y la única
     portable.
  2. **Reproducir por NDSP**, que necesita la firmware del DSP de la consola.
- `tools/make_test_ogg.py` — genera los archivos de prueba desde cualquier tema local.

Separarlas no fue cosmético: la primera versión las mezclaba y **se colgó** esperando que NDSP
liberara un buffer que nunca iba a liberar. Ahora una firmware ausente cuesta la prueba de
reproducción, no el banco de pruebas, y el bucle de reproducción se rinde tras 600 vueltas en
vez de girar para siempre.

## Resultado en Azahar

```
44100 Hz, 2 ch, 1543552 frames (0:35)
decoder memory: 162160 setup + 4096 temp bytes

-- pass 1: decode only --
   1543552 frames = 35.0 s of audio
   decode cpu 2408 ms over 378 chunks (6.4 ms each, worst 7.6)
   REALTIME FACTOR: 14.54x

-- pass 2: playback through NDSP --
   ndspInit failed: no DSP firmware.
```

**Correcto**: decodificó los 1.543.552 frames exactos, los 35,0 s completos, sin errores. El
decodificador y su integración funcionan.

**El 14,54x no vale como número de rendimiento**: es la CPU de la Mac. Y haciendo la cuenta al
revés, tampoco es tranquilizador: un núcleo moderno hace este trabajo en el orden de 30 a 60
veces lo que un ARM11 a 268 MHz, así que 14,54x podría traducirse a **menos de 1x en la consola**.
No es una predicción —el factor de conversión es muy grueso— pero sí es razón suficiente para no
dar por ganada esta fase.

Dos cosas juegan a favor: el archivo de prueba es de **230 kbps**, bastante más duro que los 160
de Spotify, y el New 3DS corre a 804 MHz con `osSetSpeedupEnable`.

Si no alcanza, el plan B está claro y es conocido: **tremor (libvorbisidec)**, el decodificador
Vorbis de enteros que usan los reproductores tipo Rockbox. stb_vorbis es de punto flotante y el
VFP del ARM11 es lento; tremor existe precisamente para este caso.

## Esto hay que correrlo en la consola

Es la primera vez que el emulador no puede contestar la pregunta: NDSP necesita la firmware del
DSP, y el número de rendimiento necesita el ARM11 de verdad.

Copiar a la SD la carpeta `client3ds/dist/3ds/`, que ya queda armada con:

```
SD:/3ds/probe_audio/probe_audio.3dsx
SD:/3ds/probe_audio/test.ogg        (230 kbps, el caso duro)
SD:/3ds/probe_audio/test-low.ogg    (97 kbps, para comparar)
```

Lanzarlo del Homebrew Launcher. Deja `spotify-audio.log` al lado. Lo que hay que mirar:

- **REALTIME FACTOR** de la pasada 1. Por encima de 3x es cómodo; entre 1,5x y 3x es justo pero
  alcanza; por debajo de 1,5x hay que pasar a tremor.
- **underruns** de la pasada 2, que deben ser 0.
- Si suena limpio.
