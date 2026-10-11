# audio3ds en modo nativo

El mismo cliente, con la misma interfaz, leyendo de Spotify en vez del puente.

## Cómo se activa

En `SD:/3ds/audio3ds/server.txt`, la **primera línea** decide el modo:

```
native
```

Cualquier otra cosa se interpreta como antes, `host:puerto` del puente:

```
192.168.100.20:8080
22050
```

Se eligió así para no romper nada, y tiene una propiedad útil: **agregá `native`
como primera línea encima de lo que ya tenías** y el resto del archivo queda
intacto, sin usar. Borrás esa línea y volvés al puente con tu configuración de
siempre:

```
native
192.168.100.20:8080
22050
```

## Qué hace falta

- Haber pareado una vez con `probe_pair`, que deja `spotify.cfg` con el refresh token.
- Wi-Fi.

Al arrancar muestra los pasos del inicio de sesión en texto (son uno o dos segundos:
token OAuth, access point, login5) y después pasa a la interfaz normal con la lista abierta.

## Qué se puede hacer

| Acción | Resultado |
|---|---|
| Arranque | la lista abre en tus playlists, con **Me gusta** como primera fila |
| **A** sobre una lista | entra y muestra sus temas |
| **A** sobre un tema | arma una **radio** a partir de él |
| **B** | vuelve a las listas |
| **Y** | teclado del sistema, busca en el catálogo de Spotify |
| **L / R** | pasa página sin volver a pedir nada a la red |

## Qué NO hace

**No reproduce.** La clave de audio está bloqueada por Spotify para esta cuenta
(librespot#1649), y es el único eslabón que falta. La pantalla superior lo dice
en vez de fingir que hay algo sonando.

El modo puente sigue siendo el que suena.

## Detalles de diseño

- Las páginas se recortan de lo ya descargado: **pasar página no cuesta red**. En esta
  consola el coste son los viajes de ida y vuelta.
- Los nombres de los temas se piden en **un solo lote**, no uno por fila.
- La radio viene con los títulos adentro, así que no cuesta ni ese lote.
- Los nombres de las playlists se piden al abrirlas, no al listarlas: hacerlo por fila
  sería una petición por lista para nombres que casi nunca se miran.
- Todo queda registrado en `SD:/native.log`, porque la interfaz tapa la consola de texto
  y sin eso un fallo se vería como una lista vacía y nada más.
