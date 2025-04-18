#include "web_server.h"
#include <Preferences.h>

WebServer server(80);
extern Preferences preferences;

// Variable global para almacenar el color aleatorio
String randomStateColor = "";

// Variable para almacenar la nota
String notaPersonalizada = "";


bool useFuenteDC = false;
float fuenteDC_Amps = 0.0;
float maxBulkHours = 0.0;


// Variables para el control de apagado temporal de la carga
unsigned long loadOffStartTime = 0;
unsigned long loadOffDuration = 0;
bool temporaryLoadOff = false;



// Función para generar un color hexadecimal aleatorio
String generateRandomColor() {
  uint32_t randomNum = esp_random(); // Usa la función de generación de números aleatorios del ESP32
  
  // Crea un color hexadecimal usando el número aleatorio generado
  char colorBuffer[8];
  sprintf(colorBuffer, "#%06X", (randomNum & 0xFFFFFF)); // Extraemos los últimos 6 dígitos para el código de color
  
  return String(colorBuffer);
}

  void checkLoadOffTimer() {
    if (temporaryLoadOff && millis() - loadOffStartTime >= loadOffDuration) {
      // Solo encender si fue apagado por esta funcionalidad y no por otras razones
      digitalWrite(LOAD_CONTROL_PIN, HIGH);
      temporaryLoadOff = false;
      notaPersonalizada = "Carga reactivada automáticamente después del tiempo especificado";
    }
  }


void initWebServer() {

  // Genera un color aleatorio al inicializar el servidor web
  randomStateColor = generateRandomColor(); 

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getHTML());
  });

  server.on("/data", HTTP_GET, []() {
    String json = getData();
    server.send(200, "application/json", json);
  });

  server.on("/update", HTTP_POST, []() {
    if (server.hasArg("batteryCapacity") &&
        server.hasArg("thresholdPercentage") &&
        server.hasArg("maxAllowedCurrent") &&
        server.hasArg("bulkVoltage") &&
        server.hasArg("absorptionVoltage") &&
        server.hasArg("floatVoltage") &&
        server.hasArg("isLithium")) {

      batteryCapacity = server.arg("batteryCapacity").toFloat();
      thresholdPercentage = server.arg("thresholdPercentage").toFloat();
      maxAllowedCurrent = server.arg("maxAllowedCurrent").toFloat();
      bulkVoltage = server.arg("bulkVoltage").toFloat();
      absorptionVoltage = server.arg("absorptionVoltage").toFloat();
      floatVoltage = server.arg("floatVoltage").toFloat();
      isLithium = server.arg("isLithium") == "true";
      useFuenteDC = server.arg("powerSource") == "true";
      fuenteDC_Amps = server.arg("fuenteDC_Amps").toFloat();
      
      absorptionCurrentThreshold_mA = (batteryCapacity * thresholdPercentage) * 10;
      currentLimitIntoFloatStage = absorptionCurrentThreshold_mA / factorDivider;

      preferences.begin("charger", false);
      preferences.putFloat("batteryCap", batteryCapacity);
      preferences.putFloat("thresholdPerc", thresholdPercentage);
      preferences.putFloat("maxCurrent", maxAllowedCurrent);
      preferences.putFloat("bulkV", bulkVoltage);
      preferences.putFloat("absV", absorptionVoltage);
      preferences.putFloat("floatV", floatVoltage);
      preferences.putBool("isLithium", isLithium);
      preferences.putBool("useFuenteDC", useFuenteDC);
      preferences.putFloat("fuenteDC_Amps", fuenteDC_Amps);
      preferences.end();

      server.sendHeader("Location", "/");
      server.send(303);
    } else {
      server.send(400, "text/plain", "Parámetros inválidos");
    }
  });


  server.on("/toggle-load", HTTP_POST, []() {
    if (server.hasArg("seconds")) {
      int seconds = server.arg("seconds").toInt();
      if (seconds > 0 && seconds <= 300) { // Máximo 5 minutos (300 segundos)
        if (digitalRead(LOAD_CONTROL_PIN) == HIGH) {
          digitalWrite(LOAD_CONTROL_PIN, LOW);
          temporaryLoadOff = true;
          loadOffStartTime = millis();
          loadOffDuration = seconds * 1000; // Convertir a milisegundos
          notaPersonalizada = "Carga apagada por " + String(seconds) + " segundos";
        } else {
          temporaryLoadOff = false; // Asegurarse de que no activemos el temporizador
          notaPersonalizada = "La carga ya está apagada, no se realizó ninguna acción";
        }
      } else {
        notaPersonalizada = "Tiempo fuera de rango (1-300 segundos)";
      }
      server.sendHeader("Location", "/");
      server.send(303);
    } else {
      server.send(400, "text/plain", "Parámetro 'seconds' no proporcionado");
    }
  });


  server.begin();
}

