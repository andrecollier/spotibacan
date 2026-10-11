# Reporte — Fase 3: sesión con el access point de Spotify

Fecha: 2026-10-10 · Rama: `develop`

## Resultado

**APWelcome en el 3DS.** La consola emulada abre TCP crudo contra
`ap-guc3.spotify.com:4070`, completa el handshake Diffie-Hellman verificando la firma RSA del
servidor, deriva las claves Shannon, se autentica con el token OAuth guardado y recibe
APWelcome con el nombre canónico de la cuenta. Después sostiene la sesión: contesta el Ping con
un Pong y **el AP devuelve PongAck**, lo que prueba que el cifrado y el MAC funcionan en las dos
direcciones con el nonce avanzando. Llega también `CountryCode: EC`, el SecretBlock de 4368
bytes y un MercuryReq.

Esto es lo que a Spotty y al del post de Reddit les faltó, y es lo que destraba las fases de
audio: el AP es quien emite las claves de audio.

```
  tcp 166 ms  handshake 343 ms  login 376 ms
  *** APWelcome ***
  account: 31xn3ibkyttdciogihibyz6jjuym
  account type: 0 (0 = Spotify)
  reusable credentials: 195 bytes (not stored)
```

## Cómo se validó, antes de escribir una línea de C para la consola

El riesgo de esta fase era depurar a ciegas: si el handshake está mal por un byte, el servidor
simplemente **cuelga la conexión sin decir nada**. Así que el orden fue:

1. **Implementé el protocolo entero en Python en la Mac** (`scratchpad`, no se commitea),
   transcrito de librespot, y lo corrí contra un access point real. Dio APWelcome. Eso dejó
   probado el protocolo antes de que el 3DS entrara en escena.
2. **Test diferencial del Shannon en C contra esa referencia.** Coincide bit a bit, incluido el
   caso difícil. Quedó como test repetible del repo: `python3 client3ds/tests/run.py`.
3. Recién entonces, el port a la consola.

Sin Rust en la Mac no se podía comparar contra el crate original, así que la referencia es una
transcripción propia en Python; lo que la hace creíble no es la transcripción sino que **el
servidor de Spotify la aceptó**.

## Lo que se construyó

- `platform3ds/shannon.c` — port a C del crate `shannon` (MIT, Paul Liétar 2016), el mismo con
  el que librespot interopera. Licencia en `licenses/rust-shannon-MIT.txt`. Port deliberadamente
  fiel: donde algo parece raro está reproducido a propósito, porque el servidor implementa las
  mismas rarezas y "arreglar" una rompe la interoperabilidad.
- `platform3ds/proto3ds.c` — el protobuf justo. Los mensajes son pocos, chicos y fijos, y los
  números de campo están escritos literalmente en los `.proto` de Spotify, así que un generador
  y un runtime serían peso muerto. Los escritores reciben el buffer del llamador y **no
  asignan memoria**.
- `platform3ds/ap3ds.c` — la sesión: apresolve, TCP, ClientHello, verificación de la firma,
  secreto compartido, derivación de claves, ClientResponsePlaintext, y la capa de paquetes
  Shannon con login y pings.
- `tests/` — el test de Shannon con vectores commiteados y su generador.

## Detalles que costaron o que valen para después

- **El acumulador es cada byte enviado y recibido durante el handshake, con el framing incluido**
  (611 bytes en la corrida de referencia). Las dos puntas derivan las claves de ahí, así que un
  byte fuera de lugar no da un error: da un servidor que cuelga.
- La firma del servidor **hay que verificarla**. El transporte es TCP plano, así que esa firma
  RSA-SHA1 es lo único que separa al cliente de un hombre en el medio. Si falla, se corta.
- La clave privada DH son 95 bytes aleatorios leídos **little-endian** (`mbedtls_mpi_read_binary_le`).
- `platform` tiene que ser `PLATFORM_LINUX_ARM`. librespot documenta que declararse Android hace
  que **todos** los APs rechacen con `TryAnotherAP`, con credenciales válidas y todo.
- La derivación son cinco bloques HMAC-SHA1 sobre `acumulador || i`, i de 1 a 5: 100 bytes, de
  los que los primeros 20 son la clave del HMAC del challenge y después 32 y 32 para enviar y
  recibir.
- Shannon es cifrado **y** MAC a la vez, y el estado se arrastra dentro de un paquete: la
  secuencia es nonce → encrypt/decrypt → finish, y nada más puede tocar el contexto en el medio.
  El header de 3 bytes deja una palabra a medio llenar que se arrastra a la llamada del cuerpo.
- Un MAC malo no es un paquete dañado: es el stream entero perdido, porque no hay cómo
  recuperar el estado del cifrador.
- El **device id se guarda en su propio archivo** (`device.id`), no en `spotify.cfg`: ese
  archivo tiene el refresh token y se reescribe atómicamente en cada renovación, y el device id
  no tiene por qué compartir ese camino. Si cambiara en cada arranque, la cuenta vería un
  dispositivo nuevo cada vez.

## Un bug que la primera corrida destapó

`ap_recv` no distinguía "no llegó nada en 15 s" de "se cortó la conexión", y la primera corrida
anunció `session ended after 9 packets` con la sesión perfectamente viva. Una sesión inactiva es
lo normal — el AP habla solo cuando tiene algo que decir y puede callarse un minuto entre
pings — así que eso habría roto el bucle del cliente real. Ahora hay `ap_wait()`, que separa
inactividad de conexión rota (y trata `POLLHUP`/`POLLERR` como rotura, no como "hay datos").

## No verificado

- Los tiempos son del emulador: no valen. En hardware el handshake va a costar más, y el
  `mbedtls_mpi_exp_mod` de 768 bits dos veces es el candidato a dominarlo.
- `account_type: 0` significa Spotify, **no** Premium. El AP no reporta el plan acá; que la
  cuenta sea Premium se nota recién al pedir claves de audio, en la Fase 5.
- Sigue sin correr en consola real.

## Cómo probarlo

```bash
python3 client3ds/tests/run.py
python3 client3ds/build.py --app probe_ap
/Applications/Azahar.app/Contents/MacOS/azahar \
    client3ds/dist/3ds/probe_ap/probe_ap.3dsx
```
Requiere haber pareado antes con `probe_pair`.
