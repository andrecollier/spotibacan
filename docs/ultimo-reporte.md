# Reporte — la biblioteca completa y la interfaz enchufada

Fecha: 2026-10-11 · Rama: `develop`

## Paso 1: contenido de playlists y Me gusta — cerrado

Faltaba resolver los URIs que daba el rootlist.

- `GET /playlist/v2/playlist/{id}` devuelve el **mismo `SelectedListContent`** que el rootlist:
  uno guarda URIs de playlist y el otro de pista, así que un solo recorrido sirve para los dos
  y el rootlist pasó a usarlo también.
- Me gusta va por `/context-resolve/v1/spotify:user:{id}:collection`, que contesta JSON con la
  primera página entera. **La página vacía que vimos antes era la biblioteca vacía, no el
  endpoint equivocado**: con la cuenta ya poblada, devuelve las pistas.

Verificado en consola emulada: playlist de 37 temas con su nombre real
("Molotov, Control Machete, Plastilina Mosh y demás incorrectos"), 24 URIs leídos, 8 nombrados
en **un solo viaje** de metadata; y 7 Me gusta nombrados.

## Paso 2: la interfaz, enchufada al cliente nativo

`audio3ds` fue escrito alrededor del puente: pide una lista a un servidor de la LAN y recibe
JSON. El modo nativo ofrece **la misma forma** desde Spotify, así que las pieles, las carátulas,
el teclado del sistema y la lista táctil no cambian ni una línea.

Se activa con una primera línea `native` en `SD:/3ds/audio3ds/server.txt`. Un `server.txt`
existente sigue funcionando igual — era la forma de no romper nada.

Qué se puede hacer, documentado en `docs/modo-nativo.md`:

| Acción | Resultado |
|---|---|
| Arranque | la lista abre en tus playlists, con **Me gusta** de primera fila |
| **A** sobre una lista | entra y muestra sus temas |
| **A** sobre un tema | arma una **radio** a partir de él |
| **B** | vuelve atrás |
| **Y** | teclado del sistema, busca en el catálogo de Spotify |
| **L / R** | pasa página **sin pedir nada a la red** |

**No reproduce**, y la pantalla superior lo dice en vez de fingir. Esa es la Fase 5a, bloqueada.

### Decisiones que valen

- **Pasar página no cuesta red.** La vista se descarga una vez y las páginas se recortan de lo
  que ya está en memoria; sólo se vuelve a pedir si cambia la vista o la consulta. En esta
  consola el coste son los viajes de ida y vuelta.
- **Los nombres se piden en lote.** Una pantalla de resultados cuesta un viaje, no uno por fila.
- **La radio no cuesta ni ese lote**: `radio-apollo` trae los títulos adentro.
- **Los nombres de las playlists se piden al abrirlas**, no al listarlas: hacerlo por fila sería
  una petición por lista para nombres que casi nunca se miran. Mientras tanto la fila dice
  "Lista N", que es feo y honesto.
- **La sesión del access point se cierra** después de login5. Mantenerla abierta sólo serviría
  para pedir claves de audio, que es justo lo que Spotify niega.
- **Hay log en `SD:/native.log`**, porque la interfaz tapa la consola de texto y sin eso un
  fallo de red se vería como una lista vacía y nada más.

### Dos bugs del camino

1. El modo nativo levantaba sin pedir jamás la biblioteca. **`lib_reload` lo consume el hilo de
   comandos del puente**, que en modo nativo no arranca. Ahora hay un hilo propio que lo
   atiende — y sigue estando fuera del bucle de render por la misma razón de siempre: una
   petición de un segundo no puede congelar la interfaz.
2. Las dos funciones de lista usaban anchos de URI distintos (96 y 64). Unificado en 64:
   `spotify:playlist:` más 22 caracteres son 39, así que sobra.

---

# La clave de audio: caso cerrado (11-oct-2026)

