/*
 * ============================================================
 *  DETECTOR DE FALLA DE LUZ - VENEZUELA
 *  Versión: 3.2 - Primer test con el sensor ZMPT101B
 *
 *  Cambios respecto a v3.1:
 *  - Integración del módulo ZMPT101B código de prueba
 *    (sin batería ni TP4056 funcionando)
 *
 *  Hardware:
 *  - Arduino ESP32C3 Super Mini
 *  - Batería Li-ion 18650 3000mAh via JST   (deshabilitado)
 *  - Módulo TP4056 con protección            (deshabilitado)
 *  - Sensor ZMPT101B (VCC → 3V3, OUT → GPIO3)
 *  - Pantalla OLED 0.96" SSD1306 via I2C
 *  - Conexión WiFi: router local
 *
 *  Jesús Rivas && Sofía Karabin
 *  Ing. de Sistemas
 *  UNEFA
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
//  PINES
// ============================================================

// OLED I2C (ESP32C3 Super Mini: SDA=8, SCL=9)
#define OLED_SDA  8
#define OLED_SCL  9
#define SCREEN_W  128
#define SCREEN_H  64
#define OLED_ADDR 0x3C

#define PIN_SENSOR 3   // GPIO3 → OUT del ZMPT101B

// ============================================================
//  VARIABLES GLOBALES
// ============================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

int   valorADC  = 0;
float voltajeSalida = 0.0;

// ============================================================
//  PROTOTIPOS
// ============================================================
void mostrarEstado();

// ============================================================
//  SETUP
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== TEST DE SENSOR v3.2 ===");

  analogReadResolution(12);   // ADC de 12 bits (0-4095)

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    Serial.println("[ERROR] OLED no encontrada");
  }
  else
  {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.println("Iniciando sensor...");
    display.display();
    Serial.println("[OK] OLED inicializada");
    delay(1000);
  }
}

// ============================================================
//  LOOP PRINCIPAL
// ============================================================
void loop()
{
  valorADC      = analogRead(PIN_SENSOR);
  voltajeSalida = (valorADC * 3.3) / 4095.0;

  Serial.print("[ADC] ");
  Serial.print(valorADC);
  Serial.print(" | Voltaje OUT: ");
  Serial.print(voltajeSalida, 3);
  Serial.println(" V");

  mostrarEstado();

  delay(150);
}

// ============================================================
//  MOSTRAR ESTADO EN OLED
// ============================================================
void mostrarEstado()
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Título
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("=== CALIBRACION ===");

  // Valor ADC
  display.setCursor(0, 16);
  display.print("ADC (0-4095):");
  display.setTextSize(2);
  display.setCursor(0, 28);
  display.println(valorADC);

  // Voltaje en el pin OUT
  display.setTextSize(1);
  display.setCursor(0, 50);
  display.print("Voltaje OUT: ");
  display.print(voltajeSalida, 2);
  display.println(" V");

  display.display();
}