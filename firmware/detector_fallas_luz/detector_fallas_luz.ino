/*
 * ============================================================
 *  DETECTOR DE FALLA DE LUZ - VENEZUELA
 *  Versión: 6.0 - Corriente AC + mejor estética en pantalla OLED
 *
 *  Cambios respecto a v5.0:
 *  - Estado de falla activa persistido en NVS
 *  - Al reiniciarse, recupera la falla en curso y continúa
 *  - Duración calculada con NTP tras reinicio (hora real)
 *  - FALLA_INICIO guardado en NVS antes de intentar envío
 *  - FALLA_FIN guardado en NVS antes de intentar envío
 *  - NVS mantiene registros hasta confirmar envío exitoso
 *  - Timeout HTTP aumentado a 8s para mayor confiabilidad
 *
 *  Cambios de diagnóstico (sobre v5.2):
 *  - Al arrancar, muestra en el OLED y por Serial la causa del
 *    reinicio anterior (esp_reset_reason). Si dice "BROWNOUT",
 *    confirma que el problema es de alimentación, no de código.
 *
 *  Hardware:
 *  - Arduino ESP32C3 Super Mini
 *  - Módulo TP4056 con protección (OUT+ directo al pin 5V)
 *  - Transformador HKL-5M05 5V (Vo+ → IN+ TP4056, Vo- → IN- TP4056)
 *  - Batería Li-ion 18650 3000mAh conectada
 *  - Sensor ZMPT101B (VCC → 3V3, GND → GND, OUT → GPIO3)
 *  - Pantalla OLED 0.96" SSD1306 via I2C
 *  - Switch SWKCD1G 
 *  - Conexión WiFi: router local [pendiente BAM USB]
 *
 *  Servicios externos:
 *  - Google Sheets (via Google Apps Script Web App)
 *  - NTP para hora exacta
 *  - Vercel.app
 *
 *  Jesús Rivas && Sofía Karabin
 *  Ing. de Sistemas
 *  UNEFA
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <time.h>
#include "esp_system.h"
#include "secrets.h"

// ============================================================
//  CONFIGURACIÓN
// ============================================================

// WIFI_SSID, WIFI_PASSWORD y GOOGLE_SCRIPT_URL
// definidos en secrets.h

// NTP Venezuela = UTC-4
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_S = -4 * 3600;
const int   DST_OFFSET_S = 0;

// ============================================================
//  PINES
// ============================================================

// OLED I2C (ESP32C3 Super Mini: SDA=8, SCL=9)
#define OLED_SDA 8
#define OLED_SCL 9
#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C

// ZMPT101B
#define PIN_SENSOR 3 // GPIO3 → OUT del sensor

// ============================================================
//  CONSTANTES
// ============================================================
#define OLED_UPDATE_MS 1000 // refresco pantalla
#define MAX_REGISTROS 60 // buffer NVS

// Detección RMS [Root Mean Square (Raíz Cuadrada de la Media de Cuadrados)]
// Mide la energía de la oscilación AC independientemente del voltaje
// de alimentación (USB o batería). Sin AC: ~6-13. Con AC: ~275.
#define TIEMPO_MUESTRAS 100 // ms de muestreo por medición
#define UMBRAL_RMS 250 // umbral de detección (mitad entre 13 y 275)

// ============================================================
//  VARIABLES GLOBALES
// ============================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences nvs;

// ============================================================
//  ÍCONOS OLED (16x16, 1 bit por pixel, formato Adafruit_GFX)
// ============================================================
const unsigned char PROGMEM icon_bulb_on[] = { 0x21, 0x04, 0x23, 0xC4, 0x07, 0xE0, 0x0F, 0xE0, 0x8F, 0xE0, 0x0F, 0xE0, 0x0F, 0xE0, 0x07, 0xC0, 0x00, 0x00, 0x03, 0x80, 0x03, 0x80, 0x03, 0x80, 0x03, 0x80, 0x00, 0x00, 0x03, 0x80, 0x00, 0x00 };
const unsigned char PROGMEM icon_bulb_off[] = { 0x00, 0x00, 0x03, 0xC0, 0x04, 0x60, 0x08, 0x20, 0x08, 0x20, 0x08, 0x20, 0x0C, 0x60, 0x07, 0xC0, 0x01, 0x00, 0x03, 0x80, 0x03, 0x80, 0x03, 0x80, 0x03, 0x80, 0x00, 0x00, 0x03, 0x80, 0x00, 0x00 };
const unsigned char PROGMEM icon_alarma[] = { 0x00, 0x84, 0x10, 0x8C, 0x18, 0x08, 0x03, 0xE0, 0x0F, 0xF0, 0x1F, 0xF8, 0x1F, 0xFC, 0x3F, 0xFC, 0x3F, 0xFC, 0x0F, 0xF8, 0x0F, 0xF0, 0x0F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
const unsigned char PROGMEM icon_wifi[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xE0, 0x0C, 0x38, 0x10, 0x0C, 0x23, 0xC4, 0x66, 0x26, 0x4C, 0x12, 0x48, 0x12, 0x01, 0x80, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00 };
const unsigned char PROGMEM icon_wave[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xC0, 0x01, 0x60, 0x03, 0x20, 0x02, 0x30, 0x06, 0x10, 0x84, 0x18, 0x84, 0x08, 0xCC, 0x08, 0x48, 0x0C, 0x38, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 };
const unsigned char PROGMEM icon_madrugada[] = { 0x00, 0x00, 0x00, 0x00, 0x82, 0x00, 0x86, 0x00, 0x0C, 0x00, 0x1C, 0x00, 0x1C, 0x00, 0x5C, 0x00, 0x5E, 0x00, 0x0E, 0x00, 0x0F, 0x00, 0x07, 0xC0, 0x10, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00 };
const unsigned char PROGMEM icon_manana[] = { 0x00, 0x00, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x10, 0x08, 0x0B, 0xF8, 0x0F, 0xF0, 0x0F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
const unsigned char PROGMEM icon_tarde[] = { 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x08, 0x03, 0xD0, 0x07, 0xE0, 0x0F, 0xE0, 0x6F, 0xE0, 0x6F, 0xE6, 0x0F, 0xE0, 0x07, 0xC0, 0x08, 0x10, 0x10, 0x08, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00 };
const unsigned char PROGMEM icon_noche[] = { 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x06, 0x00, 0x0C, 0x00, 0x1C, 0x00, 0x1C, 0x00, 0x1C, 0x00, 0x1E, 0x00, 0x6E, 0x00, 0x0F, 0x00, 0x07, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// Estado del sensor
bool luzPresente = true;
bool lecturaAnterior = true;
int amplitudADC = 0;

// ============================================================
//  HISTORIAL DE RMS PARA LA GRÁFICA DINÁMICA EN PANTALLA
// ============================================================
#define HIST_RMS_N 16
int histRMS[HIST_RMS_N] = {0};
int histRMSPos = 0;

void pushHistorialRMS(int valor)
{
  histRMS[histRMSPos] = valor;
  histRMSPos = (histRMSPos + 1) % HIST_RMS_N;
}

// Dibuja el historial como una línea continua dentro del recuadro x,y,w,h
void dibujarGraficaRMS(int x, int y, int w, int h)
{
  int minV = histRMS[0], maxV = histRMS[0];
  for (int i = 1; i < HIST_RMS_N; i++)
  {
    if (histRMS[i] < minV) minV = histRMS[i];
    if (histRMS[i] > maxV) maxV = histRMS[i];
  }
  if (maxV - minV < 8) { maxV += 4; minV -= 4; }
  if (minV < 0) minV = 0;

  int prevX = x, prevY = y + h - 1;
  for (int i = 0; i < HIST_RMS_N; i++)
  {
    int valor = histRMS[(histRMSPos + i) % HIST_RMS_N];
    int px = x + (i * (w - 1)) / (HIST_RMS_N - 1);
    long escala = (long)(valor - minV) * (h - 1) / (maxV - minV);
    int py = y + h - 1 - (int)escala;
    if (i > 0) display.drawLine(prevX, prevY, px, py, SSD1306_WHITE);
    prevX = px; prevY = py;
  }
}

// Temporización
unsigned long tiempoFalla = 0;
unsigned long duracionFalla = 0;
unsigned long ultimoDisplay = 0;
unsigned long ultimoWifiCheck = 0;
unsigned long ultimoSync = 0;

// Banderas de escritura NVS (no bloqueante)
bool nvsPendiente = false;
String nvsTipo = "";
String nvsHora = "";
unsigned long nvsDurSeg = 0;
String nvsNotas = "";

//Wifi e internet
bool internetDisponible = false;
unsigned long ultimoNTPSync = 0;
unsigned long ahoraGlobal = 0;

// Banderas de envío HTTP
bool envioFallaPendiente = false;
bool envioRetornoPendiente = false;

// Datos del evento actual
int contadorFallas = 0;
String horaFalla = "";
String horaRetorno = "";
String duracionStr = "";

// NVS sincronización
int pendientesEnNVS = 0;
bool sincronizando = false;

// ============================================================
//  DIAGNÓSTICO: CAUSA DEL ÚLTIMO REINICIO
//  Permite saber si el chip se reinició por brownout (caída de
//  voltaje), watchdog, panic, o un encendido normal.
// ============================================================
String razonReinicio()
{
  esp_reset_reason_t r = esp_reset_reason();

  switch (r)
  {
    case ESP_RST_POWERON:   return "Encendido normal";
    case ESP_RST_EXT:       return "Reset externo (pin)";
    case ESP_RST_SW:        return "Reset por software";
    case ESP_RST_PANIC:     return "PANIC / excepcion";
    case ESP_RST_INT_WDT:   return "Watchdog interno";
    case ESP_RST_TASK_WDT:  return "Watchdog de tarea";
    case ESP_RST_WDT:       return "Otro watchdog";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (caida V)";
    case ESP_RST_SDIO:      return "Reset SDIO";
    default:                return "Actualizando codigo (" + String((int)r) + ")";
  }
}

// ============================================================
//  PROTOTIPOS
// ============================================================
bool medirLuz();
void guardarEnNVS(String tipo, String hora, unsigned long durSeg, String notas);
void sincronizarUnPendiente();
int contarPendientes();
void cargarContadores();
void recuperarFallaActiva();
bool enviarAGoogleSheets(String tipo, String hora, String duracion, String notas);
String obtenerHora();
String segundosAHMS(unsigned long seg);
long diferenciaEnSegundos(String horaInicio, String horaFin);
void mostrarEstado();
const unsigned char* iconoFranjaActual();
void mostrarMensaje(String linea1, String linea2 = "", String linea3 = "");

// ============================================================
//  SETUP
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== DETECTOR DE FALLA DE LUZ v5.2 ===");

  analogReadResolution(12);

  // I2C y OLED
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("[ERROR] OLED no encontrada");
  }
  else
  {
    display.setTextColor(SSD1306_WHITE);
  }

  // DIAGNÓSTICO: causa del reinicio anterior (clave para detectar
  // brownouts por alimentación inestable cuando se usa el step-down)
  String causaReset = razonReinicio();
  Serial.println("[BOOT] Causa del reinicio: " + causaReset);
  mostrarMensaje("Causa reinicio:", causaReset, "");
  delay(2500);

  // NVS
  nvs.begin("falluzvzla", false);
  cargarContadores();
  pendientesEnNVS = contarPendientes();
  Serial.println("[NVS] Total: " + String(nvs.getInt("total", 0)) + " | Pendientes: " + String(pendientesEnNVS));

  // WiFi
  mostrarMensaje("Conectando", "WiFi...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // baja potencia TX para estabilidad con batería
  WiFi.setSleep(false); // evita modo ahorro que pierde conexión
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Esperar máximo 15s mostrando progreso en pantalla
  unsigned long inicioWifi = millis();
  int intentosMostrados = 0;

  while (WiFi.status() != WL_CONNECTED && millis() - inicioWifi < 15000)
  {
    delay(500);
    intentosMostrados++;
    mostrarMensaje(
      "Conectando WiFi",
      WIFI_SSID,
      String(intentosMostrados / 2) + "s / 15s"
    );
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("[WiFi] Conectado! IP: " + WiFi.localIP().toString());

    // NTP solo si hay WiFi
    mostrarMensaje("Sincronizando", "hora NTP...", "");
    configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);

    struct tm timeinfo;
    int intentosNTP = 0;

    while (!getLocalTime(&timeinfo) && intentosNTP < 10)
    {
      delay(500);
      intentosNTP++;
    }

    if (intentosNTP < 10)
    {
      Serial.println("[OK] Hora: " + obtenerHora());
    }
    else
    {
      Serial.println("[WARN] Sin hora NTP");
    }
  }
  else
  {
    Serial.println("[WiFi] Sin conexión, continuando...");
  }

  // Recuperar falla activa si el sistema se reinició durante un corte
  // Esto ocurre cuando la batería sigue alimentando pero el chip se resetea
  recuperarFallaActiva();

  // Lectura inicial del sensor
  lecturaAnterior = medirLuz();

  // Si hay falla activa recuperada, mantener ese estado
  // sin importar lo que lea el sensor en este instante
  if (!luzPresente)
  {
    lecturaAnterior = false;
  }
  else
  {
    luzPresente = lecturaAnterior;
  }

  Serial.println("[OK] Sistema listo. RMS: " + String(amplitudADC) + " | Estado: " + String(luzPresente ? "LUZ" : "SIN LUZ"));
  mostrarEstado();
}

// ============================================================
//  LOOP PRINCIPAL
// ============================================================
void loop()
{
  unsigned long ahora = millis();
  ahoraGlobal = ahora;

  // SENSOR
  bool lecturaActual = medirLuz();

  if (lecturaActual != lecturaAnterior)
  {
    lecturaAnterior = lecturaActual;
    luzPresente = lecturaActual;

    if (!lecturaActual)
    {
      // SE FUE LA LUZ
      tiempoFalla = ahora;
      horaFalla = obtenerHora();
      duracionFalla = 0;
      contadorFallas++;

      Serial.println("\n[FALLA] " + horaFalla + " | F#" + String(contadorFallas) + " | RMS:" + String(amplitudADC));

      // Persistir estado de falla activa en NVS
      // Si el chip se reinicia durante el corte, recuperará esto
      nvs.putBool("fallaActiva", true);
      nvs.putString("fallaHoraInicio", horaFalla.c_str());

      // Marcar para guardar registro completo en NVS
      nvsPendiente = true;
      nvsTipo = "FALLA_INICIO";
      nvsHora = horaFalla;
      nvsDurSeg = 0;
      nvsNotas = "Falla #" + String(contadorFallas);

      envioFallaPendiente = true;
      envioRetornoPendiente = false;
    }
    else
    {
      // VOLVIÓ LA LUZ
      horaRetorno = obtenerHora();

      // Duración real: preferir el cálculo por timestamps NTP (horaFalla/horaRetorno).
      // Esto sigue siendo correcto incluso si el ESP32 se reinició durante el corte,
      // porque millis() se reinicia a 0 en cada reinicio y (ahora - tiempoFalla)
      // por sí solo deja de ser confiable en ese caso.
      if (horaFalla.length() >= 19 && horaRetorno.length() >= 19)
      {
        long durNTP = diferenciaEnSegundos(horaFalla, horaRetorno);
        duracionFalla = (durNTP > 0) ? (unsigned long)durNTP : (ahora - tiempoFalla) / 1000;
      }
      else
      {
        duracionFalla = (ahora - tiempoFalla) / 1000;
      }

      duracionStr = segundosAHMS(duracionFalla);

      Serial.println("\n[RETORNO] " + horaRetorno + " | Dur: " + duracionStr + " | RMS:" + String(amplitudADC));

      // Limpiar estado de falla activa en NVS
      nvs.putBool("fallaActiva", false);
      nvs.putString("fallaHoraInicio", "");

      // Marcar para guardar registro completo en NVS
      nvsPendiente = true;
      nvsTipo = "FALLA_FIN";
      nvsHora = horaRetorno;
      nvsDurSeg = duracionFalla;
      nvsNotas = "Dur: " + duracionStr + " | Inicio: " + horaFalla;

      envioRetornoPendiente = true;
      envioFallaPendiente = false;
    }

    mostrarEstado();
  }

  // ESCRITURA NVS
  if (nvsPendiente)
  {
    nvsPendiente = false;
    nvs.putInt("fallasTot", contadorFallas);
    guardarEnNVS(nvsTipo, nvsHora, nvsDurSeg, nvsNotas);
  }

  // ENVÍO HTTP — solo si hay WiFi
  if (envioFallaPendiente && internetDisponible)
  {
    envioFallaPendiente = false;
    int idx = nvs.getInt("total", 1) - 1;
    bool ok = enviarAGoogleSheets("FALLA_INICIO", horaFalla, "0", "Falla #" + String(contadorFallas));

    if (ok)
    {
      String key = "env" + String(idx);
      nvs.putBool(key.c_str(), true);
      pendientesEnNVS = contarPendientes();
      Serial.println("[OK] Falla enviada a Sheets");
    }
    else
    {
      Serial.println("[WARN] Sin internet, falla guardada en NVS");
    }
  }

  if (envioRetornoPendiente && internetDisponible)
  {
    envioRetornoPendiente = false;
    int idx = nvs.getInt("total", 1) - 1;
    bool ok = enviarAGoogleSheets("FALLA_FIN", horaRetorno, String(duracionFalla), "Dur: " + duracionStr + " | Inicio: " + horaFalla);

    if (ok)
    {
      String key = "env" + String(idx);
      nvs.putBool(key.c_str(), true);
      pendientesEnNVS = contarPendientes();
      Serial.println("[OK] Retorno enviado a Sheets");
    }
    else
    {
      Serial.println("[WARN] Sin internet, retorno guardado en NVS");
    }
  }

  // WIFI, NTP Y SINCRONIZACIÓN
  if (ahora - ultimoWifiCheck >= 15000)   // cada 15s en lugar de 30s
  {
    ultimoWifiCheck = ahora;

    if (WiFi.status() != WL_CONNECTED)
    {
      // Sin WiFi — intentar reconectar
      internetDisponible = false;
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.println("[WiFi] Reconectando...");
    }
    else
    {
      // WiFi asociado — verificar si hay internet real
      bool hayInternet = verificarInternet();

      if (hayInternet && !internetDisponible)
      {
        // Acaba de recuperar internet — re-sincronizar NTP
        internetDisponible = true;
        Serial.println("[WiFi] Internet recuperado");
        sincronizarNTP();
        ultimoNTPSync = ahora;

        // Forzar sincronización inmediata de pendientes
        ultimoSync = 0;
      }
      else if (!hayInternet)
      {
        internetDisponible = false;
      }
    }
  }

  // Sincronizar pendientes — solo con internet real confirmado
  if (internetDisponible && pendientesEnNVS > 0 && !sincronizando && (ahora - ultimoSync >= 30000))   // cada 30s en lugar de 60s
  {
    ultimoSync = ahora;
    sincronizarUnPendiente();
  }

  // PANTALLA
  if (ahora - ultimoDisplay >= OLED_UPDATE_MS)
  {
    ultimoDisplay = ahora;
    pushHistorialRMS(amplitudADC);
    mostrarEstado();
  }
}

// ============================================================
//  RECUPERAR FALLA ACTIVA TRAS REINICIO
//
//  Si el chip se reinició durante un corte de luz:
//  - NVS tiene fallaActiva = true y fallaHoraInicio guardados
//  - Se restaura el estado de falla con la hora original
//  - La duración se calcula con NTP usando la hora real de inicio
//  - El contador visual arranca desde 0 pero el registro tendrá
//    la duración real desde la hora original hasta el retorno
// ============================================================
void recuperarFallaActiva()
{
  bool hayFallaActiva = nvs.getBool("fallaActiva", false);

  if (!hayFallaActiva) return;

  horaFalla = nvs.getString("fallaHoraInicio", "");

  if (horaFalla == "")
  {
    // No hay hora guardada, limpiar estado inconsistente
    nvs.putBool("fallaActiva", false);
    return;
  }

  // Restaurar estado de falla
  luzPresente = false;
  lecturaAnterior = false;
  tiempoFalla = millis();// contador visual desde 0
  duracionFalla = 0;
  contadorFallas++;
  nvs.putInt("fallasTot", contadorFallas);

  Serial.println("[RECOVERY] Falla activa recuperada desde: " + horaFalla);
  Serial.println("[RECOVERY] El contador visual arranca desde 0");
  Serial.println("[RECOVERY] La duración real se calculará al retorno con NTP");

  mostrarMensaje("REINICIO", "Falla activa desde:", horaFalla.length() >= 19 ? horaFalla.substring(11, 16) : horaFalla);
  delay(2000);
}

// ============================================================
//  MEDIR LUZ POR RMS (Root Mean Square)
//
//  El RMS mide la energía de oscilación de la señal AC:
//  - Pasada 1: calcula el centro real de la señal (promedio)
//  - Pasada 2: calcula cuánto se aleja cada muestra del centro
//  - Resultado: valor estable independiente de la alimentación
//  Sin AC: ~6-13 RMS. Con AC 110V: ~275 RMS. Umbral: 250.
// ============================================================
bool medirLuz()
{
  long sumaLineal = 0;
  int  muestras = 0;

  // Pasada 1
  unsigned long inicio = millis();

  while (millis() - inicio < TIEMPO_MUESTRAS)
  {
    sumaLineal += analogRead(PIN_SENSOR);
    muestras++;
  }

  int centroReal = (int)(sumaLineal / muestras);

  // Pasada 2
  long sumaRMS = 0;
  muestras = 0;
  inicio = millis();

  while (millis() - inicio < TIEMPO_MUESTRAS)
  {
    long desviacion = analogRead(PIN_SENSOR) - centroReal;
    sumaRMS += desviacion * desviacion;
    muestras++;
  }

  amplitudADC = (int)sqrt((double)sumaRMS / muestras);

  return (amplitudADC > UMBRAL_RMS);
}

// ============================================================
//  GUARDAR REGISTRO EN NVS
//  Los registros se mantienen en NVS hasta confirmar envío
//  exitoso a Google Sheets (env = true)
// ============================================================
void guardarEnNVS(String tipo, String hora, unsigned long durSeg, String notas)
{
  int total = nvs.getInt("total", 0);

  if (total >= MAX_REGISTROS)
  {
    // Buffer lleno: descartar el más antiguo
    Serial.println("[NVS] Buffer lleno, descartando registro más antiguo");

    for (int i = 0; i < MAX_REGISTROS - 1; i++)
    {
      nvs.putString(("tp" + String(i)).c_str(), nvs.getString(("tp" + String(i + 1)).c_str(), ""));
      nvs.putString(("hr" + String(i)).c_str(), nvs.getString(("hr" + String(i + 1)).c_str(), ""));
      nvs.putULong(("dr" + String(i)).c_str(), nvs.getULong(("dr" + String(i + 1)).c_str(), 0));
      nvs.putString(("nt" + String(i)).c_str(), nvs.getString(("nt" + String(i + 1)).c_str(), ""));
      nvs.putBool(("env" + String(i)).c_str(), nvs.getBool(("env" + String(i + 1)).c_str(), false));
    }

    total = MAX_REGISTROS - 1;
  }

  String idx = String(total);
  nvs.putString(("tp" + idx).c_str(), tipo.c_str());
  nvs.putString(("hr" + idx).c_str(), hora.c_str());
  nvs.putULong(("dr" + idx).c_str(), durSeg);
  nvs.putString(("nt" + idx).c_str(), notas.c_str());
  nvs.putBool(("env" + idx).c_str(), false);   // pendiente de envío
  nvs.putInt("total", total + 1);

  pendientesEnNVS = contarPendientes();
  Serial.println("[NVS] Guardado #" + idx + " | " + tipo + " | " + hora);
}


// ============================================================
//  SINCRONIZAR UN PENDIENTE POR CICLO
//  Un registro por vez para no bloquear el loop
// ============================================================
void sincronizarUnPendiente()
{
  if (!internetDisponible) return;

  int total = nvs.getInt("total", 0);
  bool envioCorrecto = false;

  for (int i = 0; i < total; i++)
  {
    String envKey    = "env" + String(i);
    bool   yaEnviado = nvs.getBool(envKey.c_str(), false);

    if (yaEnviado) continue;

    String        tipo  = nvs.getString(("tp" + String(i)).c_str(), "");
    String        hora  = nvs.getString(("hr" + String(i)).c_str(), "");
    unsigned long dur   = nvs.getULong( ("dr" + String(i)).c_str(), 0);
    String        notas = nvs.getString(("nt" + String(i)).c_str(), "");

    if (tipo == "") continue;

    Serial.println("[SYNC] Enviando #" + String(i) + ": " + tipo + " | " + hora);

    bool ok = enviarAGoogleSheets(tipo, hora, String(dur), notas);

    if (ok)
    {
      nvs.putBool(envKey.c_str(), true);
      pendientesEnNVS = contarPendientes();
      envioCorrecto   = true;
      Serial.println("[SYNC] #" + String(i) + " OK. Pendientes: " + String(pendientesEnNVS));

      // Si quedan más pendientes, no esperar 30s — intentar en 2s
      if (pendientesEnNVS > 0)
      {
        ultimoSync = ahoraGlobal - 28000;   // fuerza reintento en 2s
      }
    }
    else
    {
      Serial.println("[SYNC] #" + String(i) + " falló");

      // Verificar si perdimos internet
      if (!verificarInternet())
      {
        internetDisponible = false;
        Serial.println("[SYNC] Internet perdido durante sincronización");
      }
    }

    break;
  }
}

// ============================================================
//  CONTAR REGISTROS PENDIENTES
// ============================================================
int contarPendientes()
{
  int total = nvs.getInt("total", 0);
  int pendientes = 0;

  for (int i = 0; i < total; i++)
  {
    if (!nvs.getBool(("env" + String(i)).c_str(), false)) pendientes++;
  }

  return pendientes;
}

// ============================================================
//  CARGAR CONTADORES DE SESIONES ANTERIORES
// ============================================================
void cargarContadores()
{
  contadorFallas = nvs.getInt("fallasTot", 0);
  Serial.println("[NVS] Fallas históricas: " + String(contadorFallas));
}

// ============================================================
//  ENVIAR DATOS A GOOGLE SHEETS
// ============================================================
bool enviarAGoogleSheets(String tipo, String hora, String duracion, String notas)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[HTTP] Sin WiFi, no se puede enviar");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url = String(GOOGLE_SCRIPT_URL);
  url += "?tipo=" + tipo;
  url += "&hora=" + hora;
  url += "&duracion=" + duracion;
  url += "&notas=" + notas;
  url.replace(" ", "%20");

  Serial.println("[HTTP] " + url.substring(0, 60) + "...");

  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000); // 8s: Google Apps Script puede tardar hasta 4s

  int httpCode = http.GET();

  if (httpCode > 0)
  {
    String respuesta = http.getString();
    Serial.println("[HTTP] " + String(httpCode) + " | " + respuesta);
    http.end();
    return (httpCode == 200 || httpCode == 302);
  }
  else
  {
    Serial.println("[HTTP] Error: " + http.errorToString(httpCode));
    http.end();
    return false;
  }
}

// ============================================================
//  CONECTAR WIFI
// ============================================================
void conectarWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WiFi] Iniciando conexión...");
}

// ============================================================
//  VERIFICAR INTERNET REAL (no solo WiFi)
// ============================================================
bool verificarInternet()
{
  if (WiFi.status() != WL_CONNECTED) return false;

  // Intentar resolver un host conocido
  IPAddress ip;
  bool resuelto = WiFi.hostByName("pool.ntp.org", ip);

  return resuelto;
}

// ============================================================
//  RE-SINCRONIZAR HORA NTP
//  Se llama cuando el WiFi recupera internet real
// ============================================================
void sincronizarNTP()
{
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);

  struct tm timeinfo;
  int intentos = 0;

  while (!getLocalTime(&timeinfo) && intentos < 6)
  {
    delay(500);
    intentos++;
  }

  if (intentos < 6)
  {
    Serial.println("[NTP] Hora re-sincronizada: " + obtenerHora());
  }
}

// ============================================================
//  CALCULAR DIFERENCIA EN SEGUNDOS ENTRE DOS TIMESTAMPS
// ============================================================
long diferenciaEnSegundos(String horaInicio, String horaFin)
{
  if (horaInicio.length() < 19 || horaFin.length() < 19) return 0;

  struct tm tmInicio = {0};
  struct tm tmFin = {0};

  // Parsear inicio
  tmInicio.tm_year = horaInicio.substring(0, 4).toInt() - 1900;
  tmInicio.tm_mon = horaInicio.substring(5, 7).toInt() - 1;
  tmInicio.tm_mday = horaInicio.substring(8, 10).toInt();
  tmInicio.tm_hour = horaInicio.substring(11, 13).toInt();
  tmInicio.tm_min = horaInicio.substring(14, 16).toInt();
  tmInicio.tm_sec = horaInicio.substring(17, 19).toInt();

  // Parsear fin
  tmFin.tm_year = horaFin.substring(0, 4).toInt() - 1900;
  tmFin.tm_mon = horaFin.substring(5, 7).toInt() - 1;
  tmFin.tm_mday = horaFin.substring(8, 10).toInt();
  tmFin.tm_hour = horaFin.substring(11, 13).toInt();
  tmFin.tm_min = horaFin.substring(14, 16).toInt();
  tmFin.tm_sec = horaFin.substring(17, 19).toInt();

  time_t tInicio = mktime(&tmInicio);
  time_t tFin = mktime(&tmFin);

  return (long)(tFin - tInicio);
}

// ============================================================
//  OBTENER HORA COMO STRING
// ============================================================
String obtenerHora()
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo))
  {
    // Fallback: tiempo relativo desde arranque si no hay NTP
    unsigned long s = millis() / 1000;
    char buf[20];
    sprintf(buf, "T+%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);
    return String(buf);
  }

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

// ============================================================
//  SEGUNDOS A H:M:S
// ============================================================
String segundosAHMS(unsigned long seg)
{
  unsigned long h = seg / 3600;
  unsigned long m = (seg % 3600) / 60;
  unsigned long s = seg % 60;
  char buf[12];
  sprintf(buf, "%02luh%02lum%02lus", h, m, s);
  return String(buf);
}

// ============================================================
//  ÍCONO SEGÚN LA FRANJA HORARIA ACTUAL
// ============================================================
const unsigned char* iconoFranjaActual()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return icon_tarde; // sin hora real todavía
  int h = timeinfo.tm_hour;
  if (h < 6)  return icon_madrugada;
  if (h < 12) return icon_manana;
  if (h < 19) return icon_tarde;
  return icon_noche;
}

// ============================================================
//  MOSTRAR ESTADO EN OLED (INTERFAZ PRO V3 - CON ÍCONOS)
// ============================================================
void mostrarEstado()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  const unsigned char* iconoFranja = iconoFranjaActual();

  if (!luzPresente)
  {
    // === BANNER DE ALERTA (colores invertidos) ===
    display.fillRect(1, 1, SCREEN_W - 2, 15, SSD1306_WHITE);
    display.drawBitmap(2, 0, icon_alarma, 16, 16, SSD1306_BLACK);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(22, 4);
    display.print("SIN ELECTRICIDAD");
    display.setTextColor(SSD1306_WHITE);

    // Duración grande y centrada
    unsigned long durActual = (millis() - tiempoFalla) / 1000;
    char buf[9];
    sprintf(buf, "%02lu:%02lu:%02lu", durActual / 3600, (durActual % 3600) / 60, durActual % 60);
    display.setTextSize(2);
    display.setCursor((SCREEN_W - (int)strlen(buf) * 12) / 2, 28);
    display.print(buf);

    // Contexto: desde qué hora empezó, centrado
    String linea = "Desde: " + (horaFalla.length() >= 19 ? horaFalla.substring(11, 16) : horaFalla);
    display.setTextSize(1);
    display.setCursor((SCREEN_W - (int)linea.length() * 6) / 2, 50);
    display.print(linea);
  }
  else
  {
    // === MODO NORMAL ===
    display.drawBitmap(2, 0, icon_bulb_on, 16, 16, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(22, 4);
    display.print("HAY ELECTRICIDAD");

    display.drawFastHLine(1, 17, SCREEN_W - 2, SSD1306_WHITE);

    // Ícono de franja horaria a la izquierda + hora centrada en toda la pantalla
    display.drawBitmap(4, 20, iconoFranja, 16, 16, SSD1306_WHITE);
    String hora = obtenerHora();
    String hhmm = hora.length() >= 19 ? hora.substring(11, 16) : hora;
    display.setTextSize(2);
    display.setCursor((SCREEN_W - (int)hhmm.length() * 12) / 2, 21);
    display.print(hhmm);

    display.drawFastHLine(1, 41, SCREEN_W - 2, SSD1306_WHITE);
    display.drawFastVLine(64, 43, 19, SSD1306_WHITE);

    // RMS centrado: gráfica + etiqueta + valor
    const int gw = 16, gh = 13, gy = 45;
    String valRMS = String(amplitudADC);
    int anchoTextoRMS = max(3, (int)valRMS.length()) * 6; // "RMS" = 3 chars
    int bloqueAnchoRMS = gw + 5 + anchoTextoRMS;
    int gx = (64 - bloqueAnchoRMS) / 2;
    dibujarGraficaRMS(gx, gy, gw, gh);
    display.setTextSize(1);
    int txRMS = gx + gw + 5;
    display.setCursor(txRMS, 44);
    display.print("RMS");
    display.setCursor(txRMS, 54);
    display.print(valRMS);

    // WiFi centrado: ícono + etiqueta + valor
    bool hayPendientes = (pendientesEnNVS > 0);
    String etiquetaWifi = hayPendientes ? "NVS" : "WiFi";
    String valorWifi    = hayPendientes ? String(pendientesEnNVS) : (WiFi.status() == WL_CONNECTED ? "SI" : "NO");
    int bloqueAncho = 16 + 5 + max((int)etiquetaWifi.length(), (int)valorWifi.length()) * 6;
    int bx = 64 + (64 - bloqueAncho) / 2;
    display.drawBitmap(bx, 45, icon_wifi, 16, 16, SSD1306_WHITE);
    display.setCursor(bx + 16 + 5, 44);
    display.print(etiquetaWifi);
    display.setCursor(bx + 16 + 5, 54);
    display.print(valorWifi);
  }

  // Silueta de la pantalla con esquinas redondeadas
  display.drawRoundRect(0, 0, SCREEN_W, SCREEN_H, 4, SSD1306_WHITE);
  display.display();
}

// ============================================================
//  MOSTRAR MENSAJE EN OLED
// ============================================================
void mostrarMensaje(String linea1, String linea2, String linea3)
{
  display.clearDisplay();
  display.drawRoundRect(0, 0, SCREEN_W, SCREEN_H, 3, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  
  display.setCursor(8, 12);
  display.println(linea1);

  display.setCursor(8, 30);
  display.println(linea2);

  display.setCursor(8, 48);
  display.println(linea3);

  display.display();
}