void handleWebServer() {
  server.handleClient();
  checkLoadOffTimer();
}

String getHTML() {
  String html = "<!DOCTYPE html><html lang='es'>";
  html += "<head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Cargador</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f0f0f0; }";
  html += ".container { max-width: 800px; margin: 0 auto; padding: 20px; }";
  html += "h1 { text-align: center; margin-bottom: 20px; }";
  html += "h2 { text-align: center; margin-bottom: 20px; }";
  html += ".table-wrap { overflow-x: auto; margin-bottom: 20px; }";
  html += "table { width: 100%; border-collapse: collapse; min-width: 400px; background-color: #fff; box-shadow: 0 0 10px rgba(0, 0, 0, 0.1); }";
  html += "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }";
  html += "th { background-color: #f2f2f2; }";
  html += "tr:nth-child(even) { background-color: #fafafa; }";
  html += ".form-container { background-color: #fff; padding: 20px; box-shadow: 0 0 10px rgba(0, 0, 0, 0.1); margin-bottom: 20px; }";
  html += ".form-group { margin-bottom: 15px; }";
  html += ".form-group label { display: block; margin-bottom: 5px; }";
  html += ".form-group input { width: 100%; padding: 8px; box-sizing: border-box; }";
  html += ".form-group input[type='submit'] { background-color: #4CAF50; color: white; border: none; cursor: pointer; }";
  html += ".form-group input[type='submit']:hover { background-color: #45a049; }";
  html += ".changed { background-color: #d7ffd7; transition: background-color 1s ease; }";
  html += "#chargeStateLabel { color: " + randomStateColor + "; font-weight: bold; }"; // Añadir color aleatorio para "Estado de Carga"

  // Estilos para dispositivos móviles
  html += "@media (max-width: 600px) {";
  html += "  .form-group input, .form-group select { font-size: 16px; padding: 10px; }";
  html += "  .form-group label { font-size: 14px; }";
  html += "  table { font-size: 14px; }";
  html += "  th, td { padding: 6px 4px; }";
  html += "  .container { padding: 10px; }";
  html += "  h1 { font-size: 24px; }";
  html += "  h2 { font-size: 20px; }";
  html += "  .form-group { margin-bottom: 10px; }";
  html += "  .form-group input[type='submit'] { padding: 12px; font-size: 16px; }";
  html += "}";
  
  html += "</style>";
  html += "</head>";
  html += "<body>";
  html += "<div class='container'>";
  html += "<h1>Estado del Cargador</h1>";
  html += "<div class='table-wrap'>";
  html += "<table>";
  html += "<tr><th>Parámetro</th><th>Valor</th></tr>";
  html += "<tr><td>Corriente Panel a Batería (mA)</td><td id='panelToBatteryCurrent'>-</td></tr>";
  html += "<tr><td>Corriente Batería a Carga (mA)</td><td id='batteryToLoadCurrent'>-</td></tr>";
  html += "<tr><td>Voltaje Panel</td><td id='voltagePanel'>-</td></tr>";
  html += "<tr><td>Voltaje Batería</td><td id='voltageBatterySensor2'>-</td></tr>";
  html += "<tr><td id='chargeStateLabel'>Estado de Carga</td><td id='chargeState'>-</td></tr>"; // Añadimos un ID para aplicar el estilo
  html += "<tr><td>Voltaje Etapa BULK</td><td id='bulkVoltage'>-</td></tr>";
  html += "<tr><td>Voltaje Etapa ABSORCIÓN</td><td id='absorptionVoltage'>-</td></tr>";
  html += "<tr><td>Voltaje Etapa FLOTACIÓN(GEL)</td><td id='floatVoltage'>-</td></tr>";
  html += "<tr><td>PWM Actual</td><td id='currentPWM'>-</td></tr>";
  html += "<tr><td>LVD</td><td id='LVD'>-</td></tr>";
  html += "<tr><td>LVR</td><td id='LVR'>-</td></tr>";
  html += "<tr><td>Umbral de Corriente (mA)</td><td id='absorptionCurrentThreshold_mA'>-</td></tr>";
  html += "<tr><td>Capacidad de la Batería (Ah)</td><td id='batteryCapacity'>-</td></tr>";
  html += "<tr><td>Umbral de Corriente (%)</td><td id='thresholdPercentage'>-</td></tr>";
  html += "<tr><td>Tiempo Calculado de Absorción (horas)</td><td id='calculatedAbsorptionHours'>-</td></tr>";
  html += "<tr><td>Ah Acumulados</td><td id='accumulatedAh'>-</td></tr>";
  html += "<tr><td>SOC Estimado (%)</td><td id='estimatedSOC'>-</td></tr>";
  html += "<tr><td>Corriente Máxima Permitida (mA)</td><td id='maxAllowedCurrent'>-</td></tr>";
  html += "<tr><td>Corriente Neta en Batería (mA)</td><td id='netCurrent'>-</td></tr>";
  html += "<tr><td>Límite de corriente en float (mA)</td><td id='currentLimitIntoFloatStage'>-</td></tr>";
  html += "<tr><td>Tipo de Batería</td><td id='isLithium'>-</td></tr>";
  html += "<tr><td>Temperatura</td><td id='temperature'>-</td></tr>";
  html += "<tr><td>Nota</td><td id='notaPersonalizada'>" + notaPersonalizada + "</td></tr>";
  html += "<tr><td>Fuente de Energía</td><td id='powerSource_display'>-</td></tr>";
  html += "<tr><td>Amperios Fuente DC</td><td id='fuenteDC_Amps_display'>-</td></tr>";
  html += "<tr><td>Horas máx. en Bulk</td><td id='maxBulkHours'>-</td></tr>";
  html += "</table>";
  html += "</div>";
  html += "<h2>Control de Carga</h2>";
  html += "<div class='form-container'>";
  html += "<form action='/toggle-load' method='POST'>";
  html += "<div class='form-group'>";
  html += "<label for='seconds'>Apagar carga temporalmente (segundos):</label>";
  html += "<input type='number' id='seconds' name='seconds' min='1' max='300' value='5' required>";
  html += "<input type='submit' value='Apagar'>";
  html += "</div>";
  html += "</form>";
  html += "</div>";
  html += "<h2>Configuración</h2>";
  html += "<div class='form-container'>";
  html += "<form action='/update' method='POST'>";
  html += "<div class='form-group'>";
  html += "<label for='batteryCapacity'>Capacidad de la batería (Ah):</label>";
  html += "<input type='number' id='batteryCapacity' name='batteryCapacity' step='0.1' min='0' value='" + String(batteryCapacity) + "' required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label for='thresholdPercentage'>Umbral de corriente (%):</label>";
  html += "<input type='number' id='thresholdPercentage' name='thresholdPercentage' step='0.1' min='0.1' max='5' value='" + String(thresholdPercentage) + "' required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label for='maxAllowedCurrent'>Corriente Máxima Permitida (mA):</label>";
  html += "<input type='number' id='maxAllowedCurrentInput' name='maxAllowedCurrent' step='100' min='1000' max='10000' value='" + String(maxAllowedCurrent) + "' required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label for='bulkVoltage'>Voltaje Bulk (V):</label>";
  html += "<input type='number' id='bulkVoltageInput' name='bulkVoltage' step='0.1' min='12' max='15' value='" + String(bulkVoltage) + "' required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label for='absorptionVoltage'>Voltaje Absorción (V):</label>";
  html += "<input type='number' id='absorptionVoltageInput' name='absorptionVoltage' step='0.1' min='12' max='15' value='" + String(absorptionVoltage) + "'  required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label for='floatVoltage'>Voltaje Float(GEL) (V):</label>";
  html += "<input type='number' id='floatVoltageInput' name='floatVoltage' step='0.1' min='12' max='15' value='" + String(floatVoltage) + "' required>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label for='isLithium'>Tipo de Batería:</label>";
  html += "<select id='isLithium' name='isLithium' required>";
  html += "<option value='false'" + String(isLithium ? "" : " selected") + ">GEL</option>";
  html += "<option value='true'" + String(isLithium ? " selected" : "") + ">Litio</option>";
  html += "</select>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label for='powerSource'>Fuente de Energía:</label>";
  html += "<select id='powerSource' name='powerSource' required>";
  html += "<option value='false'" + String(useFuenteDC ? "" : " selected") + ">Panel Solar</option>";
  html += "<option value='true'" + String(useFuenteDC ? " selected" : "") + ">Fuente DC</option>";
  html += "</select>";
  html += "</div>";
  html += "<div class='form-group' id='fuenteDC_container' " + String(useFuenteDC ? "" : "style='display:block;'") + ">";
  html += "<label for='fuenteDC_Amps'>Amperios de Fuente DC:</label>";
  html += "<input type='number' id='fuenteDC_Amps' name='fuenteDC_Amps' step='0.1' min='0' value='" + String(fuenteDC_Amps) + "'>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<input type='submit' value='Actualizar'>";
  html += "</div>";
  html += "</form>";
  html += "</div>";
  html += "</div>";
  html += "<script>";
  

 

  // Función updateData mejorada
  html += "function updateData() {";
  html += "  fetch('/data')";
  html += "    .then(response => {";
  html += "      if (!response.ok) {";
  html += "        throw new Error(`Error HTTP: ${response.status}`);";
  html += "      }";
  html += "      return response.json();";
  html += "    })";
  html += "    .then(data => {";
  html += "      console.log('Datos recibidos:', data);"; // Depuración
  html += "      updateField('panelToBatteryCurrent', data.panelToBatteryCurrent);";
  html += "      updateField('batteryToLoadCurrent', data.batteryToLoadCurrent);";
  html += "      updateField('voltagePanel', data.voltagePanel);";
  html += "      updateField('voltageBatterySensor2', data.voltageBatterySensor2);";
  html += "      updateField('chargeState', data.chargeState);";
  html += "      updateField('bulkVoltage', data.bulkVoltage);";
  html += "      updateField('absorptionVoltage', data.absorptionVoltage);";
  html += "      updateField('floatVoltage', data.floatVoltage);";
  html += "      updateField('currentPWM', data.currentPWM);";
  html += "      updateField('LVD', data.LVD);";
  html += "      updateField('LVR', data.LVR);";
  html += "      updateField('absorptionCurrentThreshold_mA', data.absorptionCurrentThreshold_mA);";
  html += "      updateField('batteryCapacity', data.batteryCapacity);";
  html += "      updateField('thresholdPercentage', data.thresholdPercentage);";
  html += "      updateField('calculatedAbsorptionHours', data.calculatedAbsorptionHours);";
  html += "      updateField('accumulatedAh', data.accumulatedAh);";
  html += "      updateField('estimatedSOC', data.estimatedSOC);";
  html += "      updateField('maxAllowedCurrent', data.maxAllowedCurrent);";
  html += "      updateField('netCurrent', data.netCurrent);";
  html += "      updateField('currentLimitIntoFloatStage', data.currentLimitIntoFloatStage);";
  html += "      updateField('isLithium', data.isLithium ? 'Litio' : 'GEL');";
  html += "      updateField('temperature', data.temperature);";
  html += "      updateField('notaPersonalizada', data.notaPersonalizada);";
  html += "      updateField('powerSource_display', data.useFuenteDC ? 'Fuente DC' : 'Panel Solar');";
  html += "      updateField('fuenteDC_Amps_display', data.fuenteDC_Amps);";
  html += "      updateField('maxBulkHours', data.maxBulkHours);";
  html += "    })";
  html += "    .catch(error => {";
  html += "      console.error('Error al obtener datos:', error);";
  html += "    });";
  html += "}";
  
  // Función updateField mejorada
  html += "function updateField(id, newValue) {";
  html += "  let el = document.getElementById(id);";
  html += "  if (!el) return;";
  
  // Para el campo de estado de carga
  html += "  if (id === 'chargeState') {";
  html += "    el.innerText = newValue;";
  html += "    if (newValue === 'BULK_CHARGE') {";
  html += "      el.style.color = '#ff9900';"; // Naranja para carga bulk
  html += "      el.style.fontWeight = 'bold';";
  html += "    } else if (newValue === 'ABSORPTION_CHARGE') {";
  html += "      el.style.color = '#3366cc';"; // Azul para absorción
  html += "      el.style.fontWeight = 'bold';";
  html += "    } else if (newValue === 'FLOAT_CHARGE') {";
  html += "      el.style.color = '#33cc33';"; // Verde para flotación
  html += "      el.style.fontWeight = 'bold';";
  html += "    } else if (newValue === 'ERROR') {";
  html += "      el.style.color = '#cc0000';"; // Rojo para error
  html += "      el.style.fontWeight = 'bold';";
  html += "    }";
  html += "    el.classList.add('changed');";
  html += "    setTimeout(() => { el.classList.remove('changed'); }, 1000);";
  html += "    return;";
  html += "  }";
  
  // Para el campo de SOC estimado
  html += "  if (id === 'estimatedSOC') {";
  html += "    const socValue = parseFloat(newValue);";
  html += "    el.innerText = newValue;";
  html += "    if (socValue < 20) {";
  html += "      el.style.color = '#cc0000';"; // Rojo para baja carga
  html += "    } else if (socValue < 50) {";
  html += "      el.style.color = '#ff9900';"; // Naranja para carga media
  html += "    } else {";
  html += "      el.style.color = '#33cc33';"; // Verde para buena carga
  html += "    }";
  html += "    el.classList.add('changed');";
  html += "    setTimeout(() => { el.classList.remove('changed'); }, 1000);";
  html += "    return;";
  html += "  }";
  
  // Para el campo de tipo de batería
  html += "  if (id === 'isLithium') {";
  html += "    const isLithiumSelect = document.getElementById('isLithium');";
  html += "    if (isLithiumSelect && isLithiumSelect.tagName === 'SELECT') {";
  html += "      if (isLithiumSelect.value !== (newValue === 'Litio' ? 'true' : 'false')) {";
  html += "        isLithiumSelect.value = newValue === 'Litio' ? 'true' : 'false';";
  html += "        isLithiumSelect.classList.add('changed');";
  html += "        setTimeout(() => { isLithiumSelect.classList.remove('changed'); }, 1000);";
  html += "      }";
  html += "    } else if (el.innerText !== newValue) {";
  html += "      el.innerText = newValue;";
  html += "      el.classList.add('changed');";
  html += "      setTimeout(() => { el.classList.remove('changed'); }, 1000);";
  html += "    }";
  html += "  } else if (el.innerText != newValue.toString()) {";
  html += "    el.innerText = newValue;";
  html += "    el.classList.add('changed');";
  html += "    setTimeout(() => { el.classList.remove('changed'); }, 1000);";
  html += "  }";
  html += "}";
  
  // Código para inicialización y validación
  html += "document.addEventListener('DOMContentLoaded', function() {";
  html += "  const isLithiumSelect = document.getElementById('isLithium');";
  html += "  isLithiumSelect.value = " + String(isLithium ? "'true'" : "'false'") + ";";
  
  // Inicialización de campos del formulario
  html += "  const batteryCapacity = document.getElementById('batteryCapacity');";
  html += "  const thresholdPercentage = document.getElementById('thresholdPercentage');";
  html += "  const maxAllowedCurrentInput = document.getElementById('maxAllowedCurrentInput');";
  html += "  const bulkVoltageInput = document.getElementById('bulkVoltageInput');";
  html += "  const absorptionVoltageInput = document.getElementById('absorptionVoltageInput');";
  html += "  const floatVoltageInput = document.getElementById('floatVoltageInput');";
  
  html += "  if (!batteryCapacity.value) batteryCapacity.value = '50.0';";
  html += "  if (!thresholdPercentage.value) thresholdPercentage.value = '1.0';";
  html += "  if (!maxAllowedCurrentInput.value) maxAllowedCurrentInput.value = '6000.0';";
  html += "  if (!bulkVoltageInput.value) bulkVoltageInput.value = '14.4';";
  html += "  if (!absorptionVoltageInput.value) absorptionVoltageInput.value = '14.4';";
  html += "  if (!floatVoltageInput.value) floatVoltageInput.value = '13.6';";
  
  // Cargar datos inmediatamente
  html += "  updateData();";
  
  // Validación del formulario
  html += "  const form = document.querySelector('form');";
  html += "  form.addEventListener('submit', function(e) {";
  html += "    const batteryCapacity = parseFloat(document.getElementById('batteryCapacity').value);";
  html += "    const thresholdPercentage = parseFloat(document.getElementById('thresholdPercentage').value);";
  html += "    const maxAllowedCurrent = parseFloat(document.getElementById('maxAllowedCurrentInput').value);";
  html += "    const bulkVoltage = parseFloat(document.getElementById('bulkVoltageInput').value);";
  html += "    const absorptionVoltage = parseFloat(document.getElementById('absorptionVoltageInput').value);";
  html += "    const floatVoltage = parseFloat(document.getElementById('floatVoltageInput').value);";
  
  html += "    if (isNaN(batteryCapacity) || isNaN(thresholdPercentage) || isNaN(maxAllowedCurrent) || ";
  html += "        isNaN(bulkVoltage) || isNaN(absorptionVoltage) || isNaN(floatVoltage)) {";
  html += "      alert('Por favor, complete todos los campos con valores numéricos válidos.');";
  html += "      e.preventDefault();";
  html += "      return false;";
  html += "    }";
  
  html += "    if (bulkVoltage > 15 || absorptionVoltage > 15 || floatVoltage > 15) {";
  html += "      alert('Los voltajes no deben exceder 15V para proteger la batería.');";
  html += "      e.preventDefault();";
  html += "      return false;";
  html += "    }";
  
  html += "    if (floatVoltage > absorptionVoltage) {";
  html += "      alert('El voltaje de flotación debe ser menor que el voltaje de absorción.');";
  html += "      e.preventDefault();";
  html += "      return false;";
  html += "    }";
  
  html += "    return true;";
  html += "  });";
  html += "});";
  
  // Actualización periódica
  html += "setInterval(updateData, 1000);";
  html += "</script>";
  html += "</body></html>";
  return html;
}

