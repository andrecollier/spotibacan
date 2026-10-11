# Reporte — Fase 4: biblioteca y metadata

Fecha: 2026-10-10 · Rama: `develop`

## El hallazgo que define la fase

**spclient rechaza el token de OAuth, y `api.spotify.com` contesta 429 con el client ID de
escritorio.** Ninguno de los dos sirve. El camino que funciona es el de librespot, y obliga a un
orden que no se puede atajar:

```
pairing -> token OAuth -> login al access point -> credenciales reutilizables
        -> login5 -> token Bearer -> spclient
```

Las **credenciales reutilizables que devuelve APWelcome** (195 bytes) son la bisagra entre las
dos mitades de la sesión: login5 las canjea por el Bearer que spclient sí acepta. Eso explica por
qué PSPotify describe la cadena como "AP login, luego login5, luego spclient" — no es
redundancia, es la única ruta.

Dato adicional: login5 **no pidió hashcash** en ninguna corrida, pero el solucionador quedó
implementado porque el servidor puede pedirlo cuando quiera. Es el único lugar del cliente que es
fuerza bruta pura, así que reporta cuántos intentos y cuántos ms costó en lugar de dejarlo a
adivinar.

## Lo que se construyó

- `platform3ds/login5.c` — canje de credenciales por Bearer, con el hashcash de
  `util::solve_hash_cash` portado.
- `platform3ds/spclient.c` — resolución del host de spclient, metadata de pista por el servicio
  `extended-metadata`, y el rootlist.
- `platform3ds/http3ds.c` — generalizado a `http_request()`; de ahí salen el POST de formulario,
  el GET con Bearer y los dos de protobuf. El buffer de cabeceras se dimensiona para el token,
  que pasa de 400 caracteres.
- `platform3ds/ap3ds.c` — `ap_info` ahora **transporta** las credenciales reutilizables, no solo
  su longitud; y nuevos `ap_resolve_list()` / `ap_login_any()`.
- `probe_meta/` — la app que encadena los seis pasos.

## Verificado en la consola emulada

```
2. Access point...
   6 candidates, first ap-guc3.spotify.com:4070
   ap-guc3.spotify.com:4070 (attempt 1), 31xn3ibkyttdciogihibyz6jjuym, 195 bytes
3. login5 token...   443 chars, valid 3540 s
4. spclient host...  guc3-spclient.spotify.com
5. Track metadata
   Mr. Brightside / by The Killers / from Hot Fuss / 3:42
   4 audio files: Vorbis 320, Vorbis 160, Vorbis 96, AAC
   pick for this console: Vorbis 160
6. Rootlist
   length field 0, 0 uris read
```

apresolve devuelve además un host de spclient **regional** (`guc3-spclient.spotify.com`), no el
`spclient.wg.spotify.com` genérico.

## Tres bugs que se cazaron antes de llegar a la consola

1. **El entity request iba en el campo 1 de `BatchedEntityRequest`, que es el `header`.** Va en
   el 2. El servicio contesta **HTTP 200 con cuerpo vacío** cuando la forma está mal, lo que se
   lee como un problema de autorización y no de formato. Costó una vuelta.
2. **El POST de metadata iba sin el Bearer**, aunque la función recibía el token: lo cazó el
   `-Wunused-parameter` del compilador. Mismo síntoma que el anterior — 200 con cuerpo vacío en
   vez de 401 — así que sin el aviso del compilador habría costado otra vuelta.

3. **`duration` se leía como varint plano, y es un `sint32`** — zigzag. Leído mal vuelve al
   **doble**: la primera corrida en la consola mostró 7:24 para "Mr. Brightside", que son 3:42.
   Lo delató el valor, no un error. Y lo importante: **mi referencia en Python tenía el mismo
   bug**, así que el test diferencial no podía cazarlo; solo lo cazó leer el tipo declarado en
   `metadata.proto`. `popularity`, `number` y `disc_number` son iguales. De ahí sale
   `pb_get_sint()`.

Y uno de robustez, que apareció como un `ECONNREFUSED` en el 4070 justo después de que la Fase 3
hubiera funcionado contra ese mismo host: **apresolve devuelve seis access points, en los puertos
4070, 443 y 80, y el cliente usaba solo el primero.** Cualquiera de ellos puede rechazar una
conexión mientras los otros contestan, así que tomar solo el primero era un punto único de falla
gratis. Ahora `ap_login_any()` los prueba en orden — y no reintenta cuando el AP rechazó el
*login*, porque esa respuesta va a ser la misma en todos.

## Verificado desde la Mac, antes de portar

Metadata de una pista conocida, por `extended-metadata` con `TRACK_V4`:

```
  name='Mr. Brightside'  duration=444400 ms  artists=['The Killers']
  audio files: 4
    cf100297a1c010f8bca9667e8d7822c2d35a20dc format=2   OGG_VORBIS_320
    3f090e7f0793eac0c6d562385820c576e9ff7d29 format=1   OGG_VORBIS_160
    191184dcab41508b524b84f43d86ba5527472bc6 format=0   OGG_VORBIS_96
    42a528f2f7db69b9ccabd1cbb533c24695ec8ef8 format=8   AAC_24
```

**Ahí están los `file_id`, que es lo que la Fase 5 necesita**: el audio se pide por file_id, no
por pista. Y `OGG_VORBIS_160` está disponible, que es justo el formato que el 3DS puede decodificar
en software. `sp_pick_file()` prefiere 160, después 96 y recién al final 320: 320 triplica el
caudal sobre el 802.11g del 3DS por una diferencia que sus parlantes no pueden mostrar.

## Lo que hay que decidir con Emilio

**La cuenta no tiene playlists ni Me gusta.** El rootlist vuelve con `length = 0` y cero items, y
el contexto de la colección devuelve una página vacía:

```json
{"metadata": {"context_owner": "spotify:user:31xn..."}, "pages": [{}],
 "uri": "spotify:user:31xn...:collection"}
```

No es un error del código — los endpoints contestan 200 — pero significa que la pantalla de
biblioteca va a estar vacía y no hay con qué probar la navegación. Hace falta saber si la cuenta
está realmente vacía o si estoy usando el endpoint equivocado para Me gusta.

## No verificado

- Sigue sin correr en consola real.
- El contenido de una playlist (`/playlist/v2/playlist/{id}`) no se probó: no hay ninguna.
- `product-info` y `metadata/4` dan 404; no hacen falta, pero quedan como rutas equivocadas.
