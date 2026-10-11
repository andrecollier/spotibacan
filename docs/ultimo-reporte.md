# Reporte — Fase 1: TLS 1.2 nativo en el 3DS

Fecha: 2026-10-10 · Rama: `develop` (creada en este turno; antes solo existía `main`)

## Resultado

**Fase 1 cerrada y verificada corriendo.** El probe se ejecutó en Azahar y habló TLS 1.2 con
los 5 hosts de Spotify: cadena verificada contra nuestras 11 anclas, SNI, reanudación por
session ticket, y un GET HTTPS real a apresolve que devolvió 613 bytes descifrados con la lista
de access points. Log completo en `docs/fase1-probe-azahar.log`.

El dato de diseño que salió de la corrida: **los cinco hosts aceptaron
ECDHE-CHACHA20-POLY1305**, que es justo lo que queremos porque el ARM11 no tiene instrucciones
AES. Las pruebas desde la Mac daban AES-GCM solo porque OpenSSL lo prefiere en su orden; con
nuestra lista de suites, ChaCha20 gana en todos. `i.scdn.co` va por ECDHE-**ECDSA**, los otros
cuatro por ECDHE-**RSA**: las dos familias hacen falta.

## Qué se construyó

### `client3ds/deps.py` — mbedTLS 3.6.7 para ARM11
- Release pineado y verificado por SHA-256 (`a7e8bcbe…f6`); se baja una vez a `deps/`
  (gitignoreado) y nunca se ejecuta nada de ahí: solo se le pasan rutas .c a clang.
- 109 fuentes compiladas con el mismo shim de clang del port de Melee → `libmbedtls.a`.
  Con `--probe` agrega `mbedtls_strerror` y los hooks de debug del handshake.
- Se eligió 3.6 LTS y no 4.x: 4.x tira la API legacy y hace PSA obligatorio, que TLS 1.2 no necesita.

### `client3ds/platform3ds/` — la capa que el 3DS tiene que aportar
- `mbedtls_config_3ds.h`: cliente TLS 1.2 y nada más. Sin TLS 1.3 (en 3.6 arrastra el core PSA
  y Spotify negocia 1.2 igual), sin `MBEDTLS_FS_IO`, sin `MBEDTLS_NET_C`. Incluye ya el
  `MBEDTLS_CIPHER_MODE_CTR` del audio y el `MBEDTLS_DHM_C` del handshake del AP.
  Buffers 16 KB in / 4 KB out (ahorra 12 KB frente al default).
- `plat3ds.c`: los dos hooks que mbedTLS no puede resolver solo.
  `mbedtls_ms_time()` desde `osGetTime()` (mbedTLS solo conoce POSIX y Win32) y
  `mbedtls_hardware_poll()` desde **`PS_GenerateRandomBytes`**, el CSPRNG de hardware de la
  consola. Esto es estrictamente mejor que el PSP, que tuvo que inventar entropía hasheando
  la MAC del Wi-Fi, el RTC y el jitter de los hilos.
- `tls3ds.c`: sockets de libctru + mbedTLS. Lista de suites corta (ChaCha20 primero, porque el
  ARM11 no tiene instrucciones AES), X25519 primero por lo mismo, SNI obligatorio,
  verificación completa de cadena y de hostname, y caché de un session ticket por host para que
  la reconexión se salte el ECDHE. Timeouts por `poll()`: SOC no tiene `SO_RCVTIMEO` usable.
- `gen_ca_bundle.py` + `ca_bundle.c/.pem`: 11 anclas. Las necesarias se descubren recorriendo
  las cadenas reales (el último cert que manda el server nombra su raíz) y se sacan de dos
  stores: el del Mac y el export CCADB de Mozilla, cacheado en `deps/`, porque Apple no trae
  `Certainly Root R1` ni `GlobalSign Root R46`. `--verify` revalida todo contra los hosts vivos.