Apareció **Spoti3DS** (kxrbystar, [hilo en GBAtemp](https://gbatemp.net/threads/wip-spoti3ds-native-spotify-connect-client-for-3ds.682623/)),
un cliente nativo de Spotify Connect para 3DS que **suena en hardware real**. Eso obligó a
revisar si nuestro método estaba mal. No lo está.

## Qué hace Spoti3DS

Su lista: login5, generación de token, registro como dispositivo Connect, WebSocket al Dealer
por TLS 1.3, obtención del stream desde el CDN, decodificación. **No menciona el access point
ni las claves de audio en ningún momento**, lo que parecía la pista: si no hace el handshake
del AP, conseguiría la clave de otra forma.

No es así. Y lo descarta la investigación del propio issue de librespot.

## Lo que dice librespot#1649, 155 comentarios y abierto desde nov-2025

El comentario más completo es de **JesperNaarttijarvi (26-mar-2026)**, que probó en la **misma
máquina, el mismo código y el mismo flujo de autenticación, cambiando solo la cuenta**:

- Cuenta con nombre de usuario corto (las viejas, personalizadas): funciona **siempre**.
- Cuenta con nombre generado largo (las nuevas): **error de clave en todas las pistas**, tres
  cuentas probadas.
- Probó **el Player directo, sin Spirc** → mismo error. *Eso descarta que el registro como
  dispositivo Connect tenga algo que ver.*
- Probó quitar el usuario de las credenciales guardadas → el servidor rechaza la sesión.
- Probó su propio client ID registrado → sin permiso de streaming. Probó la clave de Android →
  el client ID no lo permite.
- "El pedido de clave de audio ni siquiera contiene el nombre de usuario, así que tiene que ser
  algo de la sesión misma."

Y **ayer mismo** (10-oct-2026), fherrera124: *"The AES issue with new accounts is definitely a
roadblock now, though luckily it hasn't affected my older account yet."*

El nombre de usuario de esta cuenta, `31xn3ibkyttdciogihibyz6jjuym`, tiene **28 caracteres** —
exactamente el patrón de cuenta nueva que describen.

## Conclusión

1. **El código es correcto.** Tres implementaciones independientes —la nuestra, librespot y
   PSPotify— hacen el mismo pedido byte por byte y reciben el mismo rechazo con esta cuenta.
2. **La variable es la cuenta, y es su antigüedad.** Restricción del lado de Spotify, de siete
   meses, sin arreglo, que afecta a todo el ecosistema librespot.
3. **Spoti3DS funciona porque su autor tiene una cuenta vieja.** Lo que sí demuestra, y es
   valioso: una 3DS da de sobra para esto, audio incluido.
4. No hay atajo técnico. Lo intentaron durante siete meses gente con más contexto que nosotros.

Contexto: Spotify publicó **Soloist**, su reemplazo oficial para dispositivos Connect de
terceros. El rumbo del ecosistema es ese.

## Lo único que falta probar

Una cuenta Premium más vieja. El cliente está escrito y verificado hasta la clave; si una
cuenta así funciona, la Fase 5a se cierra en una tarde.

---

# DESTRABADO (11-oct-2026): la cuenta vieja funciona

Emilio pareó una cuenta Premium más antigua y la cadena completa pasó de punta a punta:

```
== audio key, over the access point ==
    <- 0x0d AesKey for seq 0: 16 bytes        <-- la clave
== storage-resolve ==
    result=0 (CDN), 3 url(s)
== CDN download ==
    HTTP 206, 262144 bytes
== decrypt ==
    68 paginas Ogg, separacion tipica 4140 bytes
```

Y el archivo resultante, verificado con ffprobe:

```
codec_name=vorbis
sample_rate=44100
channels=2
format_name=ogg
```

Decodifica sin un solo error. **Es audio de Spotify descifrado por nuestro propio código.**

Queda confirmado lo del issue de librespot: la variable era la antigüedad de la cuenta. La
cuenta nueva (usuario de 28 caracteres) recibe `AesKeyError 0001` siempre; la vieja entrega la
clave a la primera.

## Una corrección importante para el port a C

Antes anoté que el header de Spotify previo al Ogg **no se hardcodea**, que había que buscar el
magic `OggS`. **Eso está mal y rompería el decodificador.**

El archivo descifrado tiene una **página Ogg señuelo en el offset 0, de 167 bytes** (`0xa7`), y
el flujo Vorbis real empieza justo después. Las ocurrencias de `OggS` caen en 0, **167**, 225,
4511... Buscar "el primer OggS" aterriza en el señuelo, que es exactamente lo que hizo mi prueba
y por eso `ffprobe` devolvía "End of file" sobre un archivo que en realidad estaba perfecto.

**La regla correcta: saltar 167 bytes.** Es el `SPOTIFY_OGG_HEADER_END` que librespot tenía como
constante, y existe de verdad.

## Lo que falta, ahora sin bloqueos

1. `ap_request_audio_key()` en C: paquete `0x0c` sobre la sesión Shannon que ya funciona.
2. `sp_storage_resolve()` en C: devuelve hasta 3 URLs firmadas del CDN.
3. Descarga por rangos + AES-128-CTR con mbedTLS (ya habilitado en la config) e IV
   `72 e0 67 fb dd cb cf 77 eb e8 bc 64 3f 63 0d 93`.
4. Saltar 167 bytes y alimentar el decodificador stb_vorbis, medido a 5.37x en hardware.
5. Enchufarlo al modo nativo de `audio3ds`.

Cada pieza está probada por separado. Falta unirlas.

---

# Fase 5a en C: la cadena completa en la consola

Fecha: 2026-10-11 · Rama: `develop`

Con la clave destrabada, las piezas que faltaban se escribieron en C:

| Pieza | Dónde |
|---|---|
| `ap_audio_key()` | `platform3ds/ap3ds.c` — paquete `0x0c` sobre la sesión Shannon |
| `sp_storage_resolve()` | `platform3ds/spclient.c` — hasta 3 URLs firmadas del CDN |
| `http_get_url()` | `platform3ds/http3ds.c` — GET de una URL completa, con tope propio |
| `track_fetch()` | `platform3ds/track.c` — une metadata, clave, CDN y AES-128-CTR |
| `probe_play/` | la app que lo reproduce |

## Decisiones

- **El `gid` de la pista, no el URI.** El pedido de clave quiere los 16 bytes crudos del
  identificador, que es lo mismo que el base62 del URI decodifica. Ahora `sp_track` lo trae.
- **La clave se pide antes de bajar los bytes.** Sin ella la descarga es ruido, y son megabytes.
- **Las tres URLs del CDN se prueban en orden.** Una firma puede haber caducado o su borde estar
  caído; las otras sirven los mismos bytes.
- **El descifrado es en el sitio**, así que el buffer de la respuesta HTTP pasa a ser el del
  track sin copiar nada. Un track de 160 kbps y cuatro minutos son ~5 MB, y copiarlo sería
  regalar 5 MB de los 23 que hay.
- **La sesión del access point se cierra apenas se tiene la clave.** Es lo único para lo que
  sirve.
- **El tope de descarga es explícito** (12 MB): un track que lo supere falla en vez de quedar
  truncado y decodificar a silencio a la mitad.
- **`SPOTIFY_OGG_HEADER` es 167 y está escrito como constante**, con el comentario de por qué no
  se busca el magic `OggS`: el señuelo *es* una página OggS, así que la primera coincidencia es
  la equivocada. Y hay una comprobación explícita de que en el byte 167 aparece `OggS`, que es
  la forma barata de detectar una clave equivocada antes de pasarle basura al decodificador.

## Verificado en consola emulada

```
4. pista, clave y descarga...
   ... metadata
   ... storage-resolve
   ... clave de audio
   ... descarga del CDN
   ... descifrado
   The Killers - Mr. Brightside
   Vorbis 160, 1048576 bytes
   resolve 2205  key 902  download 7002  decrypt 249 ms
5. decodificar y sonar...
   44100 Hz, 2 canales
   ndspInit fallo (emulador sin firmware del DSP)
```

Que stb_vorbis lea la cabecera del stream **descifrado** prueba dos cosas a la vez: la clave es
correcta y el salto de 167 bytes también. Si cualquiera de las dos estuviera mal, no habría
Vorbis que abrir.

Falta oírlo, y eso el emulador no puede: NDSP necesita la firmware del DSP de la consola.

Los tiempos son del emulador y no valen como medida, salvo uno que da una idea útil: **249 ms
para descifrar 1 MB**. A 160 kbps eso es un segundo de CPU por cada ~80 segundos de audio, así
que el AES no va a ser el cuello de botella ni en hardware.

## Dos errores propios en esta tanda

1. **Reporté un arreglo sin verificar el archivo.** La edición de la cabecera `Range` no hizo
   match y la di por aplicada; el `0` suelto siguió malformando todas las peticiones y la app
   moría en el primer pedido. Lo que lo destrabó fue bisecar con `probe_meta`, compilado antes
   del cambio, que corría perfecto con la misma cuenta y el mismo entorno — eso descartó el
   entorno y dejó solo mi código nuevo.
2. **Una conversión `%u` no se puede omitir como una `%s`.** Intercalar `Range` con ternarios
   dentro del formato imprimía un `0` cuando no había rango. Ahora la cabecera se arma en su
   propio buffer.
