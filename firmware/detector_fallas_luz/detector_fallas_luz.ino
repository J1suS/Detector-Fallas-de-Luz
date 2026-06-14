/*
 * ============================================================
  DETECTOR DE FALLA DE LUZ - VENEZUELA
  Versión: 3.1 - Integración de batería 18659 + TP4056

  Cambios respecto a v3.0:
  - Alimentación autónoma por batería 18650 vía TP4056
  - Validada la autonomía sin conexión USB

  Hardware:
  - Arduino ESP32C3 Super Mini
  - Batería LiPo 18650 3000mAh conectada via JST
  - Módulo TP4056 con protección
  - Sensor ZMPT101B  (simulado con cable jumper)
  - Pantalla OLED 0.96" SSD1306 via I2C
  - Conexión WiFi (actualmente router) [pendiente el BAM USB]

  Servicios externos:
  - Google Sheets (via Google Apps Script Web App)
  - NTP para hora exacta

  Jesús Rivas && Sofía Karabin
  Ing. de Sistemas
  UNEFA
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

// zona horaria = UTC-4
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = -4 * 3600;
const int   DST_OFFSET_S = 0;

// ============================================================
//  PINES
// ============================================================

// OLED I2C  (ESP32C3 Super Mini: SDA=8, SCL=9)
#define OLED_SDA  8
#define OLED_SCL  9
#define SCREEN_W  128
#define SCREEN_H  64
#define OLED_ADDR 0x3C

/*
SENSOR DE VOLTAJE:
  - Fase real:   salida digital del ZMPT101B (HIGH = hay luz, LOW = sin luz)
  - Fase prueba: cable jumper entre GPIO3 y GND

  Para simular falla:   conectar el cable a una línea GND
  Para simular retorno: desconectar cable
*/

#define PIN_SENSOR 3   // GPIO3

// LED interno del ESP32C3 Super Mini
#define PIN_LED 2

// ============================================================
//  CONSTANTES
// ============================================================
#define RECONNECT_TIMEOUT 15000 // ms máximo esperando reconexión WiFi
#define OLED_UPDATE_MS 1000 // actualiza pantalla cada 1 segundo
#define MAX_REGISTROS 60  // máximo de registros en NVS sin sincronizar

// ============================================================
//  VARIABLES GLOBALES
// ============================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Preferences nvs;

// Estado del sensor
bool luzPresente = true;
bool lecturaAnterior = true;

// Temporización
unsigned long tiempoFalla = 0;
unsigned long duracionFalla = 0;
unsigned long ultimoDisplay = 0;
unsigned long ultimoWifiCheck = 0;
unsigned long ultimoSync = 0;

// Banderas de envío
bool envioFallaPendiente = false;
bool envioRetornoPendiente = false;

// Datos del evento actual
int contadorFallas = 0;
String horaFalla = "";
String horaRetorno = "";
String duracionStr = "";

// Estado de sincronización
int pendientesEnNVS = 0;
bool sincronizando = false;

// ============================================================
//  PROTOTIPOS
// ============================================================
void conectarWiFi();
bool enviarAGoogleSheets(String tipo, String hora, String duracion, String notas);
void guardarEnNVS(String tipo, String hora, unsigned long durSeg, String notas);
void sincronizarPendientes();
int contarPendientes();
void cargarContadores();
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
  Serial.println("\n\n=== DETECTOR DE FALLA DE LUZ v3.1 ===");

  // I2C y OLED
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("[ERROR] OLED no encontrada");
  }

  // Sensor
  pinMode(PIN_SENSOR, INPUT_PULLUP);

  // NVS — cargar datos persistidos de sesiones anteriores
  nvs.begin("falluzvzla", false);
  cargarContadores();
  pendientesEnNVS = contarPendientes();
  Serial.println("[NVS] Registros totales: " + String(nvs.getInt("total", 0)));
  Serial.println("[NVS] Pendientes por sincronizar: " + String(pendientesEnNVS));

  // WiFi
  mostrarMensaje("Conectando", "WiFi...", WIFI_SSID);
  conectarWiFi();

  // NTP
  mostrarMensaje("Sincronizando", "hora NTP...", "");
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);

  Serial.print("[NTP] Esperando sincronización");
  struct tm timeinfo;
  int intentosNTP = 0;

  while (!getLocalTime(&timeinfo) && intentosNTP < 20)
  {
    Serial.print(".");
    delay(500);
    intentosNTP++;
  }

  if (intentosNTP < 20)
  {
    Serial.println("\n[OK] Hora sincronizada: " + obtenerHora());
  }
  else
  {
    Serial.println("\n[WARN] Sin hora NTP, usando millis()");
  }

  // Si hay WiFi al arrancar, sincronizar pendientes de sesiones anteriores
  if (WiFi.status() == WL_CONNECTED && pendientesEnNVS > 0)
  {
    mostrarMensaje("Sincronizando", String(pendientesEnNVS) + " pendientes", "espera...");
    sincronizarPendientes();
  }

  // Estado inicial del sensor
  lecturaAnterior = (digitalRead(PIN_SENSOR) == HIGH);
  luzPresente     = lecturaAnterior;

  Serial.println("[OK] Sistema listo. Sensor: " + String(luzPresente ? "LUZ" : "SIN ELECTRICIDAD"));
  mostrarEstado();
}

