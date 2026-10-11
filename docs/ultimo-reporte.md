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
