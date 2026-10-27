# Potabilizadora P12 · Simulación 3D

Página web que reproduce el prototipo de potabilizadora de agua lluvia y muestra, en tiempo real, lo que miden sus sensores.

- **Modelo 3D (Three.js)**: techo → tanque 1 → EV1 → filtro de sedimentos → carbón activado → tanque 2 → EV2, con el electrodo de pH en el tanque 2.
- **Sensores**: nivel de cada tanque (ultrasónicos US1 y US2), acidez del agua (pH) y estado de las dos electroválvulas, cada uno con su explicación en lenguaje claro.
- **Gráfica**: niveles y pH de los últimos 10 minutos simulados.
- **Registro de eventos**: qué cambió, cuándo y por qué.
- **Controles**: lluvia, pH del agua de lluvia, consumo, velocidad de simulación, modo automático/manual, umbrales de disparo y fallas inyectadas (sensor sin eco, filtro saturado, lluvia ácida).

Todo corre en el navegador. No hay backend ni base de datos.

```
web/
  index.html   página completa (sin build ni dependencias)
  package.json
```

## Cómo funciona el control

- **EV1** (tanque 1 → filtros → tanque 2) abre cuando el tanque 1 llega al 80 %; cierra cuando el tanque 1 baja al 20 % o el tanque 2 se llena.
- **EV2** (salida de agua apta) abre cuando el tanque 2 llega al 80 % **y** el pH está entre 6,5 y 8,5; cierra si el pH se sale del rango o el tanque 2 baja al 20 %.
- El carbón activado arrastra el pH hacia 7,2, así que el agua ácida o alcalina se corrige al pasar por el filtro.

## Ver la página en local

Desde la carpeta `web`:

```
python -m http.server 8080
```

Y abrir `http://127.0.0.1:8080/`. También funciona abriendo `index.html` directamente, salvo el modelo 3D, que necesita servidor propio para los módulos de Three.js.

## Desplegar en Vercel

1. Sube el repositorio a GitHub.
2. En Vercel: **Add New → Project** e importa el repositorio. Framework: **Other**. No hace falta comando de build ni variables de entorno.
3. Deploy.

El `vercel.json` de la raíz del repositorio hace que la dirección principal (`/`) muestre `web/index.html`, así que no hace falta cambiar el **Root Directory**. Si igual lo pones en `web`, también funciona.

## Limitaciones

- Three.js 0.170.0 se carga desde `cdn.jsdelivr.net`, así que el 3D necesita internet y WebGL. Si no carga, la página lo avisa y los sensores, la gráfica y el registro siguen funcionando.
- La simulación usa sus propios umbrales (los del panel de control), no los del firmware del prototipo físico.
