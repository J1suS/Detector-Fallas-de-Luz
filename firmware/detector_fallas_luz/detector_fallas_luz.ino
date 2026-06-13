/*
 * ============================================================
  DETECTOR DE FALLA DE LUZ - VENEZUELA
  Versión: 1.0 - Validación USB (alimentación por USB)

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
#include "secrets.h"   // ← por mi seguridad :v

// ============================================================
//  CONFIGURACIÓN
// ============================================================

// WIFI_SSID, WIFI_PASSWORD y GOOGLE_SCRIPT_URL
// vienen definidos en secrets.h

// NTP - zona horaria Venezuela = UTC-4
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_S = -4 * 3600;   // UTC-4
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

  Para simular falla: conectar el cable a una linea GND
  Para simular retorno: desconectar cable
*/

#define PIN_SENSOR 3 // GPIO3

// LED interno del ESP32C3 Super Mini
#define PIN_LED 2

// ============================================================
//  CONSTANTES
// ============================================================
#define DEBOUNCE_MS 2000 // ms para confirmar cambio de estado (evitar falsos)
#define RECONNECT_TIMEOUT 15000  // ms máximo esperando reconexión WiFi
#define OLED_UPDATE_MS 1000 // actializa pantalla cada 1 segundo

// ============================================================
//  VARIABLES GLOBALES
// ============================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

bool luzPresente = true; // estado actual de la red eléctrica
bool estadoAnterior = true; // para detectar cambios
unsigned long tiempoFalla = 0; // millis() cuando se fue la luz
unsigned long duracionFalla = 0; // duración de la última falla en segundos

unsigned long ultimoDisplayUpdate = 0;
unsigned long ultimoDebounce = 0;
bool estadoPendiente = false;
bool nuevoEstadoPendiente = false;

int contadorFallas = 0; // fallas en esta sesión
String horaFalla = ""; // timestamp de inicio de falla
String horaRetorno = ""; // timestamp de retorno

// ============================================================
//  PROTOTIPOS
// ============================================================
void conectarWiFi();
bool enviarAGoogleSheets(String tipo, String hora, String duracion, String notas);
String obtenerHora();
void mostrarEstado();
void mostrarMensaje(String linea1, String linea2 = "", String linea3 = "");
void procesarCambioEstado(bool nuevaLuz);
String segundosAHMS(unsigned long seg);

// ============================================================
//  SETUP
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== DETECTOR DE FALLA DE LUZ ===");

  // I2C para OLED
  Wire.begin(OLED_SDA, OLED_SCL);

  // Iniciar OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("[ERROR] OLED no encontrada");
  }
  else
  {
    Serial.println("[OK] OLED inicializada");
    mostrarMensaje("Iniciando...", "Detector de Luz", "v1.0");
    delay(1500);
  }

  // Configurar pin del sensor/botón
  pinMode(PIN_SENSOR, INPUT_PULLUP);

  // Conectar WiFi
  mostrarMensaje("Conectando", "WiFi...", WIFI_SSID);
  conectarWiFi();

  // Sincronizar hora NTP
  mostrarMensaje("Sincronizando", "hora NTP...", "");
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);

  // Esperar hasta obtener hora válida
  Serial.print("[NTP] Esperando sincronización");
  int intentosNTP = 0;
  struct tm timeinfo;

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

  // Leer estado inicial del sensor
  luzPresente = (digitalRead(PIN_SENSOR) == HIGH);
  estadoAnterior = luzPresente;

  mostrarMensaje("Sistema listo", luzPresente ? "Luz: PRESENTE" : "Luz: AUSENTE", "");
  delay(1500);

  ultimoDisplayUpdate = 0; // Forzar actualización inmediata
  mostrarEstado();

  Serial.println("[OK] Sistema listo. Estado inicial: " + String(luzPresente ? "LUZ" : "SIN LUZ"));
}

// ============================================================
//  LOOP PRINCIPAL
// ============================================================
void loop()
{
  bool lecturaSensor = (digitalRead(PIN_SENSOR) == HIGH); // HIGH = hay luz

  // Detección de cambio con debounce
  if (lecturaSensor != luzPresente)
  {
    if (!estadoPendiente)
    {
      estadoPendiente    = true;
      nuevoEstadoPendiente = lecturaSensor;
      ultimoDebounce     = millis();
    }
    else if (lecturaSensor == nuevoEstadoPendiente && (millis() - ultimoDebounce) >= DEBOUNCE_MS)
    {
      // Cambio confirmado
      estadoPendiente = false;
      procesarCambioEstado(lecturaSensor);
    }
    else if (lecturaSensor != nuevoEstadoPendiente)
    {
      // Fluctuación, resetear debounce
      estadoPendiente = false;
    }
  }
  else
  {
    estadoPendiente = false;
  }

  // Actualizar pantalla cada segundo
  if (millis() - ultimoDisplayUpdate >= OLED_UPDATE_MS)
  {
    ultimoDisplayUpdate = millis();
    mostrarEstado();
  }

  delay(50);
}

