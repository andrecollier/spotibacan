# Reporte — Fase 2: pairing con la cuenta de Spotify

Fecha: 2026-10-10 · Rama: `develop`

## Resultado

**Fase 2 cerrada y verificada en dos corridas.** La consola emulada pidió un código, Emilio lo
aprobó en spotify.com/pair, el refresh token quedó en la SD y acto seguido se volvió a leer del
disco y se canjeó con éxito. Después se relanzó la app desde cero para probar la ruta de
"arranque posterior": encontró el token guardado y lo canjeó sin pairing. Es la prueba que
importa — no conseguir un token una vez, sino arrancar mañana y seguir teniéndolo.

Permisos concedidos (6, el mínimo; ninguno de escritura):
`streaming`, `playlist-read-private`, `playlist-read-collaborative`, `user-library-read`,
`user-read-email`, `user-read-private`.

Logs: `docs/fase2-pairing-azahar.log` (pairing) y `docs/fase2-reboot-azahar.log` (rearranque).

## El hallazgo que cambió el código

**Spotify rota el refresh token en cada canje.** Las dos corridas devolvieron un token distinto
del guardado. Eso convierte la escritura a la SD en un punto crítico: en el instante en que un
refresh tiene éxito, el token guardado ya está muerto, así que una escritura interrumpida —
tapa cerrada, tarjeta sacada, disco lleno — dejaría la consola sin ningún token usable y
obligaría a parear de nuevo.

Por eso `cfg_save_refresh_token()` escribe a `spotify.cfg.new` y después renombra. FAT no tiene
reemplazo atómico, así que el archivo viejo se borra primero y esa ventana es el precio; es
mucho más chica que una escritura parcial sobre la única copia del token.

## Qué se construyó

- `platform3ds/pair3ds.c` — el device authorization grant (RFC 8628). `pair_begin()`,
  `pair_poll()` y `token_refresh()`. Endpoints y client ID **no adivinados**: salen de
  `oauth/src/lib.rs` y `docs/device-authorization.md` de librespot.
  `POST accounts.spotify.com/oauth2/device/authorize` y `POST /api/token`, sin headers más allá
  del `Content-Type`, sin client secret, sin PKCE y sin cookies.
- `platform3ds/http3ds.c` — HTTP/1.1 sobre `tls3ds`: POST de formulario, GET con Bearer,
  `Content-Length` y chunked, y `recv` en bloques de 4 KB, nunca byte a byte.
- `platform3ds/json3ds.c` — lo justo para objetos planos de strings y números. Todo lo que se
  parsea en esta etapa es un objeto plano, así que un parser de verdad sería peso muerto; el
  header dice explícitamente que no camina objetos anidados, en vez de adivinar.
- `platform3ds/cfg3ds.c` — el token en la SD, con la escritura atómica de arriba.
- `probe_pair/main.c` — muestra el código, consulta respetando el `interval`, guarda, y vuelve a
  leer del disco para cerrar el círculo. **Ningún token se imprime ni se loguea**: solo
  longitudes y los permisos concedidos.

## Detalles que valen para después

- El `client_id` tiene que ser el de escritorio, `65b708073fc0480ea92a077233ca87bd`. De los tres
  que trae librespot es el único habilitado para este flujo; los de Android e iOS lo rechazan y
  **lo reportan como `invalid_scope`, no como `unauthorized_client`**, que apunta al lugar
  equivocado. Spotify valida los scopes antes de la elegibilidad del cliente.
- Un solo scope no reconocido tumba la petición entera, así que no se pueden sondear pidiendo
  extras y viendo qué vuelve.
- Los errores llegan como JSON con HTTP 400, así que un 400 es una respuesta a leer, no un fallo
  de transporte. `authorization_pending` y `slow_down` son estados normales de la espera.
- Una conexión cortada durante la espera **no** es un pairing fallido: el código sigue vivo, así
  que `pair_poll()` devuelve PENDING y se sigue consultando.
- El access token dura 3600 s y se renueva 60 s antes, porque un token que caduca en medio de
  una petición es peor que uno renovado un minuto antes de tiempo.
- Tamaños reales: access token 296 chars, refresh token 131 chars.

## Pendiente / a saber

- **Hay un refresh token real, en texto plano, en la Mac**:
  `~/Library/Application Support/Azahar/sdmc/spotify.cfg`. Es la SD emulada. Para desparear:
  X en la app borra el archivo, y la autorización se revoca del lado de Spotify en
  spotify.com/account/apps.
- No se comprobó si el token viejo muere de inmediato al rotar. Para saberlo habría que
  guardarse el anterior y reintentarlo, y eso implica volver a parear; el código ya asume el
  peor caso, que es lo correcto.
- Sigue sin correr en consola real. Nada de esta fase depende del hardware salvo los tiempos.

## Cómo probarlo

```bash
python3 client3ds/build.py --app probe_pair
/Applications/Azahar.app/Contents/MacOS/azahar \
    client3ds/dist/3ds/probe_pair/probe_pair.3dsx
```
