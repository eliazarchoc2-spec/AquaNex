/*
  ==========================================================================
   Potabilizadora de agua lluvia solar · P12
   Firmware para ESP32-S3-DevKitC-1  (esquemático Rev. C)
  ==========================================================================
   - 2 tinacos con sensor ultrasónico HC-SR04 (nivel de agua)
   - EV1: tanque 1 -> filtro -> tanque 2   (relé K1, IO15)
   - EV2: salida del tanque 2 (agua lista)  (relé K2, IO16)
   - 1 sensor de pH PH-4502C en el tanque 2 (agua filtrada, IO3)
   - Medición de batería 12 V (IO1)
   - Panel web por WiFi (punto de acceso propio) + Monitor Serie
   - Envío opcional del estado a la nube (Vercel) para verlo desde internet

   Placa en Arduino IDE: "ESP32S3 Dev Module"  (core esp32 de Espressif >= 3.0)
   Monitor Serie: 115200 baudios, fin de línea "Nueva línea".
   Escribe "ayuda" en el Monitor Serie para ver los comandos.
  ==========================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ==========================================================================
//  CONFIGURACIÓN — AJUSTA ESTOS VALORES A TUS TINACOS
// ==========================================================================

// Distancias medidas desde la cara del sensor ultrasónico (en cm):
//   DIST_VACIO = del sensor al FONDO del tinaco (tinaco vacío)
//   DIST_LLENO = del sensor a la superficie del agua con el tinaco LLENO
//   (el HC-SR04 no mide bien a menos de ~20 cm; deja ese espacio libre)
const float T1_DIST_VACIO_CM = 100.0;
const float T1_DIST_LLENO_CM = 20.0;
const float T1_CAPACIDAD_L   = 200.0;   // litros con el tinaco lleno (solo para mostrar)

const float T2_DIST_VACIO_CM = 100.0;
const float T2_DIST_LLENO_CM = 20.0;
const float T2_CAPACIDAD_L   = 200.0;

// Lógica de EV1 (tanque 1 -> filtro -> tanque 2), en % de nivel
const float EV1_ABRIR_T1_MIN  = 20.0;   // abre si el tanque 1 tiene al menos esto...
const float EV1_ABRIR_T2_MAX  = 85.0;   // ...y el tanque 2 tiene como máximo esto
const float EV1_CERRAR_T1     = 10.0;   // cierra si el tanque 1 baja a esto
const float EV1_CERRAR_T2     = 95.0;   // cierra si el tanque 2 llega a esto (lleno)

// Lógica de EV2 (salida del tanque 2), en % de nivel
const float EV2_ABRIR_T2_MIN  = 30.0;   // abre si el tanque 2 tiene al menos esto y el pH es bueno
const float EV2_CERRAR_T2     = 15.0;   // cierra si el tanque 2 baja a esto

// Rango de pH aceptable para dejar salir el agua
const float PH_MIN  = 6.5;
const float PH_MAX  = 8.5;
const float PH_HIST = 0.1;              // margen para que la válvula no "tiemble" en el límite

// Protección de batería (las válvulas consumen ~0,57 A cada una)
const bool  USAR_CORTE_BATERIA   = true;
const float BAT_CORTE_V          = 11.0; // debajo de esto se cierran las válvulas
const float BAT_REARME_V         = 11.8; // vuelve a permitir abrir arriba de esto
const float BAT_MIN_PRESENTE_V   = 5.0;  // menos que esto = sin batería (pruebas por USB): no se corta

// Tiempo mínimo entre aperturas/cierres de una misma válvula (evita golpeteo)
const unsigned long MIN_ENTRE_CAMBIOS_MS = 5000;

// Red WiFi que crea el ESP32 (la contraseña debe tener 8 caracteres o más)
const char* AP_SSID = "Potabilizadora";
const char* AP_PASS = "agua1234";

// Monitoreo en la nube (página en Vercel). Pon USAR_NUBE en true después de llenar estos datos.
// El ESP32 se conecta además a una red WiFi con internet y envía su estado cada NUBE_CADA_MS.
const bool  USAR_NUBE  = false;
const char* WIFI_SSID  = "TU_RED_WIFI";      // red con internet (solo 2,4 GHz)
const char* WIFI_PASS  = "TU_CLAVE_WIFI";
const char* NUBE_URL   = "https://TU-PROYECTO.vercel.app/api/telemetry";
const char* DEVICE_KEY = "cambia-esta-clave-larga";   // la misma que DEVICE_KEY en Vercel
const unsigned long NUBE_CADA_MS = 5000;

// ==========================================================================
//  PINES (según el esquemático) — normalmente no hay que cambiarlos
// ==========================================================================
const uint8_t PIN_TRIG1 = 4;
const uint8_t PIN_ECHO1 = 5;
const uint8_t PIN_TRIG2 = 6;
const uint8_t PIN_ECHO2 = 7;
const uint8_t PIN_RELE_EV1 = 15;
const uint8_t PIN_RELE_EV2 = 16;
const uint8_t PIN_PH   = 3;   // U6 · PH2 · IO3/ADC1_2 (módulo "después del filtro")
const uint8_t PIN_VBAT = 1;   // IO1/ADC1_0

const uint8_t RELE_ACTIVO   = HIGH;  // módulo de relés con jumper en H (disparo en ALTO)
const uint8_t RELE_INACTIVO = LOW;

// Divisores de voltaje del esquemático
const float FACTOR_PH   = 2.0;                    // R5/R6 = 10k/10k
const float FACTOR_VBAT = (120.0 + 22.0) / 22.0;  // R7/R8 = 120k/22k  (≈ 6,45)

// Calibración de pH por defecto (se reemplaza con los comandos cal7 / cal4 / cal10)
const float PH_V7_DEFECTO        = 2.50;  // voltaje en Po con pH 7
const float PH_PENDIENTE_DEFECTO = 0.18;  // voltios por unidad de pH (sube el voltaje al bajar el pH)

// ==========================================================================
//  ESTADO DEL SISTEMA
// ==========================================================================
const uint8_t N_MUESTRAS_US = 5;

struct Tanque {
  const char* nombre;
  uint8_t trig, echo;
  float distVacio, distLleno, capacidadL;
  float muestras[N_MUESTRAS_US];
  uint8_t idx;
  uint8_t fallasSeguidas;
  bool listo;      // ya tiene al menos una medición válida
  bool falla;      // sensor sin respuesta
  float distCm;
  float nivelPct;
  float litros;
};

Tanque t1 = {"Tanque 1", PIN_TRIG1, PIN_ECHO1, T1_DIST_VACIO_CM, T1_DIST_LLENO_CM, T1_CAPACIDAD_L,
             {NAN, NAN, NAN, NAN, NAN}, 0, 0, false, false, NAN, 0, 0};
Tanque t2 = {"Tanque 2", PIN_TRIG2, PIN_ECHO2, T2_DIST_VACIO_CM, T2_DIST_LLENO_CM, T2_CAPACIDAD_L,
             {NAN, NAN, NAN, NAN, NAN}, 0, 0, false, false, NAN, 0, 0};

// pH
float phV7 = PH_V7_DEFECTO;
float phPendiente = PH_PENDIENTE_DEFECTO;
float phVolt = NAN;     // voltaje en Po (ya corregido por el divisor), filtrado
float phValor = NAN;
bool  phSenal = false;   // hay voltaje razonable en Po (módulo conectado)
bool  phValido = false;
bool  phOk = false;     // dentro del rango aceptable (con histéresis)

// Batería
float vBat = NAN;
bool  batPresente = false;
bool  batBaja = false;

// Válvulas
struct Valvula {
  const char* nombre;
  uint8_t pin;
  bool abierta;
  unsigned long ultimoCambio;
  const char* motivo;
  bool manual;     // estado pedido en modo manual
};
Valvula ev1 = {"EV1", PIN_RELE_EV1, false, 0, "Iniciando", false};
Valvula ev2 = {"EV2", PIN_RELE_EV2, false, 0, "Iniciando", false};

bool modoManual = false;
bool sistemaListo = false;

Preferences prefs;
WebServer server(80);

// Nube: loop() deja una copia del JSON y la tarea "nube" (núcleo 0) la envía,
// así un POST HTTPS lento nunca frena el control de las válvulas.
SemaphoreHandle_t nubeMutex = nullptr;
char nubeBuffer[768];
bool nubePendiente = false;
volatile int nubeUltimoCodigo = 0;          // último código HTTP (<0 = error de conexión, 0 = sin envíos)
volatile unsigned long nubeUltimoOkMs = 0;

// ==========================================================================
//  UTILIDADES
// ==========================================================================
void ordenar(float* a, int n) {
  for (int i = 1; i < n; i++) {
    float x = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
    a[j + 1] = x;
  }
}

// Lee un pin analógico varias veces y promedia la mitad central (quita picos). Devuelve voltios.
float leerVoltiosFiltrado(uint8_t pin, int n) {
  float v[32];
  if (n > 32) n = 32;
  for (int i = 0; i < n; i++) {
    v[i] = analogReadMilliVolts(pin) / 1000.0f;
    delayMicroseconds(200);
  }
  ordenar(v, n);
  int desde = n / 4, hasta = n - n / 4;
  float suma = 0;
  for (int i = desde; i < hasta; i++) suma += v[i];
  return suma / (hasta - desde);
}

const char* txtValvula(const Valvula& v) { return v.abierta ? "ABIERTA" : "CERRADA"; }

// ==========================================================================
//  SENSORES ULTRASÓNICOS
// ==========================================================================
float medirDistanciaCm(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(3);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long us = pulseIn(echo, HIGH, 30000UL);  // máx. ~5 m
  if (us == 0) return NAN;
  float d = us * 0.0343f / 2.0f;
  if (d < 2.0f || d > 400.0f) return NAN;
  return d;
}

void actualizarTanque(Tanque& t) {
  float d = medirDistanciaCm(t.trig, t.echo);
  t.muestras[t.idx] = d;
  t.idx = (t.idx + 1) % N_MUESTRAS_US;

  if (isnan(d)) {
    if (t.fallasSeguidas < 255) t.fallasSeguidas++;
  } else {
    t.fallasSeguidas = 0;
  }
  t.falla = (t.fallasSeguidas >= N_MUESTRAS_US);

  // Mediana de las muestras válidas
  float v[N_MUESTRAS_US];
  int n = 0;
  for (int i = 0; i < N_MUESTRAS_US; i++)
    if (!isnan(t.muestras[i])) v[n++] = t.muestras[i];
  if (n < 3) return;   // pocas lecturas buenas: se conserva el último valor
  ordenar(v, n);
  t.distCm = v[n / 2];

  float pct = (t.distVacio - t.distCm) * 100.0f / (t.distVacio - t.distLleno);
  t.nivelPct = constrain(pct, 0.0f, 100.0f);
  t.litros = t.capacidadL * t.nivelPct / 100.0f;
  t.listo = true;
}

// ==========================================================================
//  pH
// ==========================================================================
float calcularPh(float volt) { return 7.0f + (phV7 - volt) / phPendiente; }

void actualizarPh() {
  float v = leerVoltiosFiltrado(PIN_PH, 24) * FACTOR_PH;
  bool habiaSenal = phSenal;

  // Si el módulo está desconectado el divisor lleva el pin a ~0 V
  phSenal = (v >= 0.10f && v <= 5.20f);
  if (!phSenal) {
    phValido = false;
    phOk = false;
    phVolt = v;
    phValor = NAN;
    return;
  }

  phVolt = habiaSenal ? phVolt * 0.8f + v * 0.2f : v;  // suavizado
  float ph = calcularPh(phVolt);
  phValido = (ph >= 0.0f && ph <= 14.0f);
  phValor = ph;

  if (!phValido) { phOk = false; return; }
  if (phOk) {
    if (ph < PH_MIN - PH_HIST || ph > PH_MAX + PH_HIST) phOk = false;
  } else {
    if (ph >= PH_MIN && ph <= PH_MAX) phOk = true;
  }
}

void cargarCalibracion() {
  prefs.begin("potab", true);
  phV7 = prefs.getFloat("v7", PH_V7_DEFECTO);
  phPendiente = prefs.getFloat("pend", PH_PENDIENTE_DEFECTO);
  prefs.end();
  if (fabsf(phPendiente) < 0.03f || fabsf(phPendiente) > 0.50f) phPendiente = PH_PENDIENTE_DEFECTO;
}

void guardarCalibracion() {
  prefs.begin("potab", false);
  prefs.putFloat("v7", phV7);
  prefs.putFloat("pend", phPendiente);
  prefs.end();
}

// Calibración con buffer de pH 7: guarda el voltaje actual como punto neutro
String calibrarPh7() {
  if (!phSenal) return "ERROR: no hay lectura valida del sensor de pH";
  phV7 = phVolt;
  guardarCalibracion();
  return "OK: pH 7 = " + String(phV7, 3) + " V guardado";
}

// Calibración con un segundo buffer (4 o 10): calcula la pendiente
String calibrarPhPunto(float phBuffer) {
  if (!phSenal) return "ERROR: no hay lectura valida del sensor de pH";
  float pend = (phV7 - phVolt) / (phBuffer - 7.0f);
  if (fabsf(pend) < 0.03f || fabsf(pend) > 0.50f)
    return "ERROR: pendiente " + String(pend, 4) + " V/pH fuera de rango. Calibra primero pH 7 y espera a que se estabilice.";
  phPendiente = pend;
  guardarCalibracion();
  return "OK: pendiente = " + String(phPendiente, 4) + " V/pH guardada";
}

String borrarCalibracion() {
  phV7 = PH_V7_DEFECTO;
  phPendiente = PH_PENDIENTE_DEFECTO;
  guardarCalibracion();
  return "OK: calibracion de pH restablecida a valores por defecto";
}

// ==========================================================================
//  BATERÍA
// ==========================================================================
void actualizarBateria() {
  float v = leerVoltiosFiltrado(PIN_VBAT, 16) * FACTOR_VBAT;
  vBat = isnan(vBat) ? v : vBat * 0.8f + v * 0.2f;
  batPresente = (vBat >= BAT_MIN_PRESENTE_V);
  if (!batPresente) { batBaja = false; return; }
  if (batBaja) { if (vBat >= BAT_REARME_V) batBaja = false; }
  else         { if (vBat <= BAT_CORTE_V)  batBaja = true;  }
}

// ==========================================================================
//  VÁLVULAS Y LÓGICA DE CONTROL
// ==========================================================================
// forzar = ignora el tiempo mínimo entre cambios (cierres de seguridad y modo manual)
void ponerValvula(Valvula& v, bool abrir, const char* motivo, bool forzar) {
  v.motivo = motivo;
  if (v.abierta == abrir) return;
  if (abrir && !forzar && (millis() - v.ultimoCambio < MIN_ENTRE_CAMBIOS_MS)) return;
  digitalWrite(v.pin, abrir ? RELE_ACTIVO : RELE_INACTIVO);
  v.abierta = abrir;
  v.ultimoCambio = millis();
  Serial.printf(">> %s %s (%s)\n", v.nombre, abrir ? "ABIERTA" : "CERRADA", motivo);
}

// Un tanque sin medición confiable cuenta como falla
bool sensorMal(const Tanque& t) { return t.falla || !t.listo; }

void controlar() {
  if (!sistemaListo) {
    ponerValvula(ev1, false, "Iniciando sensores", true);
    ponerValvula(ev2, false, "Iniciando sensores", true);
    return;
  }

  // Seguridad de batería: aplica también en modo manual
  if (USAR_CORTE_BATERIA && batPresente && batBaja) {
    ponerValvula(ev1, false, "Bateria baja", true);
    ponerValvula(ev2, false, "Bateria baja", true);
    ev1.manual = ev2.manual = false;
    return;
  }

  if (modoManual) {
    ponerValvula(ev1, ev1.manual, "Modo manual", true);
    ponerValvula(ev2, ev2.manual, "Modo manual", true);
    return;
  }

  // ---- EV1: tanque 1 -> filtro -> tanque 2 ----
  if (sensorMal(t1))      ponerValvula(ev1, false, "Falla sensor tanque 1", true);
  else if (sensorMal(t2)) ponerValvula(ev1, false, "Falla sensor tanque 2", true);
  else if (ev1.abierta) {
    if (t1.nivelPct <= EV1_CERRAR_T1)      ponerValvula(ev1, false, "Tanque 1 sin agua", true);
    else if (t2.nivelPct >= EV1_CERRAR_T2) ponerValvula(ev1, false, "Tanque 2 lleno", true);
    else                                   ponerValvula(ev1, true,  "Transfiriendo agua", false);
  } else {
    if (t1.nivelPct < EV1_ABRIR_T1_MIN)      ponerValvula(ev1, false, "Esperando agua en tanque 1", false);
    else if (t2.nivelPct > EV1_ABRIR_T2_MAX) ponerValvula(ev1, false, "Tanque 2 lleno", false);
    else                                     ponerValvula(ev1, true,  "Transfiriendo agua", false);
  }

  // ---- EV2: salida del tanque 2 ----
  if (sensorMal(t2))   ponerValvula(ev2, false, "Falla sensor tanque 2", true);
  else if (!phValido)  ponerValvula(ev2, false, "Sensor de pH sin lectura", true);
  else if (!phOk)      ponerValvula(ev2, false, "pH fuera de rango", true);
  else if (ev2.abierta) {
    if (t2.nivelPct <= EV2_CERRAR_T2) ponerValvula(ev2, false, "Tanque 2 casi vacio", true);
    else                              ponerValvula(ev2, true,  "Entregando agua", false);
  } else {
    if (t2.nivelPct < EV2_ABRIR_T2_MIN) ponerValvula(ev2, false, "Esperando nivel en tanque 2", false);
    else                                ponerValvula(ev2, true,  "Entregando agua", false);
  }
}

void ponerModoManual(bool manual) {
  if (manual && !modoManual) {   // al entrar, conserva el estado actual
    ev1.manual = ev1.abierta;
    ev2.manual = ev2.abierta;
  }
  modoManual = manual;
  Serial.printf(">> Modo %s\n", manual ? "MANUAL" : "AUTOMATICO");
}

// ==========================================================================
//  REPORTES
// ==========================================================================
void imprimirEstado() {
  Serial.println(F("------------------------------------------------------------"));
  for (Tanque* t : {&t1, &t2}) {
    if (!t->listo) Serial.printf("%s: sin lectura todavia%s\n", t->nombre, t->falla ? "  [FALLA SENSOR]" : "");
    else Serial.printf("%s: %6.1f cm  nivel %5.1f %%  ~%5.0f L%s\n", t->nombre, t->distCm, t->nivelPct,
                       t->litros, t->falla ? "  [FALLA SENSOR]" : "");
  }
  if (phValido) Serial.printf("pH: %.2f  (Po = %.3f V)  %s\n", phValor, phVolt, phOk ? "OK" : "FUERA DE RANGO");
  else          Serial.printf("pH: sin lectura valida (Po = %.3f V) - revisa el modulo\n", isnan(phVolt) ? 0.0f : phVolt);
  if (batPresente) Serial.printf("Bateria: %.2f V%s\n", vBat, batBaja ? "  [BAJA]" : "");
  else             Serial.printf("Bateria: no detectada (%.2f V) - modo prueba por USB\n", isnan(vBat) ? 0.0f : vBat);
  Serial.printf("EV1: %s (%s)\n", txtValvula(ev1), ev1.motivo);
  Serial.printf("EV2: %s (%s)\n", txtValvula(ev2), ev2.motivo);
  Serial.printf("Modo: %s   |   WiFi: %s  ->  http://%s\n", modoManual ? "MANUAL" : "AUTOMATICO",
                AP_SSID, WiFi.softAPIP().toString().c_str());
  if (USAR_NUBE) {
    int codigo = nubeUltimoCodigo;
    if (WiFi.status() != WL_CONNECTED) Serial.printf("Nube: sin conexion a \"%s\"\n", WIFI_SSID);
    else if (codigo == 200) Serial.printf("Nube: OK, ultimo envio hace %lu s\n", (millis() - nubeUltimoOkMs) / 1000);
    else if (codigo == 401) Serial.println(F("Nube: ERROR 401 - DEVICE_KEY no coincide con la de Vercel"));
    else if (codigo == 0)   Serial.println(F("Nube: conectado, esperando primer envio"));
    else Serial.printf("Nube: ERROR %d al enviar a %s\n", codigo, NUBE_URL);
  }
}

void imprimirAyuda() {
  Serial.println(F(
    "\nComandos (escribe y pulsa Enter):\n"
    "  estado     muestra todas las lecturas\n"
    "  auto       modo automatico\n"
    "  manual     modo manual\n"
    "  ev1 on     abre EV1   (ev1 off la cierra; pasa a manual)\n"
    "  ev2 on     abre EV2   (ev2 off la cierra; pasa a manual)\n"
    "  cal7       calibra con el electrodo en buffer pH 7\n"
    "  cal4       calibra con buffer pH 4  (despues de cal7)\n"
    "  cal10      calibra con buffer pH 10 (en lugar de cal4)\n"
    "  calreset   borra la calibracion de pH\n"
    "  ayuda      esta lista\n"));
}

String numJson(float v, int dec) { return isnan(v) ? String("null") : String(v, dec); }

String jsonTanque(const Tanque& t) {
  return "{\"d\":" + numJson(t.listo ? t.distCm : NAN, 1) +
         ",\"n\":" + numJson(t.listo ? t.nivelPct : NAN, 1) +
         ",\"l\":" + numJson(t.listo ? t.litros : NAN, 0) +
         ",\"f\":" + (t.falla ? "true" : "false") + "}";
}

String jsonEstado() {
  String s = "{";
  s += "\"t1\":" + jsonTanque(t1);
  s += ",\"t2\":" + jsonTanque(t2);
  s += ",\"ph\":" + numJson(phValido ? phValor : NAN, 2);
  s += ",\"phv\":" + numJson(phVolt, 3);
  s += ",\"phok\":" + String(phOk ? "true" : "false");
  s += ",\"phmin\":" + String(PH_MIN, 1) + ",\"phmax\":" + String(PH_MAX, 1);
  s += ",\"bat\":" + numJson(vBat, 2);
  s += ",\"batp\":" + String(batPresente ? "true" : "false");
  s += ",\"batb\":" + String(batBaja ? "true" : "false");
  s += ",\"ev1\":" + String(ev1.abierta ? "true" : "false");
  s += ",\"ev2\":" + String(ev2.abierta ? "true" : "false");
  s += ",\"m1\":\"" + String(ev1.motivo) + "\"";
  s += ",\"m2\":\"" + String(ev2.motivo) + "\"";
  s += ",\"manual\":" + String(modoManual ? "true" : "false");
  s += ",\"listo\":" + String(sistemaListo ? "true" : "false");
  s += ",\"up\":" + String(millis() / 1000);
  s += "}";
  return s;
}

// ==========================================================================
//  COMANDOS (compartidos por Serie y web)
// ==========================================================================
String ejecutarComando(String c) {
  c.trim();
  c.toLowerCase();
  if (c == "") return "";
  if (c == "ayuda" || c == "help" || c == "?") { imprimirAyuda(); return ""; }
  if (c == "estado")   { imprimirEstado(); return ""; }
  if (c == "auto")     { ponerModoManual(false); controlar(); return "OK: modo automatico"; }
  if (c == "manual")   { ponerModoManual(true);  controlar(); return "OK: modo manual"; }
  if (c == "cal7")     return calibrarPh7();
  if (c == "cal4")     return calibrarPhPunto(4.0);
  if (c == "cal10")    return calibrarPhPunto(10.0);
  if (c == "calreset") return borrarCalibracion();
  if (c == "ev1 on" || c == "ev1 off" || c == "ev2 on" || c == "ev2 off") {
    if (!modoManual) ponerModoManual(true);
    Valvula& v = (c[2] == '1') ? ev1 : ev2;
    v.manual = c.endsWith("on");
    controlar();
    if (v.manual && !v.abierta) return "AVISO: no se pudo abrir (bateria baja o sistema iniciando)";
    return String("OK: ") + v.nombre + " " + txtValvula(v);
  }
  return "Comando desconocido: '" + c + "'. Escribe 'ayuda'.";
}

void leerSerie() {
  static String linea;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (linea.length()) {
        String r = ejecutarComando(linea);
        if (r.length()) Serial.println(r);
        linea = "";
      }
    } else if (linea.length() < 40) {
      linea += ch;
    }
  }
}

// ==========================================================================
//  PÁGINA WEB
// ==========================================================================
const char PAGINA[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Potabilizadora P12</title>
<style>
:root{--bg:#f4f7fa;--card:#fff;--tx:#1d2733;--mut:#6b7785;--ok:#1f9d55;--bad:#d64545;--warn:#d98a00;--agua:#2f80ed;--bd:#dde3ea}
@media(prefers-color-scheme:dark){:root{--bg:#121820;--card:#1c2530;--tx:#e6edf3;--mut:#93a1b0;--bd:#2c3846}}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--tx)}
header{padding:16px;text-align:center}h1{margin:0;font-size:1.3rem}header p{margin:4px 0 0;color:var(--mut);font-size:.85rem}
main{display:grid;gap:12px;padding:0 16px 24px;max-width:900px;margin:auto;grid-template-columns:repeat(auto-fit,minmax(260px,1fr))}
.c{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px}
.c h2{margin:0 0 10px;font-size:1rem}.big{font-size:2rem;font-weight:700}.mut{color:var(--mut);font-size:.85rem}
.tq{display:flex;gap:14px;align-items:flex-end}.tk{width:70px;height:120px;border:2px solid var(--mut);border-top:none;border-radius:0 0 8px 8px;position:relative;overflow:hidden}
.ag{position:absolute;bottom:0;left:0;right:0;background:var(--agua);opacity:.8;transition:height .6s}
.pill{display:inline-block;padding:3px 10px;border-radius:99px;font-size:.8rem;font-weight:600;color:#fff}
.on{background:var(--ok)}.off{background:var(--mut)}.bad{background:var(--bad)}.warn{background:var(--warn)}
.row{display:flex;justify-content:space-between;align-items:center;margin:6px 0}
button{font:inherit;padding:8px 12px;border-radius:8px;border:1px solid var(--bd);background:var(--bg);color:var(--tx);cursor:pointer}
button.sel{background:var(--agua);color:#fff;border-color:var(--agua)}button:disabled{opacity:.4}
.btns{display:flex;gap:6px;flex-wrap:wrap}#msg{min-height:1.2em}
</style></head><body>
<header><h1>Potabilizadora de agua lluvia · P12</h1><p id="st">Conectando…</p></header>
<main>
<div class="c"><h2>Tanque 1 · agua de lluvia</h2><div class="tq"><div class="tk"><div class="ag" id="a1"></div></div>
<div><div class="big" id="n1">--</div><div class="mut" id="d1"></div><div id="f1"></div></div></div></div>
<div class="c"><h2>Tanque 2 · agua filtrada</h2><div class="tq"><div class="tk"><div class="ag" id="a2"></div></div>
<div><div class="big" id="n2">--</div><div class="mut" id="d2"></div><div id="f2"></div></div></div></div>
<div class="c"><h2>pH del agua filtrada</h2><div class="big" id="ph">--</div><div class="mut" id="phv"></div><div id="phs"></div></div>
<div class="c"><h2>Batería</h2><div class="big" id="bat">--</div><div id="bats"></div></div>
<div class="c"><h2>Electroválvulas</h2>
<div class="row"><span><b>EV1</b> tanque 1 → filtro</span><span id="e1"></span></div><div class="mut" id="m1"></div>
<div class="btns" style="margin:6px 0 12px"><button onclick="cmd('ev1 on')" class="mb">Abrir</button><button onclick="cmd('ev1 off')" class="mb">Cerrar</button></div>
<div class="row"><span><b>EV2</b> salida tanque 2</span><span id="e2"></span></div><div class="mut" id="m2"></div>
<div class="btns" style="margin-top:6px"><button onclick="cmd('ev2 on')" class="mb">Abrir</button><button onclick="cmd('ev2 off')" class="mb">Cerrar</button></div></div>
<div class="c"><h2>Modo de operación</h2><div class="btns"><button id="bA" onclick="cmd('auto')">Automático</button><button id="bM" onclick="cmd('manual')">Manual</button></div>
<p class="mut">En manual puedes abrir o cerrar las válvulas a mano. El corte por batería baja siempre está activo.</p><div class="mut" id="msg"></div></div>
</main>
<script>
const $=id=>document.getElementById(id);
const pill=(t,c)=>'<span class="pill '+c+'">'+t+'</span>';
function tanque(i,t){$('a'+i).style.height=(t.n??0)+'%';$('n'+i).textContent=t.n==null?'--':t.n.toFixed(0)+' %';
$('d'+i).textContent=t.d==null?'sin lectura':t.d.toFixed(1)+' cm · ~'+t.l+' L';$('f'+i).innerHTML=t.f?pill('Falla sensor','bad'):'';}
async function act(){try{const r=await fetch('/api');const j=await r.json();
tanque(1,j.t1);tanque(2,j.t2);
$('ph').textContent=j.ph==null?'--':j.ph.toFixed(2);$('phv').textContent='Po = '+(j.phv==null?'--':j.phv.toFixed(3))+' V · rango '+j.phmin+'–'+j.phmax;
$('phs').innerHTML=j.ph==null?pill('Sin lectura','bad'):(j.phok?pill('Apta','on'):pill('Fuera de rango','bad'));
$('bat').textContent=j.bat==null?'--':j.bat.toFixed(2)+' V';
$('bats').innerHTML=!j.batp?pill('No detectada (USB)','warn'):(j.batb?pill('Baja','bad'):pill('Normal','on'));
$('e1').innerHTML=j.ev1?pill('ABIERTA','on'):pill('CERRADA','off');$('m1').textContent=j.m1;
$('e2').innerHTML=j.ev2?pill('ABIERTA','on'):pill('CERRADA','off');$('m2').textContent=j.m2;
$('bA').className=j.manual?'':'sel';$('bM').className=j.manual?'sel':'';
document.querySelectorAll('.mb').forEach(b=>b.disabled=!j.manual);
$('st').textContent=(j.listo?'Funcionando':'Iniciando sensores…')+' · modo '+(j.manual?'MANUAL':'AUTOMÁTICO')+' · encendido hace '+Math.floor(j.up/60)+' min';
}catch(e){$('st').textContent='Sin conexión con el ESP32';}}
async function cmd(c){try{const r=await fetch('/cmd?c='+encodeURIComponent(c));$('msg').textContent=await r.text();}catch(e){}act();}
act();setInterval(act,2000);
</script></body></html>)rawliteral";

void configurarWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGINA); });
  server.on("/api", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", jsonEstado());
  });
  server.on("/cmd", HTTP_GET, []() {
    String c = server.arg("c");
    // Desde la web solo se permiten comandos de operación (la calibración va por Serie)
    if (!(c == "auto" || c == "manual" || c == "ev1 on" || c == "ev1 off" || c == "ev2 on" || c == "ev2 off")) {
      server.send(400, "text/plain; charset=utf-8", "Comando no permitido");
      return;
    }
    server.send(200, "text/plain; charset=utf-8", ejecutarComando(c));
  });
  server.onNotFound([]() { server.send(404, "text/plain", "No encontrado"); });
  server.begin();
}

// ==========================================================================
//  NUBE (Vercel)
// ==========================================================================
// Llamada desde loop(): copia el estado actual para que la tarea lo envíe.
void encolarNube() {
  String s = jsonEstado();
  if (s.length() >= sizeof(nubeBuffer)) return;   // no debería pasar (~450 bytes)
  if (xSemaphoreTake(nubeMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
  memcpy(nubeBuffer, s.c_str(), s.length() + 1);
  nubePendiente = true;
  xSemaphoreGive(nubeMutex);
}

// Tarea en el núcleo 0: el POST HTTPS tarda ~0,5–1 s y aquí no afecta al loop().
void tareaNube(void*) {
  WiFiClientSecure cliente;
  cliente.setInsecure();   // prototipo: no se valida el certificado de Vercel
  char copia[sizeof(nubeBuffer)];

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));

    bool hay = false;
    if (xSemaphoreTake(nubeMutex, portMAX_DELAY) == pdTRUE) {
      if (nubePendiente) {
        memcpy(copia, nubeBuffer, sizeof(copia));
        nubePendiente = false;
        hay = true;
      }
      xSemaphoreGive(nubeMutex);
    }
    if (!hay || WiFi.status() != WL_CONNECTED) continue;

    HTTPClient http;
    if (!http.begin(cliente, NUBE_URL)) { nubeUltimoCodigo = -1; continue; }
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-device-key", DEVICE_KEY);
    int codigo = http.POST((uint8_t*)copia, strlen(copia));
    http.end();

    nubeUltimoCodigo = codigo;
    if (codigo == 200) nubeUltimoOkMs = millis();
  }
}

void iniciarNube() {
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  nubeMutex = xSemaphoreCreateMutex();
  // Pila holgada: el handshake TLS de mbedTLS usa bastante stack
  xTaskCreatePinnedToCore(tareaNube, "nube", 12288, nullptr, 1, nullptr, 0);
  Serial.printf("Nube: conectando a \"%s\" para enviar a %s\n", WIFI_SSID, NUBE_URL);
}

// ==========================================================================
//  SETUP Y LOOP
// ==========================================================================
void setup() {
  // Relés apagados lo antes posible (válvulas cerradas)
  pinMode(PIN_RELE_EV1, OUTPUT);
  digitalWrite(PIN_RELE_EV1, RELE_INACTIVO);
  pinMode(PIN_RELE_EV2, OUTPUT);
  digitalWrite(PIN_RELE_EV2, RELE_INACTIVO);

  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== Potabilizadora de agua lluvia solar · P12 ==="));

  pinMode(PIN_TRIG1, OUTPUT); digitalWrite(PIN_TRIG1, LOW);
  pinMode(PIN_TRIG2, OUTPUT); digitalWrite(PIN_TRIG2, LOW);
  pinMode(PIN_ECHO1, INPUT);
  pinMode(PIN_ECHO2, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_PH, ADC_11db);    // rango 0 - ~3,1 V
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);

  cargarCalibracion();
  Serial.printf("Calibracion pH: pH7 = %.3f V, pendiente = %.4f V/pH\n", phV7, phPendiente);

  // Con nube: AP propio + estación en la red con internet (el AP pasa al canal del router)
  WiFi.mode(USAR_NUBE ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  configurarWeb();
  Serial.printf("WiFi \"%s\" (clave \"%s\")  ->  abre http://%s\n", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());
  if (USAR_NUBE) iniciarNube();

  imprimirAyuda();
}

void loop() {
  static unsigned long tUltra = 0, tAnalog = 0, tControl = 0, tReporte = 0, tNube = 0;
  static bool turnoT1 = true;
  unsigned long ahora = millis();

  server.handleClient();
  leerSerie();

  // Ultrasónicos: uno a la vez, cada 60 ms, para que no se interfieran los ecos
  if (ahora - tUltra >= 60) {
    tUltra = ahora;
    actualizarTanque(turnoT1 ? t1 : t2);
    turnoT1 = !turnoT1;
  }

  // pH y batería: una vez por segundo
  if (ahora - tAnalog >= 1000) {
    tAnalog = ahora;
    actualizarPh();
    actualizarBateria();
  }

  // Arranque: espera lecturas de ambos tanques (o falla declarada) y unos segundos de pH
  if (!sistemaListo && ahora > 4000 && (t1.listo || t1.falla) && (t2.listo || t2.falla)) {
    sistemaListo = true;
    Serial.println(F(">> Sensores listos, control activo"));
  }

  if (ahora - tControl >= 250) {
    tControl = ahora;
    controlar();
  }

  if (ahora - tReporte >= 3000) {
    tReporte = ahora;
    imprimirEstado();
  }

  if (USAR_NUBE && ahora - tNube >= NUBE_CADA_MS) {
    tNube = ahora;
    encolarNube();
  }
}