// ============================================================
//  PROCESAR CAMBIO DE ESTADO
// ============================================================
void procesarCambioEstado(bool nuevaLuz)
{
  luzPresente = nuevaLuz;
  String hora = obtenerHora();

  if (!nuevaLuz)
  {
    //SE FUE LA LUZ
    contadorFallas++;
    tiempoFalla = millis();
    horaFalla   = hora;
    duracionFalla = 0;

    Serial.println("\n[FALLA] Corte detectado a las " + hora);
    Serial.println("[INFO] Falla #" + String(contadorFallas) + " en esta sesión");

    mostrarMensaje("! FALLA DE LUZ !", hora, "Registrando...");

    // Reconectar WiFi si es necesario
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("[WiFi] Reconectando...");
      conectarWiFi();
    }

    // Enviar registro de inicio de falla
    bool ok = enviarAGoogleSheets("FALLA_INICIO", hora, "0", "Falla #" + String(contadorFallas));

    if (ok)
    {
      Serial.println("[OK] Falla registrada en Google Sheets");
      mostrarMensaje("! SIN LUZ !", "Registrado OK", hora);
    }
    else
    {
      Serial.println("[ERROR] No se pudo registrar. Reintentando en loop...");
      mostrarMensaje("! SIN LUZ !", "Error al enviar", "Reintentando...");
      // Podrías guardar en EEPROM aquí para mayor robustez
    }

  }
  else
  {
    //VOLVIÓ LA LUZ
    horaRetorno   = hora;
    duracionFalla = (millis() - tiempoFalla) / 1000; // en segundos
    String durStr = segundosAHMS(duracionFalla);

    Serial.println("\n[RETORNO] Luz restaurada a las " + hora);
    Serial.println("[INFO] Duración de falla: " + durStr);

    mostrarMensaje("RETORNO LUZ", hora, "Dur: " + durStr);

    // Reconectar WiFi si es necesario
    if (WiFi.status() != WL_CONNECTED)
    {
      conectarWiFi();
    }

    // Enviar registro de fin de falla
    bool ok = enviarAGoogleSheets("FALLA_FIN", hora, String(duracionFalla), "Duracion: " + durStr + " | Inicio: " + horaFalla);

    if (ok)
    {
      Serial.println("[OK] Retorno registrado en Google Sheets");
      mostrarMensaje("LUZ PRESENTE", "Registrado OK", "Dur: " + durStr);
    }
    else
    {
      Serial.println("[ERROR] No se pudo registrar retorno.");
      mostrarMensaje("LUZ PRESENTE", "Error al enviar", "Dur: " + durStr);
    }
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
  client.setInsecure(); // Para HTTPS sin validar certificado (suficiente para este uso)

  HTTPClient http;

  // Construir URL con parámetros GET
  String url = String(GOOGLE_SCRIPT_URL);
  url += "?tipo="     + tipo;
  url += "&hora="     + hora;
  url += "&duracion=" + duracion;
  url += "&notas="    + notas;

  // Reemplazar espacios por %20
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
//  MOSTRAR ESTADO EN OLED (actualización periódica)
// ============================================================
void mostrarEstado()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (!luzPresente)
  {
    //PANTALLA DE FALLA
    
    // Título parpadeante
    static bool parpadeo = false;
    parpadeo = !parpadeo;
    display.setTextSize(1);
    display.setCursor(0, 0);
    if (parpadeo) display.println("!!! SIN LUZ !!!");

    // Contador de segundos
    unsigned long durActual = (millis() - tiempoFalla) / 1000;
    unsigned long h = durActual / 3600;
    unsigned long m = (durActual % 3600) / 60;
    unsigned long s = durActual % 60;

    display.setTextSize(2);
    display.setCursor(0, 16);
    char buf[12];
    sprintf(buf, "%02lu:%02lu:%02lu", h, m, s);
    display.println(buf);

    // Hora de inicio de falla
    display.setTextSize(1);
    display.setCursor(0, 40);
    display.print("Desde: ");

    if (horaFalla.length() >= 19)
    {
      display.println(horaFalla.substring(11, 16)); // HH:MM
    }
    else
    {
      display.println(horaFalla);
    }

    // Falla número
    display.setCursor(0, 52);
    display.print("Falla #");
    display.println(contadorFallas);

  }
  else
  {
    //PANTALLA NORMAL

    // Estado
    display.setTextSize(1);
    display.setCursor(4, 0);
    display.println(" HAY ELECTRICIDAD ");

    // Hora actual
    display.setCursor(40, 18);
    String hora = obtenerHora();

    if (hora.length() >= 19)
    {
      display.setTextSize(1);
      display.println(hora.substring(11)); // HH:MM:SS
    }
    else
    {
      display.println(hora); // fallback T+xx:xx:xx si no hay NTP
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
      display.print("Duracion: ");
      display.println(segundosAHMS(duracionFalla));
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
//  MOSTRAR MENSAJE A OLED
// ============================================================
void mostrarMensaje(String linea1, String linea2, String linea3) {
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