String getData() {
  // Asegurar que las variables tengan valores válidos
  float safeVoltagePanel = ina219_1.getBusVoltage_V();
  float safeVoltageBattery = ina219_2.getBusVoltage_V();
  
  // Evitar valores NaN o infinitos
  if (isnan(safeVoltagePanel) || isinf(safeVoltagePanel)) safeVoltagePanel = 0.0;
  if (isnan(safeVoltageBattery) || isinf(safeVoltageBattery)) safeVoltageBattery = 0.0;
  
  // Asegurar que las variables numéricas sean al menos 0
  float safePanelToBatteryCurrent = max(0.0f, panelToBatteryCurrent);
  float safeBatteryToLoadCurrent = max(0.0f, batteryToLoadCurrent);
  float safeBulkVoltage = max(0.0f, bulkVoltage);
  float safeAbsorptionVoltage = max(0.0f, absorptionVoltage);
  float safeFloatVoltage = max(0.0f, floatVoltage);
  float safeabsorptionCurrentThreshold_mA = max(0.0f, absorptionCurrentThreshold_mA);
  float safeBatteryCapacity = max(0.0f, batteryCapacity);
  float safeThresholdPercentage = max(0.0f, thresholdPercentage);
  float safeCalculatedAbsorptionHours = max(0.0f, calculatedAbsorptionHours);
  float safeAccumulatedAh = max(0.0f, accumulatedAh);
  float safeSOC = max(0.0f, getSOCFromVoltage(safeVoltageBattery));
  float safeMaxAllowedCurrent = max(0.0f, maxAllowedCurrent);
  float safeNetCurrent = safePanelToBatteryCurrent - safeBatteryToLoadCurrent;
  float safeCurrentLimitIntoFloatStage = max(0.0f, currentLimitIntoFloatStage);
  float safeTemperature = isnan(temperature) ? 0.0 : temperature;
  
  String json = "{";
  json += "\"panelToBatteryCurrent\": " + String(safePanelToBatteryCurrent) + ",";
  json += "\"batteryToLoadCurrent\": " + String(safeBatteryToLoadCurrent) + ",";
  json += "\"voltagePanel\": " + String(safeVoltagePanel) + ",";
  json += "\"voltageBatterySensor2\": " + String(safeVoltageBattery) + ",";
  json += "\"chargeState\": \"" + getChargeStateString(currentState) + "\",";
  json += "\"bulkVoltage\": " + String(safeBulkVoltage) + ",";
  json += "\"absorptionVoltage\": " + String(safeAbsorptionVoltage) + ",";
  json += "\"floatVoltage\": " + String(safeFloatVoltage) + ",";
  json += "\"currentPWM\": " + String(currentPWM) + ",";
  json += "\"LVD\": " + String(LVD) + ",";
  json += "\"LVR\": " + String(LVR) + ",";
  json += "\"absorptionCurrentThreshold_mA\": " + String(safeabsorptionCurrentThreshold_mA) + ",";
  json += "\"batteryCapacity\": " + String(safeBatteryCapacity) + ",";
  json += "\"thresholdPercentage\": " + String(safeThresholdPercentage) + ",";
  json += "\"calculatedAbsorptionHours\": " + String(safeCalculatedAbsorptionHours) + ",";
  json += "\"accumulatedAh\": " + String(safeAccumulatedAh) + ",";
  json += "\"estimatedSOC\": " + String(safeSOC) + ",";
  json += "\"maxAllowedCurrent\": " + String(safeMaxAllowedCurrent) + ",";
  json += "\"netCurrent\": " + String(safeNetCurrent) + ",";
  json += "\"currentLimitIntoFloatStage\": " + String(safeCurrentLimitIntoFloatStage) + ",";
  json += "\"isLithium\": ";
  json += isLithium ? "true" : "false";
  json += ",";
  json += "\"temperature\": " + String(safeTemperature);
  json += ",";
  // Sanitizar notaPersonalizada para evitar problemas con JSON
  String sanitizedNota = notaPersonalizada;
  sanitizedNota.replace("\"", "\\\""); // Escapar comillas dobles
  json += "\"notaPersonalizada\": \"" + sanitizedNota + "\"";
  json += ",";
  json += "\"useFuenteDC\": ";
  json += useFuenteDC ? "true" : "false";
  json += ",";
  json += "\"fuenteDC_Amps\": " + String(fuenteDC_Amps);
  json += ",";
  json += "\"maxBulkHours\": " + String(maxBulkHours);
  json += "}";
  
  // Log para depuración
  Serial.println("JSON enviado: " + json);
  
  return json;
}