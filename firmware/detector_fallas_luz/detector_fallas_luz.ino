/*
 * ============================================================
 *  DETECTOR DE FALLA DE LUZ - VENEZUELA
 *  Versión: 5.0 - Arquitectura no bloqueante completa
 *
 *  Cambios respecto a v4.0:
 *  - Detección por RMS (más estable que pico a pico)
 *  - WiFi 100% asíncrono, nunca bloquea el loop
 *  - NVS escrito con bandera, no interrumpe el contador
 *  - HTTP con timeout reducido a 5s
 *  - Sincronización de un registro por ciclo, no en bloque
 *  - Umbral RMS: 100 (sin luz ~13, con luz ~275)
 *
 *  Hardware:
 *  - Arduino ESP32C3 Super Mini
 *  - Módulo TP4056 con protección (OUT+ → 5V, OUT- → GND)
 *  - Batería Li-ion 18650 3000mAh conectada via JST
 *  - Sensor ZMPT101B (VCC → 3V3, GND → GND, OUT → GPIO3)
 *  - Pantalla OLED 0.96" SSD1306 via I2C
 *  - Conexión WiFi: router local [pendiente BAM USB]
 *
 *  Servicios externos:
 *  - Google Sheets (via Google Apps Script Web App)
 *  - NTP para hora exacta
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
#include "secrets.h"

// ============================================================
//  CONFIGURACIÓN
// ============================================================

// WIFI_SSID, WIFI_PASSWORD y GOOGLE_SCRIPT_URL
// definidos en secrets.h

// NTP - zona horaria Venezuela = UTC-4
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = -4 * 3600;
const int   DST_OFFSET_S = 0;

// ============================================================
//  PINES
// ============================================================

// OLED I2C (ESP32C3 Super Mini: SDA=8, SCL=9)
#define OLED_SDA  8
#define OLED_SCL  9
#define SCREEN_W  128
#define SCREEN_H  64
#define OLED_ADDR 0x3C

// ZMPT101B
#define PIN_SENSOR 3 // GPIO3 → OUT del sensor

// ============================================================
//  CONSTANTES
// ============================================================
#define OLED_UPDATE_MS 1000   // refresco pantalla
#define MAX_REGISTROS 60     // buffer NVS

// Detección RMS
#define TIEMPO_MUESTRAS 100 // ms de muestreo
#define UMBRAL_RMS 250

// ============================================================
//  VARIABLES GLOBALES
// ============================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences nvs;

// Estado del sensor
bool luzPresente = true;
bool lecturaAnterior = true;
int  amplitudADC = 0;

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

// Banderas de envío HTTP
bool envioFallaPendiente = false;
bool envioRetornoPendiente = false;

// Datos del evento actual
int contadorFallas = 0;
String horaFalla = "";
String horaRetorno = "";
String duracionStr = "";

// NVS sincronización
int  pendientesEnNVS = 0;
bool sincronizando = false;

// ============================================================
//  PROTOTIPOS
// ============================================================
bool medirLuz();
void guardarEnNVS(String tipo, String hora, unsigned long durSeg, String notas);
void sincronizarUnPendiente();
int contarPendientes();
void cargarContadores();
bool enviarAGoogleSheets(String tipo, String hora, String duracion, String notas);
String obtenerHora();
String segundosAHMS(unsigned long seg);
void mostrarEstado();
void mostrarMensaje(String linea1, String linea2 = "", String linea3 = "");

// ============================================================
//  SETUP
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== DETECTOR DE FALLA DE LUZ v5.0 ===");

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

  // NVS
  nvs.begin("falluzvzla", false);
  cargarContadores();
  pendientesEnNVS = contarPendientes();
  Serial.println("[NVS] Total: " + String(nvs.getInt("total", 0)) + " | Pendientes: " + String(pendientesEnNVS));

  // WiFi — sin bloquear
  mostrarMensaje("Conectando", "WiFi...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // baja a 8.5dBm
  WiFi.setSleep(false); // evita que el WiFi entre en modo ahorro y pierda conexión
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Esperar máximo 10s mostrando progreso en pantalla
  unsigned long inicioWifi = millis();
  int intentosMostrados = 0;

  while (WiFi.status() != WL_CONNECTED && millis() - inicioWifi < 10000)
  {
    delay(500);
    intentosMostrados++;
    mostrarMensaje(
      "Conectando WiFi",
      WIFI_SSID,
      String(intentosMostrados / 2) + "s / 10s"
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

  // Lectura inicial del sensor
  lecturaAnterior = medirLuz();
  luzPresente     = lecturaAnterior;

  Serial.println("[OK] Sistema listo. RMS inicial: " + String(amplitudADC) + " | Estado: " + String(luzPresente ? "LUZ" : "SIN LUZ"));
  mostrarEstado();
}

// ============================================================
//  LOOP PRINCIPAL
// ============================================================
void loop()
{
  unsigned long ahora = millis();

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

      // Marcar NVS inicial
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
      duracionFalla = (ahora - tiempoFalla) / 1000;
      horaRetorno = obtenerHora();
      duracionStr = segundosAHMS(duracionFalla);

      Serial.println("\n[RETORNO] " + horaRetorno + " | Dur: " + duracionStr + " | RMS:" + String(amplitudADC));

      // Marcar NVS final
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

  // ENVÍO INMEDIATO SI HAY WIFI
  if (envioFallaPendiente && WiFi.status() == WL_CONNECTED)
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
      Serial.println("[WARN] Falla guardada en NVS, sin internet");
    }
  }

  if (envioRetornoPendiente && WiFi.status() == WL_CONNECTED)
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
      Serial.println("[WARN] Retorno guardado en NVS, sin internet");
    }
  }

  // Verificar WiFi cada 30 segundos
  if (ahora - ultimoWifiCheck >= 30000)
  {
    ultimoWifiCheck = ahora;

    if (WiFi.status() != WL_CONNECTED)
    {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.println("[WiFi] Reintentando...");
    }
  }

  // Sincronizar pendientes cada 60 segundos si hay WiFi
  if (WiFi.status() == WL_CONNECTED &&
      pendientesEnNVS > 0 &&
      !sincronizando &&
      (ahora - ultimoSync >= 60000))
  {
    ultimoSync = ahora;
    sincronizarUnPendiente();
  }

  // ACTUALIZAR PANTALLA CADA SEGUNDO
  if (ahora - ultimoDisplay >= OLED_UPDATE_MS)
  {
    ultimoDisplay = ahora;
    mostrarEstado();
  }
}

// ============================================================
//  MEDIR LUZ POR RMS
// ============================================================
bool medirLuz()
{
  long sumaLineal = 0;
  int  muestras = 0;

  // Pasada 1: calcular centro real del ADC
  unsigned long inicio = millis();

  while (millis() - inicio < TIEMPO_MUESTRAS)
  {
    sumaLineal += analogRead(PIN_SENSOR);
    muestras++;
  }

  int centroReal = (int)(sumaLineal / muestras);

  // Pasada 2: calcular RMS con el centro real
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
//  GUARDAR EN NVS
// ============================================================
void guardarEnNVS(String tipo, String hora, unsigned long durSeg, String notas)
{
  int total = nvs.getInt("total", 0);

  if (total >= MAX_REGISTROS)
  {
    for (int i = 0; i < MAX_REGISTROS - 1; i++)
    {
      nvs.putString(("tp"  + String(i)).c_str(), nvs.getString(("tp"  + String(i + 1)).c_str(), ""));
      nvs.putString(("hr"  + String(i)).c_str(), nvs.getString(("hr"  + String(i + 1)).c_str(), ""));
      nvs.putULong( ("dr"  + String(i)).c_str(), nvs.getULong( ("dr"  + String(i + 1)).c_str(), 0));
      nvs.putString(("nt"  + String(i)).c_str(), nvs.getString(("nt"  + String(i + 1)).c_str(), ""));
      nvs.putBool(  ("env" + String(i)).c_str(), nvs.getBool(  ("env" + String(i + 1)).c_str(), false));
    }
    total = MAX_REGISTROS - 1;
  }

  String idx = String(total);
  nvs.putString(("tp"  + idx).c_str(), tipo.c_str());
  nvs.putString(("hr"  + idx).c_str(), hora.c_str());
  nvs.putULong( ("dr"  + idx).c_str(), durSeg);
  nvs.putString(("nt"  + idx).c_str(), notas.c_str());
  nvs.putBool(  ("env" + idx).c_str(), false);
  nvs.putInt("total", total + 1);

  pendientesEnNVS = contarPendientes();
  Serial.println("[NVS] Guardado #" + idx + " | " + tipo);
}

// ============================================================
//  SINCRONIZAR PENDIENTES CON GOOGLE SHEETS
// ============================================================
void sincronizarUnPendiente()
{
  int total = nvs.getInt("total", 0);

  for (int i = 0; i < total; i++)
  {
    String envKey  = "env" + String(i);
    bool yaEnviado = nvs.getBool(envKey.c_str(), false);

    if (yaEnviado) continue;

    String        tipo  = nvs.getString(("tp" + String(i)).c_str(), "");
    String        hora  = nvs.getString(("hr" + String(i)).c_str(), "");
    unsigned long dur   = nvs.getULong( ("dr" + String(i)).c_str(), 0);
    String        notas = nvs.getString(("nt" + String(i)).c_str(), "");

    if (tipo == "") continue;

    Serial.println("[SYNC] Enviando #" + String(i) + ": " + tipo);

    bool ok = enviarAGoogleSheets(tipo, hora, String(dur), notas);

    if (ok)
    {
      nvs.putBool(envKey.c_str(), true);
      pendientesEnNVS = contarPendientes();
      Serial.println("[SYNC] #" + String(i) + " OK. Pendientes: " + String(pendientesEnNVS));
    }
    else
    {
      Serial.println("[SYNC] #" + String(i) + " falló, reintentará en 60s");
    }

    // Solo un registro por ciclo para no bloquear
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
//  CARGAR CONTADORES
// ============================================================
void cargarContadores()
{
  contadorFallas = nvs.getInt("fallasTot", 0);
  Serial.println("[NVS] Fallas históricas: " + String(contadorFallas));
}

// ============================================================
//  ENVIAR A GOOGLE SHEETS
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

  Serial.println("[HTTP] Enviando a: " + url.substring(0, 60) + "...");

  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(3000);   // reducido a 3s para no bloquear tanto

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
//  OBTENER HORA
// ============================================================
String obtenerHora()
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo))
  {
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
//  MOSTRAR ESTADO EN OLED
// ============================================================
void mostrarEstado()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (!luzPresente)
  {
    // PANTALLA DE FALLA

    static bool parpadeo = false;
    parpadeo = !parpadeo;
    display.setTextSize(1);
    display.setCursor(4, 0);
    if (parpadeo) display.println("SIN ELECTRICIDAD");

    // Contador HH:MM:SS en grande
    unsigned long durActual = (millis() - tiempoFalla) / 1000;
    unsigned long h = durActual / 3600;
    unsigned long m = (durActual % 3600) / 60;
    unsigned long s = durActual % 60;

    display.setTextSize(2);
    display.setCursor(0, 16);
    char buf[9];
    sprintf(buf, "%02lu:%02lu:%02lu", h, m, s);
    display.println(buf);

    display.setTextSize(1);
    display.setCursor(0, 40);
    display.print("Desde: ");

    if (horaFalla.length() >= 19)
    {
      display.println(horaFalla.substring(11, 16));
    }
    else
    {
      display.println(horaFalla);
    }

    display.setCursor(0, 52);
    display.print("F#");
    display.print(contadorFallas);
    display.print(" RMS:");
    display.println(amplitudADC);
  }
  else
  {
    // PANTALLA NORMAL

    display.setTextSize(1);
    display.setCursor(4, 0);
    display.println("HAY ELECTRICIDAD");

    display.setCursor(40, 18);
    String hora = obtenerHora();

    if (hora.length() >= 19)
    {
      display.println(hora.substring(11));
    }
    else
    {
      display.println(hora);
    }

    display.setCursor(0, 30);

    if (horaFalla != "" && duracionFalla > 0)
    {
      display.print("Ult falla: ");

      if (horaFalla.length() >= 19)
      {
        display.println(horaFalla.substring(11, 16));
      }
      else
      {
        display.println(horaFalla);
      }

      display.setCursor(0, 42);
      display.print("Duracion:  ");
      display.println(duracionStr);
    }
    else
    {
      display.println("Sin fallas");
    }

    display.setCursor(0, 54);
    display.print("RMS:");
    display.print(amplitudADC);
    display.print(" ");

    if (pendientesEnNVS > 0)
    {
      display.print("NVS:");
      display.println(pendientesEnNVS);
    }
    else
    {
      display.println(WiFi.status() == WL_CONNECTED ? "WiFi SI" : "Sin WiFi");
    }
  }

  display.display();
}

// ============================================================
//  MOSTRAR MENSAJE EN OLED
// ============================================================
void mostrarMensaje(String linea1, String linea2, String linea3)
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 10);
  display.println(linea1);

  display.setCursor(0, 28);
  display.println(linea2);

  display.setCursor(0, 46);
  display.println(linea3);

  display.display();
}
