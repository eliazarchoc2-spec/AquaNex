# Potabilizadora P12 · Simulación y monitoreo en vivo

Página web con dos modos:

- **Simulación**: recrea el prototipo en 3D con Three.js (techo → tanque 1 → EV1 → filtro de sedimentos → carbón activado → tanque 2 → EV2, con electrodo de pH). EV1 abre cuando el US1 mide 80 % y EV2 cuando el US2 mide 80 % con pH apto. Todo corre en el navegador.
- **ESP32 real**: muestra en vivo lo que envía el equipo. El ESP32 manda su estado a `/api/telemetry` cada 5 s, la función lo guarda en Upstash Redis y la página lo lee.

```
web/
  index.html          página (sin build ni dependencias)
  api/telemetry.js    función de Vercel: POST del ESP32 / GET de la página
  package.json
```

## Desplegar en Vercel

1. Sube la carpeta del proyecto a un repositorio de GitHub.
2. En Vercel: **Add New → Project**, importa el repositorio y en **Root Directory** elige `web`. Framework: **Other**. No hace falta un comando de build.
3. En el proyecto: **Storage → Create / Connect → Upstash (Redis)** y conéctalo al proyecto. Vercel crea solas las variables `KV_REST_API_URL` y `KV_REST_API_TOKEN`.
4. En **Settings → Environment Variables** agrega `DEVICE_KEY` con una clave larga inventada (por ejemplo 32 caracteres al azar).
5. Vuelve a desplegar (**Deployments → Redeploy**) para que tome las variables.

Sin el ESP32 la simulación ya funciona. El modo real muestra «Esperando el primer dato del ESP32» hasta que llegue el primer envío.

## Variables de entorno

| Variable | De dónde sale |
|---|---|
| `DEVICE_KEY` | La inventas tú; debe ser igual a `DEVICE_KEY` en `Potabilizadora.ino` |
| `KV_REST_API_URL` | La crea la integración de Upstash (también se acepta `UPSTASH_REDIS_REST_URL`) |
| `KV_REST_API_TOKEN` | La crea la integración de Upstash (también se acepta `UPSTASH_REDIS_REST_TOKEN`) |

## Conectar el ESP32

En `Potabilizadora/Potabilizadora.ino`, bloque **CONFIGURACIÓN**:

```cpp
const bool  USAR_NUBE  = true;
const char* WIFI_SSID  = "TuRed";
const char* WIFI_PASS  = "TuClave";
const char* NUBE_URL   = "https://tu-proyecto.vercel.app/api/telemetry";
const char* DEVICE_KEY = "la-misma-clave-que-en-vercel";
```

Sube el firmware y mira el Monitor Serie: la línea `Nube: OK, ultimo envio hace X s` confirma que llega. `ERROR 401` significa que las claves no coinciden.

## API

| Método | Ruta | Descripción |
|---|---|---|
| `POST` | `/api/telemetry` | Header `x-device-key`. Cuerpo: el JSON de `/api` del firmware. Guarda el último estado y un historial de 300 puntos. |
| `GET` | `/api/telemetry` | `{ latest, ageSec }` |
| `GET` | `/api/telemetry?history=1` | Además, `history` en orden cronológico (para la gráfica). |

## Limitaciones

- El modelo 3D carga Three.js 0.170.0 desde cdn.jsdelivr.net, así que necesita internet y WebGL. Si no carga, la página lo avisa y los sensores, la gráfica y la simulación siguen funcionando.

- El modo real es **solo lectura**: desde internet no se abren ni cierran válvulas. Para operar a mano usa el panel local del ESP32 (WiFi `Potabilizadora`, `http://192.168.4.1`).
- El firmware no valida el certificado HTTPS (`setInsecure()`); alcanza para un prototipo.
- La simulación usa la lógica del 80 %. El firmware conserva sus propios umbrales (ver `LEEME.md`), así que en modo real las válvulas siguen la lógica del firmware.
