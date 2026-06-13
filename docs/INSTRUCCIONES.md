# DETECTOR DE FALLA DE LUZ — GUÍA COMPLETA
## Fase 1: Validación con alimentación USB

---

# LIBRERÍAS NECESARIAS (Arduino IDE)

Instarlas desde **Sketch → Include Library → Manage Libraries**:

| Librería                 | Autor     | Para qué sirve  |
|--------------------------|-----------|-----------------|
| `Adafruit SSD1306`       | Adafruit  | Pantalla OLED   |
| `Adafruit GFX Library`   | Adafruit  | Gráficos        |

Las librerías `WiFi`, `HTTPClient`, `Wire` y `time.h` vienen incluidas con el **ESP32 Arduino Core**.

### Instalar soporte ESP32 en Arduino IDE
1. Archivo → Preferencias → URLs adicionales:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. Herramientas → Placa → Gestor de placas → Buscar "esp32" → Instalar el de **Espressif Systems**
3. Seleccionar placa: **ESP32C3 Dev Module**

---

## CONEXIONES DE HARDWARE

### OLED 0.96" SSD1306 → ESP32C3 Super Mini

```
OLED VCC  →  3.3V del ESP32C3
OLED GND  →  GND del ESP32C3
OLED SDA  →  GPIO 8 (SDA)
OLED SCL  →  GPIO 9 (SCL)
```

### Botón simulador de sensor (para pruebas)

```
Un extremo del botón  →  GPIO 3 del ESP32C3
Otro extremo          →  GND del ESP32C3
```

> El código usa `INPUT_PULLUP`, así que:
> - **Botón suelto** = GPIO3 en HIGH = "Hay luz" ✅
> - **Botón presionado** = GPIO3 en LOW = "Se fue la luz" ❌

### Tabla I2C del ESP32C3 Super Mini
| Función|GPIO |
|--------|-----|
| SDA    | 8   |
| SCL    | 9   |

---

## GOOGLE SHEETS

