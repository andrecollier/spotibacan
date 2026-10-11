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

## Las dos explicaciones que quedan

1. **La cuenta tiene una o dos horas de vida.** Es plausible que el servicio de claves de audio
   esté restringido para cuentas nuevas por antiabuso, justamente porque es el paso que permite
   bajarse el audio.
2. **Spotify dejó de servir esta ruta a clientes no oficiales**, recientemente. PSPotify-ME se
   mergeó el 11-oct-2026 diciendo que le funciona, y su propio README advierte que usa servicios
   privados que "pueden cambiar o bloquear en cualquier momento".

No puedo distinguirlas desde acá, y las dos están fuera del código.

## El test que lo resuelve, y que necesita a Emilio

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

Lo que dice cada resultado:

- **librespot suena** → el problema es mío y sigo buscando con una referencia que funciona al lado.
- **librespot también falla en la clave de audio** → no es el port. Es la cuenta nueva o es
  Spotify, y entonces la salida es esperar unos días con la cuenta en uso normal, o probar con
  una cuenta Premium más vieja.

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
