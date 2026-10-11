# Reporte — Fase 1: TLS 1.2 nativo en el 3DS

Fecha: 2026-10-10 · Rama: `develop` (creada en este turno; antes solo existía `main`)

## Resultado

TLS 1.2 contra Spotify funciona desde el 3DS en lo que se puede verificar sin consola:
mbedTLS 3.6.7 compila para ARM11, el probe enlaza y empaqueta en un .3dsx de 395 KB, y el
bundle de anclas valida las 7 cadenas reales de Spotify. **Falta correrlo en hardware.**

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

## No verificado

- **Nada corrió en consola.** No hay 3DS ni emulador en esta Mac, así que el handshake real,
  los tiempos y el consumo de RAM siguen sin medir. Es lo único que cierra la Fase 1.
- `ap-guc3.spotify.com:443` no contesta desde esta red (sí el 4070 que anuncia apresolve).
  A anotar para la Fase 3; no afecta a la Fase 1.

## Cómo probarlo

```bash
python3 client3ds/deps.py --probe && python3 client3ds/build.py --app probe_tls
```
Copiar `client3ds/dist/3ds/` a la raíz de la SD y lanzar `probe_tls` desde el Homebrew Launcher
con el Wi-Fi encendido. Deja `spotify-tls.log` en `SD:/3ds/probe_tls/`.
