/*
 * ============================================================
  DETECTOR DE FALLA DE LUZ - VENEZUELA
  Versión: 2.0 - Detección instantánea, sin delays bloqueantes

  Cambios respecto a v1.0:
  - Eliminado debounce: detección al instante del corte
  - Eliminados todos los delay() del loop principal
  - Envío a Google Sheets desacoplado de la detección
  - Contador arranca en el preciso momento de la falla

  Hardware:
  - Arduino ESP32C3 Super Mini
  - Sensor ZMPT101B  (en esta fase: simulado con cable jumper)
  - Pantalla OLED 0.96" SSD1306 via I2C
  - Conexión WiFi (actualmente router)

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
#include <time.h>
#include "secrets.h"

// ============================================================
//  CONFIGURACIÓN
// ============================================================

// WIFI_SSID, WIFI_PASSWORD y GOOGLE_SCRIPT_URL
// vienen definidos en secrets.h

// NTP - zona horaria Venezuela = UTC-4
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_S = -4 * 3600;
const int   DST_OFFSET_S = 0;

// ============================================================
//  PINES
// ============================================================

// OLED I2C  (ESP32C3 Super Mini: SDA=8, SCL=9)
#define OLED_SDA 8
#define OLED_SCL 9
#define SCREEN_W 128
#define SCREEN_H 64
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

// ============================================================
//  VARIABLES GLOBALES
// ============================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// Estado del sensor
bool luzPresente = true;
bool lecturaAnterior = true;

// Temporización
unsigned long tiempoFalla = 0; // millis() exacto del corte
unsigned long duracionFalla = 0; // duración de última falla en segundos
unsigned long ultimoDisplay = 0;
unsigned long ultimoEnvio = 0;

// Banderas de envío
bool envioFallaPendiente = false;
bool envioRetornoPendiente = false;

// Datos del evento actual
int contadorFallas = 0;
String horaFalla = "";
String horaRetorno = "";
String duracionStr = "";

// ============================================================
//  PROTOTIPOS
// ============================================================
void conectarWiFi();
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
  Serial.println("\n\n=== DETECTOR DE FALLA DE LUZ v2.0 ===");

  // I2C y OLED
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("[ERROR] OLED no encontrada");
  }

  // Sensor
  pinMode(PIN_SENSOR, INPUT_PULLUP);

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

  // Estado inicial del sensor
  lecturaAnterior = (digitalRead(PIN_SENSOR) == HIGH);
  luzPresente     = lecturaAnterior;

  Serial.println("[OK] Sistema listo. Sensor: " + String(luzPresente ? "LUZ" : "SIN LUZ"));
  mostrarEstado();
}

// ============================================================
//  LOOP PRINCIPAL — sin delays bloqueantes
// ============================================================
void loop()
{
  unsigned long ahora = millis();

  // LEER SENSOR
  bool lecturaActual = (digitalRead(PIN_SENSOR) == HIGH);

  if (lecturaActual != lecturaAnterior)
  {
    lecturaAnterior = lecturaActual;
    luzPresente     = lecturaActual;

    if (!lecturaActual)
    {
      // SE FUE LA LUZ
      tiempoFalla   = ahora;          // captura exacta del instante
      horaFalla     = obtenerHora();
      duracionFalla = 0;
      contadorFallas++;

      Serial.println("\n[FALLA] " + horaFalla + " | Falla #" + String(contadorFallas));

      envioFallaPendiente   = true;
      envioRetornoPendiente = false;
    }
    else
    {
      // VOLVIÓ LA LUZ
      duracionFalla = (ahora - tiempoFalla) / 1000;
      horaRetorno   = obtenerHora();
      duracionStr   = segundosAHMS(duracionFalla);

      Serial.println("\n[RETORNO] " + horaRetorno + " | Duración: " + duracionStr);

      envioRetornoPendiente = true;
      envioFallaPendiente   = false;
    }

    // Actualizar pantalla inmediatamente al cambiar estado
    mostrarEstado();
  }

  // ENVÍO A GOOGLE SHEETS
  if (envioFallaPendiente && WiFi.status() == WL_CONNECTED)
  {
    envioFallaPendiente = false;
    bool ok = enviarAGoogleSheets("FALLA_INICIO", horaFalla, "0", "Falla #" + String(contadorFallas));
    Serial.println(ok ? "[OK] Falla enviada a Sheets" : "[ERROR] Falla no enviada");
  }

  if (envioRetornoPendiente && WiFi.status() == WL_CONNECTED)
  {
    envioRetornoPendiente = false;
    bool ok = enviarAGoogleSheets("FALLA_FIN", horaRetorno, String(duracionFalla), "Dur: " + duracionStr + " | Inicio: " + horaFalla);
    Serial.println(ok ? "[OK] Retorno enviado a Sheets" : "[ERROR] Retorno no enviado");
  }

  // Reintentar WiFi cada 30 segundos si no hay conexión
  if (WiFi.status() != WL_CONNECTED && (ahora - ultimoEnvio > 30000))
  {
    ultimoEnvio = ahora;
    conectarWiFi();
  }

  // --- 3. ACTUALIZAR PANTALLA CADA SEGUNDO ---
  if (ahora - ultimoDisplay >= OLED_UPDATE_MS)
  {
    ultimoDisplay = ahora;
    mostrarEstado();
  }
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
  url += "?tipo="     + tipo;
  url += "&hora="     + hora;
  url += "&duracion=" + duracion;
  url += "&notas="    + notas;
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
  display.setTextColor(SSD1306_WHITE);

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
    display.print("Falla #");
    display.print(contadorFallas);
    display.print("  ");
    display.println(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "Sin WiFi");
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

    // WiFi y contador
    display.setCursor(0, 54);

    if (WiFi.status() == WL_CONNECTED)
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
