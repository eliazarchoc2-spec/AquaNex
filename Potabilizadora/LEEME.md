# Potabilizadora de agua lluvia solar · P12 — Firmware

## 1. Preparar Arduino IDE
1. **Archivo → Preferencias → URLs adicionales del gestor de placas**:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. **Herramientas → Placa → Gestor de placas**: instala **esp32 de Espressif Systems** (versión 3.x).
3. Abre `Potabilizadora.ino` (la carpeta se tiene que llamar igual que el archivo).
4. **Herramientas**:
   - Placa: **ESP32S3 Dev Module**
   - USB CDC On Boot: **Disabled** (porque programas por el USB-C marcado **COM/UART**)
   - Puerto: el COM que aparece al conectar la placa
5. Subir (→). Si no conecta, mantén presionado **BOOT**, presiona y suelta **RST** y luego suelta **BOOT**.
6. Abre el **Monitor Serie** a **115200** baudios con el fin de línea en **Nueva línea**.

No necesitas instalar librerías: todo viene incluido con el core del ESP32.

## 2. Ajustar a tus tinacos
Al inicio del código, en el bloque **CONFIGURACIÓN**:

| Constante | Qué es |
|---|---|
| `T1_DIST_VACIO_CM` / `T2_…` | Distancia del sensor al **fondo** del tinaco |
| `T1_DIST_LLENO_CM` / `T2_…` | Distancia del sensor al agua con el tinaco **lleno** (mínimo ~20 cm) |
| `T1_CAPACIDAD_L` / `T2_…` | Litros del tinaco (solo se usan para mostrarlos) |
| `EV1_…`, `EV2_…` | Porcentajes de nivel para abrir y cerrar cada válvula |
| `PH_MIN`, `PH_MAX` | Rango de pH aceptable (por defecto 6,5–8,5) |

**Para medir rápido:** con el tinaco vacío, sube el código y anota la distancia que muestra el Monitor Serie. Ese valor es `DIST_VACIO`.

## 3. Cómo funciona (modo automático)
- **EV1 (tanque 1 → filtro → tanque 2)** se abre cuando el tanque 1 tiene ≥ 20 % y el tanque 2 tiene ≤ 85 %. Se cierra cuando el tanque 1 baja a 10 % o el tanque 2 llega a 95 %.
- **EV2 (salida del tanque 2)** se abre cuando el tanque 2 tiene ≥ 30 % **y** el pH está entre 6,5 y 8,5. Se cierra cuando el tanque 2 baja a 15 % o el pH se sale del rango.
- **Seguridades:** si falla un sensor, las válvulas que dependen de él se cierran. Si la batería baja de 11,0 V se cierran las dos, y vuelven a funcionar cuando la batería sube de 11,8 V. Si no hay batería conectada (pruebas solo con USB), este corte no se aplica.
- Una válvula no se vuelve a abrir antes de 5 s desde su último cambio.

## 4. Panel web
1. Conecta el celular a la red WiFi **Potabilizadora** (clave **agua1234**).
2. Abre **http://192.168.4.1**.
3. Verás los niveles, el pH, la batería y las válvulas. Con **Manual** puedes abrir o cerrar las válvulas a mano, por ejemplo para una demostración.

## 5. Calibrar el sensor de pH (por el Monitor Serie)
1. Antes que nada, ajusta el offset del módulo. Pon en corto el BNC (el centro con la carcasa) y gira el potenciómetro que está junto al BNC hasta que el Monitor Serie marque **Po = 2,500 V**.
2. Mete el electrodo en el **buffer pH 7**, espera 1–2 minutos a que el valor se estabilice y escribe `cal7`.
3. Enjuaga con agua destilada, mételo en el **buffer pH 4**, espera de nuevo y escribe `cal4` (o `cal10` si usas el buffer pH 10).
4. La calibración se guarda en la memoria del ESP32 y no se borra al apagarlo. Para volver a los valores de fábrica escribe `calreset`.

## 6. Comandos del Monitor Serie
`estado` · `auto` · `manual` · `ev1 on` / `ev1 off` · `ev2 on` / `ev2 off` · `cal7` · `cal4` · `cal10` · `calreset` · `ayuda`

## 7. Conexión del sensor de pH
El firmware lee el **único** sensor de pH en **IO3** (módulo U6, "después del filtro", tanque 2). El módulo U5 / IO2 del esquemático no se usa: no hace falta armarlo.

## 8. Monitoreo en la nube (opcional)
Para ver el equipo desde internet en la página desplegada en Vercel (carpeta `web/`, ver `web/README.md`):

1. En el bloque **CONFIGURACIÓN** llena `WIFI_SSID` y `WIFI_PASS` (una red con internet, de 2,4 GHz), `NUBE_URL` (`https://tu-proyecto.vercel.app/api/telemetry`) y `DEVICE_KEY` (la misma clave que pusiste en Vercel).
2. Cambia `USAR_NUBE` a `true` y sube el código.
3. El Monitor Serie muestra `Nube: OK, ultimo envio hace X s` cuando llegan los datos.

- El WiFi propio **Potabilizadora** y su panel en `http://192.168.4.1` siguen funcionando. Al conectarse al router, el ESP32 cambia el canal de su red, y el celular puede desconectarse un momento.
- El envío corre en una tarea aparte: si no hay internet, el control de válvulas sigue igual.
- Desde la página en la nube solo se **ven** los datos; las válvulas se operan desde el panel local.
