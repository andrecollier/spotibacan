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
