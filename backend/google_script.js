/*
 * ============================================================
  GOOGLE APPS SCRIPT - Receptor de datos del ESP32C3
  Detector de Falla de Luz - Venezuela
  Versión 2.0 - Estadísticas automáticas

  Hojas generadas automáticamente:
  - "Registro de Fallas" → todos los eventos raw
  - "Resumen" → estadísticas generales
  - "Estadísticas" → promedio semanal y franja horaria

  INSTRUCCIONES DE INSTALACIÓN:
  1. Ir a https://sheets.google.com y crear una hoja nueva
  2. Nombrar hoja, ejemplo "Registro de Fallas"
  3. En el menú: Extensiones / Apps Script
  4. Borrar el código existente y pegar este archivo
  5. Guardar
  6. Implementar / Nueva implementación
  7. Tipo: "Aplicación web"
  8. Ejecutar como: "Yo"
  9. Quién tiene acceso: "Cualquier persona"
  10. Clic en "Implementar" y copiar la URL que aparece
  11. Pegar la URL en el .ino donde dice GOOGLE_SCRIPT_URL
 * ============================================================
*/

// Nombre de la hoja
var NOMBRE_HOJA        = "Registro de Fallas";
var NOMBRE_RESUMEN     = "Resumen";
var NOMBRE_ESTADISTICAS = "Estadísticas";

// horarios
var FRANJAS = [
  { nombre: "Madrugada", inicio: 0,  fin: 5  },
  { nombre: "Mañana",    inicio: 6,  fin: 11 },
  { nombre: "Tarde",     inicio: 12, fin: 17 },
  { nombre: "Noche",     inicio: 18, fin: 23 }
];

