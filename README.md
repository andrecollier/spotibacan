# SpotiBacan

Cliente de Spotify para Nintendo 3DS: escucha tu música, tus playlists y tus
Me gusta directo en la consola, con descargas para escuchar sin internet.

![icono](client3ds/assets/icon.png)

## Características

- Reproducción por streaming (Ogg Vorbis decodificado en la consola)
- Playlists, Me gusta, búsqueda y radio
- Descarga de canciones con carátula a la SD (`3ds/spotibacan/music/`)
- Cola de descargas en segundo plano con indicador de progreso
- Funciona con la tapa cerrada (modo audífonos)
- Pantalla superior con 3D estereoscópico y color adaptado al álbum
- Letras de canciones
- Español e inglés
- Reconexión automática de sesión

## Instalación

1. Descarga `SpotiBacan.cia` de la sección **Releases**
2. Instálalo con FBI (escaneando el QR o desde la SD)
3. Al abrir la app, vincula tu cuenta de Spotify con el código en pantalla
   (se necesita Spotify Premium)

## Controles

| Botón | Acción |
|---|---|
| A | Reproducir / abrir |
| B | Volver |
| X | Radio de la canción |
| Y | Buscar |
| L / R | Canción anterior / siguiente |
| SELECT | Manual de controles |
| START | Ajustes |

## Compilar

Requiere el toolchain de `native-3ds` (llvm-mingw + libctru) y mbedtls 3.6.7
en `client3ds/deps/`. Luego:

```
python3 client3ds/build.py
```

Genera el `.3dsx` y el `.cia` en `client3ds/dist/`.

## Aviso

Proyecto no oficial, sin relación con Spotify AB. Requiere una cuenta
Spotify Premium. Uso bajo tu propia responsabilidad.
