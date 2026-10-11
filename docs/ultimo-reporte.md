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