// Recibir las peticiones GET del ESP32
function doGet(e)
{
  try
  {
    var tipo     = e.parameter.tipo     || "DESCONOCIDO";
    var hora     = e.parameter.hora     || "Sin hora";
    var duracion = e.parameter.duracion || "0";
    var notas    = e.parameter.notas    || "";

    hora  = decodeURIComponent(hora.replace(/\+/g, " "));
    notas = decodeURIComponent(notas.replace(/\+/g, " "));

    var resultado = guardarRegistro(tipo, hora, duracion, notas);

    return ContentService
      .createTextOutput(JSON.stringify({
        status:  "OK",
        mensaje: "Registro guardado",
        fila:    resultado
      }))
      .setMimeType(ContentService.MimeType.JSON);
  }
  catch (error)
  {
    return ContentService
      .createTextOutput(JSON.stringify({
        status:  "ERROR",
        mensaje: error.toString()
      }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

// Guardar registro en la hoja
function guardarRegistro(tipo, hora, duracion, notas)
{
  var ss   = SpreadsheetApp.getActiveSpreadsheet();
  var hoja = ss.getSheetByName(NOMBRE_HOJA);

  // Crear la hoja si no existe
  if (!hoja)
  {
    hoja = ss.insertSheet(NOMBRE_HOJA);
    crearEncabezados(hoja);
  }

  // Si la hoja está vacía, crear encabezados
  if (hoja.getLastRow() === 0)
  {
    crearEncabezados(hoja);
  }

  // Calcular duración
  var durSeg     = parseInt(duracion) || 0;
  var durFormato = durSeg > 0 ? segundosAHMS(durSeg) : "-";
  
  //  verificación
  var timestampServidor = Utilities.formatDate(
    new Date(),
    "America/Caracas",
    "yyyy-MM-dd HH:mm:ss"
  );

  // Fila a insertar
  var ultimaFila = hoja.getLastRow();
  var nuevaFila = [
    ultimaFila,
    tipo,
    hora,
    timestampServidor,
    durSeg,
    durFormato,
    notas
  ];

  hoja.appendRow(nuevaFila);

  // Colorear
  var filaInsertada = hoja.getLastRow();
  var rango = hoja.getRange(filaInsertada, 1, 1, nuevaFila.length);

  if (tipo === "FALLA_INICIO")
  {
    rango.setBackground("#fc3d3d");
    rango.setFontWeight("bold");
  }
  else if (tipo === "FALLA_FIN")
  {
    rango.setBackground("#5ff75f");
  }

  // Ajustar columnas
  hoja.autoResizeColumns(1, nuevaFila.length);

  // Actualizar las hojas de análisis
  actualizarResumen(hoja);
  actualizarEstadisticas(hoja);

  return filaInsertada;
}

// Crear/actualizar una hoja de resumen estadístico
function actualizarResumen(hojaRegistros)
{
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var hojaResumen = ss.getSheetByName(NOMBRE_RESUMEN);

  if (!hojaResumen)
  {
    hojaResumen = ss.insertSheet(NOMBRE_RESUMEN);
  }

  hojaResumen.clearContents();

  // Contar eventos
  var datos = hojaRegistros.getDataRange().getValues();
  var totalFallas = 0;
  var sumaSegundos = 0;
  var maxDuracion = 0;
  var ultimaFalla = "";
  var duracionesCompletas = [];

  for (var i = 1; i < datos.length; i++)
  {
    var tipo = datos[i][1];
    var durSeg = parseInt(datos[i][4]) || 0;
    var horaEvento = datos[i][2];

    if (tipo === "FALLA_INICIO")
    {
      totalFallas++;
      ultimaFalla = horaEvento;
    }

    if (tipo === "FALLA_FIN" && durSeg > 0)
    {
      duracionesCompletas.push(durSeg);
      sumaSegundos += durSeg;
      if (durSeg > maxDuracion) maxDuracion = durSeg;
    }
  }

  var promSegundos = duracionesCompletas.length > 0
    ? Math.floor(sumaSegundos / duracionesCompletas.length)
    : 0;

    // Resumen
  var resumen = [
    ["📊 RESUMEN DE FALLAS ELÉCTRICAS", ""],
    ["", ""],
    ["Total de fallas registradas:", totalFallas],
    ["Fallas con duración completa:", duracionesCompletas.length],
    ["Duración promedio por falla:", segundosAHMS(promSegundos)],
    ["Duración máxima registrada:", segundosAHMS(maxDuracion)],
    ["Tiempo total sin luz:", segundosAHMS(sumaSegundos)],
    ["Última falla:", ultimaFalla],
    ["", ""],
    ["Actualizado:", Utilities.formatDate(new Date(), "America/Caracas", "yyyy-MM-dd HH:mm:ss")]
  ];

  hojaResumen.getRange(1, 1, resumen.length, 2).setValues(resumen);
  hojaResumen.getRange(1, 1).setFontSize(14).setFontWeight("bold");
  hojaResumen.autoResizeColumns(1, 2);

  // Formato de encabezado
  hojaResumen.getRange(1, 1, 1, 2).setBackground("#1a1a2e").setFontColor("#FFFFFF");
}

// Promedio semanal y distribución por horario
function actualizarEstadisticas(hojaRegistros)
{
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var hojaEstadisticas = ss.getSheetByName(NOMBRE_ESTADISTICAS);

  if (!hojaEstadisticas)
  {
    hojaEstadisticas = ss.insertSheet(NOMBRE_ESTADISTICAS);
  }

  hojaEstadisticas.clearContents();

  var datos = hojaRegistros.getDataRange().getValues();

  // Acumuladores por semana (YYYY-Www)
  var semanas = {};

  // Acumuladores por franja horaria
  var conteoFranjas = {
    "Madrugada": 0,
    "Mañana": 0,
    "Tarde": 0,
    "Noche": 0
  };

  var duracionFranjas = {
    "Madrugada": 0,
    "Mañana": 0,
    "Tarde": 0,
    "Noche": 0
  };

  for (var i = 1; i < datos.length; i++)
  {
    var tipo = datos[i][1];
    var hora = datos[i][2].toString();
    var durSeg = parseInt(datos[i][4]) || 0;

    if (tipo === "FALLA_FIN" && durSeg > 0 && hora.length >= 16)
    {
      // Fecha y hora
      var partes = hora.split(" ");
      var fecha  = partes[0];                    // "2026-06-14"
      var hhmm   = partes[1] ? partes[1] : "00:00:00";
      var horaNum = parseInt(hhmm.split(":")[0]);  // hora del día 0-23

      // Número de semana
      var claveSemana = obtenerSemana(fecha);
      if (!semanas[claveSemana])
      {
        semanas[claveSemana] = { totalSeg: 0, fallas: 0 };
      }
      semanas[claveSemana].totalSeg += durSeg;
      semanas[claveSemana].fallas++;

      // Clasificar en franja horaria
      var franja = clasificarFranja(horaNum);
      conteoFranjas[franja]++;
      duracionFranjas[franja] += durSeg;
    }
  }

  // Promedio semanal
  var fila = 1;

  hojaEstadisticas.getRange(fila, 1, 1, 4)
    .setValues([["📅 PROMEDIO SEMANAL DE HORAS SIN LUZ", "", "", ""]])
    .setBackground("#1a1a2e")
    .setFontColor("#FFFFFF")
    .setFontWeight("bold");
  fila++;

  hojaEstadisticas.getRange(fila, 1, 1, 4)
    .setValues([["Semana", "Fallas", "Total sin luz", "Promedio diario"]])
    .setBackground("#2d2d44")
    .setFontColor("#FFFFFF")
    .setFontWeight("bold");
  fila++;

  var clavesSemanas = Object.keys(semanas).sort();
  var totalSemanaSeg = 0;
  var totalSemanas   = clavesSemanas.length;

  for (var s = 0; s < clavesSemanas.length; s++)
  {
    var clave     = clavesSemanas[s];
    var dataSem   = semanas[clave];
    var promDiario = Math.floor(dataSem.totalSeg / 7);
    totalSemanaSeg += dataSem.totalSeg;

    var filaData = [
      clave,
      dataSem.fallas,
      segundosAHMS(dataSem.totalSeg),
      segundosAHMS(promDiario)
    ];

    hojaEstadisticas.getRange(fila, 1, 1, 4).setValues([filaData]);

    // Alternar color de filas
    if (s % 2 === 0)
    {
      hojaEstadisticas.getRange(fila, 1, 1, 4).setBackground("#F0F0F0");
    }

    fila++;
  }

  // Promedio general
  var promGeneralSemanal = totalSemanas > 0
    ? Math.floor(totalSemanaSeg / totalSemanas)
    : 0;

  hojaEstadisticas.getRange(fila, 1, 1, 4)
    .setValues([["PROMEDIO GENERAL", "", segundosAHMS(promGeneralSemanal) + "/semana", segundosAHMS(Math.floor(promGeneralSemanal / 7)) + "/día"]])
    .setBackground("#CCFFCC")
    .setFontWeight("bold");
  fila += 2;

  // Franja horaria
  hojaEstadisticas.getRange(fila, 1, 1, 4)
    .setValues([["🕐 DISTRIBUCIÓN POR FRANJA HORARIA", "", "", ""]])
    .setBackground("#1a1a2e")
    .setFontColor("#FFFFFF")
    .setFontWeight("bold");
  fila++;

  hojaEstadisticas.getRange(fila, 1, 1, 4)
    .setValues([["Franja", "Horario", "Fallas", "Tiempo total sin luz"]])
    .setBackground("#2d2d44")
    .setFontColor("#FFFFFF")
    .setFontWeight("bold");
  fila++;

  var maxFallas  = 0;
  var franjaMax  = "";
  var totalFallasFranjas = 0;

  for (var f = 0; f < FRANJAS.length; f++)
  {
    var nombreFranja  = FRANJAS[f].nombre;
    var horarioFranja = "0" + FRANJAS[f].inicio + ":00 - " + FRANJAS[f].fin + ":59";
    var fallasFranja  = conteoFranjas[nombreFranja];
    var durFranja     = duracionFranjas[nombreFranja];

    totalFallasFranjas += fallasFranja;

    if (fallasFranja > maxFallas)
    {
      maxFallas = fallasFranja;
      franjaMax = nombreFranja;
    }

    var filaFranja = [
      nombreFranja,
      horarioFranja,
      fallasFranja,
      segundosAHMS(durFranja)
    ];

    hojaEstadisticas.getRange(fila, 1, 1, 4).setValues([filaFranja]);

    // Resaltar la franja más frecuente
    if (nombreFranja === franjaMax && maxFallas > 0)
    {
      hojaEstadisticas.getRange(fila, 1, 1, 4)
        .setBackground("#FFCCCC")
        .setFontWeight("bold");
    }
    else if (f % 2 === 0)
    {
      hojaEstadisticas.getRange(fila, 1, 1, 4).setBackground("#F0F0F0");
    }

    fila++;
  }

  // Conclusión de franja
  if (franjaMax !== "")
  {
    fila++;
    hojaEstadisticas.getRange(fila, 1, 1, 4)
      .setValues([["La luz se va más en:", franjaMax, "con " + maxFallas + " fallas", ""]])
      .setBackground("#FFE0B2")
      .setFontWeight("bold");
  }

  // Timestamp de actualización
  fila += 2;
  hojaEstadisticas.getRange(fila, 1, 1, 2)
    .setValues([["Actualizado:", Utilities.formatDate(new Date(), "America/Caracas", "yyyy-MM-dd HH:mm:ss")]]);

  hojaEstadisticas.autoResizeColumns(1, 4);
}


//  Clasificar hora del dia
function clasificarFranja(horaNum)
{
  for (var i = 0; i < FRANJAS.length; i++)
  {
    if (horaNum >= FRANJAS[i].inicio && horaNum <= FRANJAS[i].fin)
    {
      return FRANJAS[i].nombre;
    }
  }
  return "Noche";
}


// Clave de semana (2026-W24)
function obtenerSemana(fechaStr)
{
  var partes = fechaStr.split("-");
  var fecha = new Date(
    parseInt(partes[0]),
    parseInt(partes[1]) - 1,
    parseInt(partes[2])
  );

  // Número de semana ISO
  var inicioAnio = new Date(fecha.getFullYear(), 0, 1);
  var diasTransc = Math.floor((fecha - inicioAnio) / 86400000);
  var numSemana = Math.ceil((diasTransc + inicioAnio.getDay() + 1) / 7);

  return fecha.getFullYear() + "-W" + (numSemana < 10 ? "0" + numSemana : numSemana);
}


//  Encabezados
function crearEncabezados(hoja)
{
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

  var rangoEnc = hoja.getRange(1, 1, 1, encabezados.length);
  rangoEnc.setBackground("#1a1a2e");
  rangoEnc.setFontColor("#FFFFFF");
  rangoEnc.setFontWeight("bold");
  rangoEnc.setHorizontalAlignment("center");
}

// Conviertir segundos a H:M:S
function segundosAHMS(totalSeg)
{
  totalSeg = Math.floor(totalSeg);
  var h = Math.floor(totalSeg / 3600);
  var m = Math.floor((totalSeg % 3600) / 60);
  var s = totalSeg % 60;
  return h + "h " + m + "m " + s + "s";
}

// Función de prueba - ejecutar desde el editor de Scripts
function probarScript()
{
  // Simulación defalla de 2 horas en la madrugada
  var e1 = {
    parameter: {
      tipo:     "FALLA_INICIO",
      hora:     "2026-06-14 03:15:00",
      duracion: "0",
      notas:    "Prueba madrugada"
    }
  };
  doGet(e1);

  var e2 = {
    parameter: {
      tipo:     "FALLA_FIN",
      hora:     "2026-06-14 05:15:00",
      duracion: "7200",
      notas:    "Dur: 02h00m00s | Inicio: 2026-06-14 03:15:00"
    }
  };
  doGet(e2);

  // Simular una falla de 1 hora en la tarde
  var e3 = {
    parameter: {
      tipo:     "FALLA_INICIO",
      hora:     "2026-06-15 14:30:00",
      duracion: "0",
      notas:    "Prueba tarde"
    }
  };
  doGet(e3);

  var e4 = {
    parameter: {
      tipo:     "FALLA_FIN",
      hora:     "2026-06-15 15:30:00",
      duracion: "3600",
      notas:    "Dur: 01h00m00s | Inicio: 2026-06-15 14:30:00"
    }
  };
  doGet(e4);

  Logger.log("Prueba completada. Revisa las hojas de cálculo.");
}