// ============================================================
//  LOOP PRINCIPAL
// ============================================================
void loop()
{
  unsigned long ahora = millis();

  // LEER SENSOR
  bool lecturaActual = (digitalRead(PIN_SENSOR) == HIGH);

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
      nvs.putInt("fallasTot", contadorFallas);

      Serial.println("\n[FALLA] " + horaFalla + " | Falla #" + String(contadorFallas));

      // Guardar en NVS primero, antes de cualquier otra cosa
      guardarEnNVS("FALLA_INICIO", horaFalla, 0, "Falla #" + String(contadorFallas));

      envioFallaPendiente = true;
      envioRetornoPendiente = false;
    }
    else
    {
      // VOLVIÓ LA LUZ
      duracionFalla = (ahora - tiempoFalla) / 1000;
      horaRetorno = obtenerHora();
      duracionStr = segundosAHMS(duracionFalla);

      Serial.println("\n[RETORNO] " + horaRetorno + " | Duración: " + duracionStr);

      // Guardar en NVS primero
      guardarEnNVS("FALLA_FIN", horaRetorno, duracionFalla, "Dur: " + duracionStr + " | Inicio: " + horaFalla);

      envioRetornoPendiente = true;
      envioFallaPendiente = false;
    }

    // Actualizar pantalla inmediatamente al cambiar estado
    mostrarEstado();
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
      Serial.println("[OK] Falla enviada y marcada en NVS");
    }
    else
    {
      Serial.println("[WARN] Sin internet, quedó guardada en NVS");
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
      Serial.println("[OK] Retorno enviado y marcado en NVS");
    }
    else
    {
      Serial.println("[WARN] Sin internet, quedó guardada en NVS");
    }
  }

  // Verificar WiFi cada 30 segundos
  if (ahora - ultimoWifiCheck >= 30000)
  {
    ultimoWifiCheck = ahora;

    if (WiFi.status() != WL_CONNECTED)
    {
      conectarWiFi();
    }
  }

  // Si hay WiFi y pendientes, sincronizar cada 60 segundos
  if (WiFi.status() == WL_CONNECTED && pendientesEnNVS > 0 && !sincronizando && (ahora - ultimoSync >= 60000))
  {
    ultimoSync = ahora;
    sincronizarPendientes();
  }

  // ACTUALIZAR PANTALLA CADA SEGUNDO
  if (ahora - ultimoDisplay >= OLED_UPDATE_MS)
  {
    ultimoDisplay = ahora;
    mostrarEstado();
  }
}