### Paso 1 — Crear la hoja de cálculo
1. Ir a [sheets.google.com](https://sheets.google.com)
2. Crear una hoja nueva
3. Cambiar el nombre a: **`Detector de Fallas`**

### Paso 2 — Crear el Apps Script
1. En la hoja abierta: **Extensiones → Apps Script**
2. Se abre el editor de scripts
3. **Borrar todo el código** que aparece por defecto
4. **Pegar el contenido** del archivo `google_script.js`
5. guardar
6. Darle un nombre, ejemplo: `DetectorLuz`

### Paso 3 — Probar el script antes de implementar
1. En el editor, seleccionar la función `probarScript` en el menú desplegable de funciones
2. Hacer clic en **Ejecutar**
3. Si pide permisos, aceptarlos todos
4. Ve a tu Google Sheets y verifica que aparecieron 2 filas de prueba
5. Si aparecieron el script funciona

### Paso 4 — Implementar como Web App
1. En el editor: **Implementar → Nueva implementación**
2. Clic en el engranaje junto a "Tipo" y seleccionar **Aplicación web**
3. Configurar:
   - **Descripción:** `v1.0 - Detector Luz`
   - **Ejecutar como:** `Yo (tu@gmail.com)`
   - **Quién tiene acceso:** `Cualquier persona`
4. Clic en **Implementar**
5. Copiar la **URL de la aplicación web** que aparece
   - Se ve así: `https://script.google.com/macros/s/ABCDEF.../exec`

### Paso 5 — Configurar el .ino
Abrir `detector_fallas_luz.ino` y editar:

```cpp
const char* WIFI_SSID     = "TU_RED_WIFI";        // Nombre de tu WiFi
const char* WIFI_PASSWORD = "TU_PASSWORD_WIFI";    // Contraseña WiFi
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/XXXXXXXXXX/exec";  // URL DEL SCRIPT
```

---

## SUBIR EL CÓDIGO AL ESP32C3

1. Conectar el ESP32C3 Super Mini por USB-C
2. En Arduino IDE:
   - **Placa:** ESP32C3 Dev Module
   - **Port:** El puerto COM que aparece (ej: COM4 en Windows, /dev/ttyUSB0 en Linux)
   - **USB CDC On Boot:** Enabled *(importante para ver Serial)*
3. Clic en **Subir**
4. Si hay error de subida, mantener presionado el botón BOOT del ESP32C3 mientras inicia la subida

---

## SECUENCIA DE PRUEBAS

### Prueba 1 — Verificar conexión WiFi y NTP
1. Abrir el **Monitor Serie** (a 115200 baud)
2. Al iniciar se debería ver:
   ```
   === DETECTOR DE FALLA DE LUZ ===
   [OK] OLED inicializada
   [WiFi] Conectando a TU_RED_WIFI........
   [WiFi] Conectado! IP: 192.168.x.x
   [NTP] Esperando sincronización.....
   [OK] Hora sincronizada: 2024-11-15 14:30:00
   [OK] Sistema listo. Estado inicial: LUZ
   ```
3. La OLED debe mostrar "LUZ PRESENTE" con la hora actual

### Prueba 2 — Simular falla de luz
1. **Presionar y mantener** el botón conectado al GPIO3
2. Espera ~2 segundos (debounce)
3. En Serial deberías ver:
   ```
   [FALLA] Corte detectado a las 2024-11-15 14:31:05
   [INFO] Falla #1 en esta sesión
   [HTTP] Enviando a: https://script.google.com/...
   [HTTP] Código: 200
   [OK] Falla registrada en Google Sheets
   ```
4. La OLED debe mostrar "!!! SIN LUZ !!!" parpadeando con contador

### Prueba 3 — Simular retorno de luz
1. **Suelta el botón**
2. Espera ~2 segundos
3. En Serial:
   ```
   [RETORNO] Luz restaurada a las 2024-11-15 14:31:45
   [INFO] Duración de falla: 00h00m40s
   [OK] Retorno registrado en Google Sheets
   ```
4. La OLED muestra "LUZ PRESENTE" con duración de la falla

### Prueba 4 — Verificar Google Sheets
Abrir la hoja de cálculo y verificar:
- Hoja "Registro de Fallas": 2 nuevas filas (FALLA_INICIO y FALLA_FIN)
- Hoja "Resumen": estadísticas actualizadas

---

## SOLUCIÓN DE PROBLEMAS

| Síntoma                  | Causa probable                             | Solución                          |
|--------------------------|--------------------------------------------|-----------------------------------|
| OLED no enciende         | Dirección I2C incorrecta                   | Probar `0x3D` en lugar de `0x3C`  |
| No conecta WiFi          | Credenciales incorrectas                   | Verificar SSID y contraseña       |
| HTTP Error -1            | Sin internet o URL mal copiada             | Verificar URL del Apps Script     |
| Hora muestra T+00:00:xx  | Sin sincronización NTP                     | Verificar conectividad a internet |
| No sube el código        | Modo boot / Mantener botón BOOT al subir   |                                   |
| Datos no llegan a Sheets | Script no implementado                     | Re-implementar en Apps Script     |

### Verificar dirección I2C de la OLED
Si la OLED no funciona, correr este código de diagnóstico:

```cpp
#include <Wire.h>
void setup() {
  Serial.begin(115200);
  Wire.begin(8, 9); // SDA=8, SCL=9
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("Dispositivo I2C en 0x");
      Serial.println(addr, HEX);
    }
  }
}
void loop() {}
```

---

## CONEXIÓN CON SENSOR REAL ZMPT101B (Fase 2)

Cuando se tenga listo el hardware completo, vamos para:

```
ZMPT101B OUT  →  GPIO3
ZMPT101B VCC  →  5V
ZMPT101B GND  →  GND
```

En el código, cambia la lógica de lectura según la salida del módulo:
- Si es salida **digital** (HIGH/LOW): el código actual funciona sin cambios
- Si es salida **analógica**: cambia `digitalRead(PIN_SENSOR)` a `analogRead(PIN_SENSOR)` y se define un umbral

```cpp
// Umbral analógico (ajusta según calibración)
#define UMBRAL_VOLTAJE 100

// En el loop:
bool lecturaSensor = (analogRead(PIN_SENSOR) > UMBRAL_VOLTAJE);
```

---

## PRÓXIMOS PASOS (Fase 2 con baterías)

Una vez validado el funcionamiento por USB:

1. **Shield TP4056 o Shield V3**: Conectar la batería 18650
2. **Verificar que el BAM USB** mantenga la conexión WiFi con el ESP32 alimentado por batería
3. **Agregar pantalla LCD I2C 16x2**: Es la misma lógica que la OLED, es cosa de cambiar la librería
4. **EEPROM/Preferences**: Guardar registros localmente si se pierde WiFi y sincronizar cuando vuelva
5. **Deep Sleep**: Para mayor duración de batería entre lecturas

---

## ESTRUCTURA DE DATOS EN GOOGLE SHEETS (puede cambiar entre updates)

| N°  | Tipo de Evento  | Hora (Dispositivo)    | Hora (Servidor)       | Duración (seg)  | Duración H:M:S  | Notas              |
|---  |---              |---                    |---                    |---              |---              |---                 |
| 1   | FALLA_INICIO    | 2024-11-15 14:31:05   | 2024-11-15 14:31:06   | 0               | -               | Falla #1           |
| 2   | FALLA_FIN       | 2024-11-15 16:45:10   | 2024-11-15 16:45:11   | 8645            | 2h 24m 5s       | Duracion:02h24m05s |

---

*Proyecto: Detector de Falla de Luz — Venezuela*
*Fase 1: Validación USB | v1.0*