### `client3ds/probe_tls/main.c` — la app de Fase 1
Handshake contra los 5 hosts, con dns/tcp/handshake en ms, versión, suite y resultado de
verificación; segunda conexión al mismo host para medir el ticket; y un GET HTTPS real a
apresolve para probar la ruta completa. En New 3DS corre la tanda dos veces, a 268 y a 804 MHz
(`osSetSpeedupEnable`), porque en el PSP el reloj fue la diferencia entre 1.3 s y 0.25 s de
handshake. Todo va también a `spotify-tls.log` al lado del .3dsx.

### Andamiaje
- `client3ds/toolchain.py`: descubrimiento del shim compartido entre `build.py` y `deps.py`.
- `build.py` ahora toma `--app` (`audio3ds` | `probe_tls`); `audio3ds` sigue compilando igual
  (257.420 bytes, verificado).

## Verificado

- `libmbedtls-probe.a` compila para `--target=arm-none-eabi -mcpu=mpcore` (109/109 fuentes).
- `probe_tls.3dsx` enlaza y empaqueta: 394.896 bytes.
- `audio3ds.3dsx` sigue construyéndose sin cambios de comportamiento.
- Bundle contra hosts vivos, forzando TLS 1.2 (`gen_ca_bundle.py --verify`): **7/7 ok**.
  Todos ofrecen TLS 1.2; `login5`, `accounts`, `apresolve`, `spclient` y `audio-fa` eligen
  ECDHE-**RSA**-AES128-GCM-SHA256, mientras `i.scdn.co` y el CDN de Akamai eligen ECDHE-**ECDSA**.
  Las dos familias están en la lista de suites.
- `Certainly Root R1` cruzado entre dos fuentes independientes, mismo SHA-256
  `77:B8:2C:D8:…:D3:A0` (copia de PSPotify y export de Mozilla).

## Corrida en Azahar

Azahar implementa `SOC_U` sobre sockets del host, así que la red es real: el handshake, la
verificación de cadena y el descifrado son los de verdad. Resultado, 5/5 hosts:

```
trust anchors: 11
apresolve.spotify.com    TLSv1.2 TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256   verify ok
login5.spotify.com       TLSv1.2 TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256   verify ok
accounts.spotify.com     TLSv1.2 TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256   verify ok
spclient.wg.spotify.com  TLSv1.2 TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256   verify ok
i.scdn.co                TLSv1.2 TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256 verify ok

-- apresolve over HTTPS --
  613 bytes, ap: ap-guc3.spotify.com:4070","ap-guc3.spotify.com:443","ap-guc3
```

Cómo se lee la salida sin cámara: el probe escribe a `stdout` (pantalla), a `stderr`
(`svcOutputDebugString`, que el emulador registra) y al log. En Azahar el log queda en
`~/Library/Application Support/Azahar/sdmc/spotify-tls.log` — de ahí el fallback a la raíz de
la tarjeta cuando `argv[0]` es una ruta del host que el guest no puede abrir.

## No verificado

- **Los tiempos del log no valen.** El emulador corre sobre la CPU de la Mac y Azahar deja
  `ConfigureNew3DSCPU` sin implementar, así que las dos tandas (268 y 804 MHz) salen casi
  iguales — prueba de que las etiquetas no significan nada ahí. Los milisegundos reales y el
  consumo de RAM siguen pendientes de una corrida en la consola; no bloquean la Fase 2.
  Lo único que el emulador sí insinúa es la proporción del ticket: ~850 ms → ~320 ms.
- `ap-guc3.spotify.com:443` no contesta desde esta red (sí el 4070 que anuncia apresolve).
  A anotar para la Fase 3; no afecta a la Fase 1.

## Cómo probarlo

```bash
python3 client3ds/deps.py --probe && python3 client3ds/build.py --app probe_tls
```
En Azahar, sin tocar la SD:
```bash
/Applications/Azahar.app/Contents/MacOS/azahar \
    client3ds/dist/3ds/probe_tls/probe_tls.3dsx
```
En la consola: copiar `client3ds/dist/3ds/` a la raíz de la SD y lanzar `probe_tls` desde el
Homebrew Launcher con el Wi-Fi encendido. Deja `spotify-tls.log` en `SD:/3ds/probe_tls/`.