// ============================================================
//  GUARDAR REGISTRO NVS
// ============================================================
void guardarEnNVS(String tipo, String hora, unsigned long durSeg, String notas)
{
  int total = nvs.getInt("total", 0);

  if (total >= MAX_REGISTROS)
  {
    // desplazar registros (borrar el más antiguo)
    Serial.println("[NVS] Buffer lleno, descartando registro más antiguo");

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

  // Guardar nuevo registro
  String idx = String(total);
  nvs.putString(("tp"  + idx).c_str(), tipo.c_str());
  nvs.putString(("hr"  + idx).c_str(), hora.c_str());
  nvs.putULong( ("dr"  + idx).c_str(), durSeg);
  nvs.putString(("nt"  + idx).c_str(), notas.c_str());
  nvs.putBool(  ("env" + idx).c_str(), false);   // no enviado aún
  nvs.putInt("total", total + 1);

  pendientesEnNVS = contarPendientes();

  Serial.println("[NVS] Guardado registro #" + idx + " | " + tipo + " | " + hora);
}

// ============================================================
//  SINCRONIZAR PENDIENTES CON GOOGLE SHEETS
// ============================================================
void sincronizarPendientes()
{
  int total = nvs.getInt("total", 0);
  if (total == 0) return;

  sincronizando = true;
  int enviados  = 0;
  int fallidos  = 0;

  Serial.println("[SYNC] Iniciando sincronización de " + String(total) + " registros...");

  for (int i = 0; i < total; i++)
  {
    String envKey    = "env" + String(i);
    bool   yaEnviado = nvs.getBool(envKey.c_str(), false);

    if (yaEnviado) continue;   // saltar los que ya están en Sheets

    // Leer registro de NVS
    String        tipo  = nvs.getString(("tp" + String(i)).c_str(), "");
    String        hora  = nvs.getString(("hr" + String(i)).c_str(), "");
    unsigned long dur   = nvs.getULong( ("dr" + String(i)).c_str(), 0);
    String        notas = nvs.getString(("nt" + String(i)).c_str(), "");

    if (tipo == "") continue;   // registro vacío, saltar

    Serial.println("[SYNC] Enviando #" + String(i) + ": " + tipo + " | " + hora);

    // Mostrar progreso en OLED
    mostrarMensaje(
      "Sincronizando",
      String(i + 1) + "/" + String(total),
      tipo == "FALLA_INICIO" ? "Falla: " + hora.substring(11, 16) : "Retorno: " + hora.substring(11, 16)
    );

    bool ok = enviarAGoogleSheets(tipo, hora, String(dur), notas);

    if (ok)
    {
      nvs.putBool(envKey.c_str(), true);
      enviados++;
      Serial.println("[SYNC] #" + String(i) + " enviado OK");
    }
    else
    {
      fallidos++;
      Serial.println("[SYNC] #" + String(i) + " falló, reintentará luego");
      break;   // si falla uno, no hay internet, detener
    }

    delay(500);   // pausa entre envíos para no saturar el script
  }

  pendientesEnNVS = contarPendientes();
  sincronizando   = false;

  Serial.println("[SYNC] Completado. Enviados: " + String(enviados) + " | Fallidos: " + String(fallidos) + " | Pendientes: " + String(pendientesEnNVS));
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
    bool enviado = nvs.getBool(("env" + String(i)).c_str(), false);
    if (!enviado) pendientes++;
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

  Serial.println("[HTTP] Enviando a: " + url.substring(0, 60) + "...");

  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);

  int httpCode = http.GET();

  if (httpCode > 0)
  {
    String respuesta = http.getString();
    Serial.println("[HTTP] Código: " + String(httpCode));
    Serial.println("[HTTP] Respuesta: " + respuesta);
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

  Serial.print("[WiFi] Conectando a " + String(WIFI_SSID));
  unsigned long inicio = millis();

  while (WiFi.status() != WL_CONNECTED && (millis() - inicio) < RECONNECT_TIMEOUT)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\n[WiFi] Conectado! IP: " + WiFi.localIP().toString());
    Serial.println("[WiFi] RSSI: " + String(WiFi.RSSI()) + " dBm");
  }
  else
  {
    Serial.println("\n[WiFi] FALLO al conectar. Continuando sin internet...");
  }
}

// ============================================================
//  OBTENER HORA COMO STRING
// ============================================================
String obtenerHora()
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo))
  {
    // Fallback: usar millis si no hay NTP
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

  if (!luzPresente)
  {
    // PANTALLA DE FALLA

    // Título parpadeante
    static bool parpadeo = false;
    parpadeo = !parpadeo;
    display.setTextSize(1);
    display.setCursor(0, 0);

    if (parpadeo) display.println("!!! SIN LUZ !!!");

    // Contador HH:MM:SS en tamaño grande
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
      display.println(horaFalla.substring(11, 16));   // HH:MM
    }
    else
    {
      display.println(horaFalla);
    }

    display.setCursor(0, 52);
    display.print("F#");
    display.print(contadorFallas);
    display.print(" ");

    if (pendientesEnNVS > 0)
    {
      display.print("NVS:");
      display.print(pendientesEnNVS);
    }
    else
    {
      display.println(WiFi.status() == WL_CONNECTED ? "WiFi SI" : "Sin WiFi");
    }
  }
  else
  {
    // PANTALLA NORMAL

    display.setTextSize(1);
    display.setCursor(4, 0);
    display.println(" HAY ELECTRICIDAD ");

    // Hora actual
    display.setCursor(40, 18);
    String hora = obtenerHora();

    if (hora.length() >= 19)
    {
      display.setTextSize(1);
      display.println(hora.substring(11));   // HH:MM:SS
    }
    else
    {
      display.println(hora);   // fallback T+xx:xx:xx si no hay NTP
    }

    // Última falla y su duración
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

    // WiFi, pendientes y contador
    display.setCursor(0, 54);

    if (pendientesEnNVS > 0)
    {
      display.print("Pendientes NVS: ");
      display.println(pendientesEnNVS);
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
      display.print("WiFi SI | F:");
      display.println(contadorFallas);
    }
    else
    {
      display.println("WiFi: SIN CONEXION");
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
