/**
 * ============================================================
 *  GOOGLE APPS SCRIPT - Receptor de datos del ESP32C3
 *  Detector de Falla de Luz - Venezuela
 *  Versión: 1.0
 *
 *  INSTRUCCIONES DE INSTALACIÓN:
 *  1. Ir a https://sheets.google.com y crear una hoja nueva
 *  2. Nómbrar hoja, ejemplo "Registro de Fallas"
 *  3. En el menú: Extensiones / Apps Script
 *  4. Borrar el código existente y pegar este archivo
 *  5. Guardar
 *  6. Implementar / Nueva implementación
 *  7. Tipo: "Aplicación web"
 *  8. Ejecutar como: "Yo"
 *  9. Quién tiene acceso: "Cualquier persona"
 *  10. Clic en "Implementar" y copiar la URL que aparece
 *  11. Pegar la URL en el .ino donde dice GOOGLE_SCRIPT_URL
 * ============================================================
 */

// Nombre de la hoja
var NOMBRE_HOJA = "Registro de Fallas";

/**
 * recibir las peticiones GET del ESP32
 */
function doGet(e) {
  try {
    var tipo     = e.parameter.tipo     || "DESCONOCIDO";
    var hora     = e.parameter.hora     || "Sin hora";
    var duracion = e.parameter.duracion || "0";
    var notas    = e.parameter.notas    || "";

    // Decodificar caracteres URL si es necesario
    hora  = decodeURIComponent(hora.replace(/\+/g, " "));
    notas = decodeURIComponent(notas.replace(/\+/g, " "));

    // Guardar en Google Sheets
    var resultado = guardarRegistro(tipo, hora, duracion, notas);

    // Respuesta al ESP32
    return ContentService
      .createTextOutput(JSON.stringify({
        status: "OK",
        mensaje: "Registro guardado",
        fila: resultado
      }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (error) {
    return ContentService
      .createTextOutput(JSON.stringify({
        status: "ERROR",
        mensaje: error.toString()
      }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

/**
 * Guardar registro en la hoja
 */
function guardarRegistro(tipo, hora, duracion, notas) {
  var ss    = SpreadsheetApp.getActiveSpreadsheet();
  var hoja  = ss.getSheetByName(NOMBRE_HOJA);

  // Crear la hoja si no existe
  if (!hoja) {
    hoja = ss.insertSheet(NOMBRE_HOJA);
    crearEncabezados(hoja);
  }

  // Si la hoja está vacía, crear encabezados
  if (hoja.getLastRow() === 0) {
    crearEncabezados(hoja);
  }

  // Calcular duración formateada
  var durSeg     = parseInt(duracion) || 0;
  var durFormato = durSeg > 0 ? segundosAHMS(durSeg) : "-";

  // Timestamp del servidor (como verificación)
  var timestampServidor = Utilities.formatDate(
    new Date(),
    "America/Caracas",
    "yyyy-MM-dd HH:mm:ss"
  );

  // Fila a insertar: [N°, Tipo, Hora Dispositivo, Hora Servidor, Duración (s), Duración H:M:S, Notas]
  var ultimaFila = hoja.getLastRow();
  var numeroRegistro = ultimaFila; // El encabezado ocupa fila 1, así que la primera data es fila 2 → N°1

  var nuevaFila = [
    numeroRegistro,
    tipo,
    hora,
    timestampServidor,
    durSeg,
    durFormato,
    notas
  ];

  hoja.appendRow(nuevaFila);

  // Colorear según tipo
  var filaInsertada = hoja.getLastRow();
  var rango = hoja.getRange(filaInsertada, 1, 1, nuevaFila.length);

  if (tipo === "FALLA_INICIO") {
    rango.setBackground("#fc3d3d"); // Rojo
    rango.setFontWeight("bold");
  } else if (tipo === "FALLA_FIN") {
    rango.setBackground("#5ff75f"); // Verde
  }

  // Ajustar columnas
  hoja.autoResizeColumns(1, nuevaFila.length);

  // Actualizar hoja de resumen
  actualizarResumen(hoja);

  return filaInsertada;
}

/**
 * Crear los encabezados de la tabla
 */
function crearEncabezados(hoja) {
  var encabezados = [
    "N°",
    "Tipo de Evento",
    "Hora (Dispositivo)",
    "Hora (Servidor)",
    "Duración (seg)",
    "Duración (H:M:S)",
    "Notas"
  ];

  hoja.appendRow(encabezados);

  // Dar formato al encabezado
  var rangoEnc = hoja.getRange(1, 1, 1, encabezados.length);
  rangoEnc.setBackground("#1a1a2e");
  rangoEnc.setFontColor("#FFFFFF");
  rangoEnc.setFontWeight("bold");
  rangoEnc.setHorizontalAlignment("center");
}

/**
 * Crear/actualizar una hoja de resumen estadístico
 */
function actualizarResumen(hojaRegistros) {
  var ss          = SpreadsheetApp.getActiveSpreadsheet();
  var hojaResumen = ss.getSheetByName("Resumen");

  if (!hojaResumen) {
    hojaResumen = ss.insertSheet("Resumen");
  }

  hojaResumen.clearContents();

  // Contar eventos
  var datos         = hojaRegistros.getDataRange().getValues();
  var totalFallas   = 0;
  var sumaMinutos   = 0;
  var maxDuracion   = 0;
  var ultimaFalla   = "";
  var duracionesCompletadas = [];

  for (var i = 1; i < datos.length; i++) {
    var tipo     = datos[i][1];
    var durSeg   = parseInt(datos[i][4]) || 0;
    var horaEvento = datos[i][2];

    if (tipo === "FALLA_INICIO") {
      totalFallas++;
      ultimaFalla = horaEvento;
    }
    if (tipo === "FALLA_FIN" && durSeg > 0) {
      duracionesCompletadas.push(durSeg);
      sumaMinutos += durSeg / 60;
      if (durSeg > maxDuracion) maxDuracion = durSeg;
    }
  }

  var promMinutos = duracionesCompletadas.length > 0
    ? (sumaMinutos / duracionesCompletadas.length).toFixed(1)
    : 0;

  // Escribir resumen
  var resumen = [
    ["📊 RESUMEN DE FALLAS ELÉCTRICAS", ""],
    ["", ""],
    ["Total de fallas:", totalFallas],
    ["Fallas con duración completa:", duracionesCompletadas.length],
    ["Duración promedio:", promMinutos + " min"],
    ["Duración máxima:", segundosAHMS(maxDuracion)],
    ["Tiempo total sin electricidad:", segundosAHMS(sumaMinutos * 60)],
    ["Última falla:", ultimaFalla],
    ["", ""],
    ["Actualizado:", Utilities.formatDate(new Date(), "America/Caracas", "yyyy-MM-dd HH:mm:ss")]
  ];

  hojaResumen.getRange(1, 1, resumen.length, 2).setValues(resumen);
  hojaResumen.getRange(1, 1).setFontSize(14).setFontWeight("bold");
  hojaResumen.autoResizeColumns(1, 2);
}

/**
 * Conviertir segundos a formato H:M:S
 */
function segundosAHMS(totalSeg) {
  totalSeg = Math.floor(totalSeg);
  var h = Math.floor(totalSeg / 3600);
  var m = Math.floor((totalSeg % 3600) / 60);
  var s = totalSeg % 60;
  return h + "h " + m + "m " + s + "s";
}

/**
 * Función de prueba - ejecutar desde el editor de Scripts
 * para verificar que todo funciona sin necesidad del ESP32
 */
function probarScript() {
  var e = {
    parameter: {
      tipo:     "FALLA_INICIO",
      hora:     "2024-11-15 14:30:00",
      duracion: "0",
      notas:    "Prueba manual desde editor"
    }
  };
  var resultado = doGet(e);
  Logger.log(resultado.getContent());

  // Probar también el fin de falla
  var e2 = {
    parameter: {
      tipo:     "FALLA_FIN",
      hora:     "2024-11-15 16:45:00",
      duracion: "8100",
      notas:    "Duracion: 02h15m00s | Inicio: 2024-11-15 14:30:00"
    }
  };
  var resultado2 = doGet(e2);
  Logger.log(resultado2.getContent());
}
