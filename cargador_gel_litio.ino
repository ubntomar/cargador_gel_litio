#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_task_wdt.h"
#include <Preferences.h>
#include "config.h"        // Incluimos el nuevo archivo de configuración
#include "web_server.h"    // Incluir el archivo del servidor web


// ========== PROTOCOLO SERIAL ORANGE PI ==========
// Definir pines para UART0 (comunicación con Orange Pi)
#define RX_PIN_SERIAL 20  // GPIO20 - Pin RX físico
#define TX_PIN_SERIAL 21  // GPIO21 - Pin TX físico

// Crear instancia de HardwareSerial para Orange Pi
HardwareSerial OrangePiSerial(0);  // Usar UART0

// Buffer para comandos seriales
String serialBuffer = "";
bool commandReady = false;


Preferences preferences;
#define WDT_TIMEOUT 15

// Definición de variables para la fuente DC
bool useFuenteDC = false;
float fuenteDC_Amps = 0.0;
float maxBulkHours = 0.0;

// Sensores INA219
Adafruit_INA219 ina219_1(0x40);
Adafruit_INA219 ina219_2(0x41);

// Configuración PWM
const int pwmPin = 2;
const int pwmFrequency = 40000;
const int pwmResolution = 8;

// Configuración de lecturas
const int numSamples = 20;
float maxAllowedCurrent = 6000.0;

//Máximo voltaje de batería
const float maxBatteryVoltageAllowed = 15.0;

// Variable compartida para la nota personalizada
String notaPersonalizada = "";


// Parámetros de carga para baterías de gel
float bulkVoltage = 14.4;
float absorptionVoltage = 14.4;
float floatVoltage = 13.6;

float currentBulkHours = 0.0;
bool panelSensorAvailable = true; // Asumimos que el sensor de panel está disponible

float absorptionCurrentThreshold_mA = 350.0;
float currentLimitIntoFloatStage = 100.0;
int factorDivider;

const float chargedBatteryRestVoltage = 12.88;

// Variables para cálculo dinámico de tiempo de absorción
float accumulatedAh = 0.0;
unsigned long lastUpdateTime = 0;

unsigned long lastSaveTime = 0;
const unsigned long SAVE_INTERVAL = 300000;

// Límite máximo de absorción (respaldo)
const float maxAbsorptionHours = 1.0;

// Variables para tracking
float calculatedAbsorptionHours = 0.0;
unsigned long absorptionStartTime = 0;


unsigned long bulkStartTime = 0;


ChargeState currentState = BULK_CHARGE;

// Variable global de PWM (0-255 antes de invertir)
int currentPWM = 0;

// Almacenar corrientes generadas
float panelToBatteryCurrent = 0;
float batteryToLoadCurrent = 0;

// Parámetros de control de voltaje ya están en config.h
// const float LVD = 12.0;
// const float LVR = 12.5;

// Configuración del punto de acceso
const char *ssid = "Cargador";
const char *password = "12345678";

// Variables para almacenar los valores de entrada
float batteryCapacity = 50.0;
float thresholdPercentage = 1.0;
bool isLithium = false;


float temperature;



// Variables para el control de apagado temporal de la carga
unsigned long loadOffStartTime = 0;
unsigned long loadOffDuration = 0;
bool temporaryLoadOff = false;

float readTemperature();
void saveChargingState();
void updateAhTracking();
void resetChargingCycle();
float calculateAbsorptionTime();
float getSOCFromVoltage(float voltage);
float getAverageCurrent(Adafruit_INA219 &ina);
void updateChargeState(float batteryVoltage, float chargeCurrent);
void bulkControl(float batteryVoltage, float chargeCurrent, float bulkVoltage);
void absorptionControl(float batteryVoltage, float chargeCurrent, float absorptionVoltage);
void absorptionControlToLitium(float chargeCurrent, float batteryToLoadCurrent);
void floatControl(float batteryVoltage, float floatVoltage);
void adjustPWM(int step);
void setPWM(int pwmValue);
String getChargeStateString(ChargeState state);


// ========== FUNCIONES PROTOCOLO SERIAL ==========
void initSerialCommunication() {
  OrangePiSerial.setTxBufferSize(2048);
  OrangePiSerial.begin(9600, SERIAL_8N1, RX_PIN_SERIAL, TX_PIN_SERIAL);
  Serial.println("📡 Comunicación serial con Orange Pi inicializada");
  Serial.printf("  RX: GPIO%d, TX: GPIO%d\n", RX_PIN_SERIAL, TX_PIN_SERIAL);
  Serial.println("  Baudrate: 9600 bps");
}

void handleSerialCommands() {
  while (OrangePiSerial.available()) {
    char inChar = (char)OrangePiSerial.read();
    
    if (inChar == '\n') {
      commandReady = true;
      break;
    } else {
      serialBuffer += inChar;
    }
    
    if (serialBuffer.length() > 200) {
      serialBuffer = "";
      break;
    }
  }
  
  if (commandReady) {
    processSerialCommand(serialBuffer);
    serialBuffer = "";
    commandReady = false;
  }
}

void processSerialCommand(String command) {
  command.trim();
  Serial.println("📨 [Orange Pi] Comando recibido: " + command);
  
  if (command.startsWith("CMD:")) {
    String cmd = command.substring(4);
    
    if (cmd == "GET_DATA") {
      sendDataToOrangePi();
    }
    else if (cmd.startsWith("SET_")) {
      handleSetCommand(cmd);
    }
    else if (cmd.startsWith("TOGGLE_LOAD:")) {
      handleToggleLoad(cmd);
    }
    else if (cmd == "CANCEL_TEMP_OFF") {
      // NUEVO: Comando para cancelar apagado temporal
      if (temporaryLoadOff) {
        temporaryLoadOff = false;
        digitalWrite(LOAD_CONTROL_PIN, HIGH);
        notaPersonalizada = "Apagado temporal cancelado (Orange Pi)";
        OrangePiSerial.println("OK:Temporary load off cancelled");
        Serial.println("✅ [Orange Pi] Apagado temporal cancelado");
      } else {
        OrangePiSerial.println("OK:No temporary off active");
        Serial.println("ℹ️ [Orange Pi] No hay apagado temporal activo");
      }
    }
    else {
      Serial.println("❌ Comando no reconocido: " + cmd);
      OrangePiSerial.println("ERROR:Unknown command");
    }
  }
}



void sendDataToOrangePi() {
  Serial.println("📤 [Orange Pi] Preparando envío de datos completos...");
  
  // Crear JSON con TODOS los datos del sistema
  String json = "{";
  
  // === MEDICIONES EN TIEMPO REAL ===
  json += "\"panelToBatteryCurrent\":" + String(panelToBatteryCurrent) + ",";
  json += "\"batteryToLoadCurrent\":" + String(batteryToLoadCurrent) + ",";
  json += "\"voltagePanel\":" + String(ina219_1.getBusVoltage_V()) + ",";
  json += "\"voltageBatterySensor2\":" + String(ina219_2.getBusVoltage_V()) + ",";
  json += "\"currentPWM\":" + String(currentPWM) + ",";
  json += "\"temperature\":" + String(temperature) + ",";
  json += "\"chargeState\":\"" + getChargeStateString(currentState) + "\",";
  
  // === PARÁMETROS DE CARGA ===
  json += "\"bulkVoltage\":" + String(bulkVoltage) + ",";
  json += "\"absorptionVoltage\":" + String(absorptionVoltage) + ",";
  json += "\"floatVoltage\":" + String(floatVoltage) + ",";
  json += "\"LVD\":" + String(LVD) + ",";
  json += "\"LVR\":" + String(LVR) + ",";
  
  // === CONFIGURACIÓN DE BATERÍA ===
  json += "\"batteryCapacity\":" + String(batteryCapacity) + ",";
  json += "\"thresholdPercentage\":" + String(thresholdPercentage) + ",";
  json += "\"maxAllowedCurrent\":" + String(maxAllowedCurrent) + ",";
  json += "\"isLithium\":" + String(isLithium ? "true" : "false") + ",";
  json += "\"maxBatteryVoltageAllowed\":" + String(maxBatteryVoltageAllowed) + ",";
  
  // === PARÁMETROS CALCULADOS ===
  json += "\"absorptionCurrentThreshold_mA\":" + String(absorptionCurrentThreshold_mA) + ",";
  json += "\"currentLimitIntoFloatStage\":" + String(currentLimitIntoFloatStage) + ",";
  json += "\"calculatedAbsorptionHours\":" + String(calculatedAbsorptionHours) + ",";
  json += "\"accumulatedAh\":" + String(accumulatedAh) + ",";
  json += "\"estimatedSOC\":" + String(getSOCFromVoltage(ina219_2.getBusVoltage_V())) + ",";
  json += "\"netCurrent\":" + String(panelToBatteryCurrent - batteryToLoadCurrent) + ",";
  json += "\"factorDivider\":" + String(factorDivider) + ",";
  json += "\"currentBulkHours\":" + String(currentBulkHours) + ",";
  json += "\"panelSensorAvailable\":" + String(panelSensorAvailable ? "true" : "false") + ",";
  // === CONFIGURACIÓN DE FUENTE ===
  json += "\"useFuenteDC\":" + String(useFuenteDC ? "true" : "false") + ",";
  json += "\"fuenteDC_Amps\":" + String(fuenteDC_Amps) + ",";
  json += "\"maxBulkHours\":" + String(maxBulkHours) + ",";
  
  // === CONFIGURACIÓN AVANZADA ===
  json += "\"maxAbsorptionHours\":" + String(maxAbsorptionHours) + ",";
  json += "\"chargedBatteryRestVoltage\":" + String(chargedBatteryRestVoltage) + ",";
  json += "\"reEnterBulkVoltage\":12.6,"; // Valor fijo por ahora
  json += "\"pwmFrequency\":" + String(pwmFrequency) + ",";
  json += "\"tempThreshold\":55,"; // Valor fijo por ahora
  
  // === ESTADO DE APAGADO TEMPORAL ===
  json += "\"temporaryLoadOff\":" + String(temporaryLoadOff ? "true" : "false") + ",";
  if (temporaryLoadOff) {
    unsigned long remainingTime = 0;
    unsigned long elapsed = millis() - loadOffStartTime;
    if (elapsed < loadOffDuration) {
      remainingTime = (loadOffDuration - elapsed) / 1000; // Convertir a segundos
    }
    json += "\"loadOffRemainingSeconds\":" + String(remainingTime) + ",";
    json += "\"loadOffDuration\":" + String(loadOffDuration / 1000) + ",";
  } else {
    json += "\"loadOffRemainingSeconds\":0,";
    json += "\"loadOffDuration\":0,";
  }
  
  // === ESTADO DEL SISTEMA ===
  json += "\"loadControlState\":" + String(digitalRead(LOAD_CONTROL_PIN) ? "true" : "false") + ",";
  json += "\"ledSolarState\":" + String(digitalRead(LED_SOLAR) ? "true" : "false") + ",";
  
  // Sanitizar nota personalizada para JSON
  String sanitizedNota = notaPersonalizada;
  sanitizedNota.replace("\"", "\\\"");
  sanitizedNota.replace("\n", "\\n");
  sanitizedNota.replace("\r", "");
  json += "\"notaPersonalizada\":\"" + sanitizedNota + "\",";
  
  // === METADATOS ===
  json += "\"connected\":true,";
  json += "\"firmware_version\":\"ESP32_v2.1\",";
  json += "\"uptime\":" + String(millis()) + ",";
  json += "\"last_update\":\"" + String(millis()) + "\"";
  
  json += "}";
  Serial.println("📏 Tamaño JSON: " + String(json.length()) + " caracteres");
  // Verificar tamaño del JSON antes de enviar
  if (json.length() > 2000) {
    Serial.println("⚠️ [Orange Pi] JSON muy largo (" + String(json.length()) + " chars), dividiendo...");
    // Por ahora, solo registrar el warning
  }
  
  // Enviar JSON a Orange Pi
  OrangePiSerial.println(json);
  Serial.println("📤 [Orange Pi] Datos completos enviados: " + String(json.length()) + " caracteres");
  
  // Debug: mostrar primeros 200 caracteres del JSON
  Serial.println("📋 [Orange Pi] JSON preview: " + json.substring(0, min(1050, (int)json.length())) + "...");
}




void handleSetCommand(String cmd) {
  int colonIndex = cmd.indexOf(':');
  if (colonIndex == -1) {
    OrangePiSerial.println("ERROR:Invalid SET format");
    return;
  }
  
  String parameter = cmd.substring(4, colonIndex); // Remover "SET_"
  String valueStr = cmd.substring(colonIndex + 1);
  float value = valueStr.toFloat();
  
  bool success = false;
  String response = "OK:";
  
  Serial.println("🔧 [Orange Pi] Procesando SET " + parameter + " = " + valueStr);
  
  // === PARÁMETROS BÁSICOS ===
  if (parameter == "batteryCapacity") {
    if (value > 0 && value <= 1000) {
      batteryCapacity = value;
      // Recalcular parámetros dependientes
      absorptionCurrentThreshold_mA = (batteryCapacity * thresholdPercentage) * 10;
      currentLimitIntoFloatStage = absorptionCurrentThreshold_mA / factorDivider;
      success = true;
    }
  }
  else if (parameter == "thresholdPercentage") {
    if (value >= 0.1 && value <= 5.0) {
      thresholdPercentage = value;
      absorptionCurrentThreshold_mA = (batteryCapacity * thresholdPercentage) * 10;
      currentLimitIntoFloatStage = absorptionCurrentThreshold_mA / factorDivider;
      success = true;
    }
  }
  else if (parameter == "maxAllowedCurrent") {
    if (value >= 1000 && value <= 15000) {
      maxAllowedCurrent = value;
      success = true;
    }
  }
  else if (parameter == "bulkVoltage") {
    if (value >= 12.0 && value <= 15.0) {
      bulkVoltage = value;
      success = true;
    }
  }
  else if (parameter == "absorptionVoltage") {
    if (value >= 12.0 && value <= 15.0) {
      absorptionVoltage = value;
      success = true;
    }
  }
  else if (parameter == "floatVoltage") {
    if (value >= 12.0 && value <= 15.0) {
      floatVoltage = value;
      success = true;
    }
  }
  
  // === PARÁMETROS DE TIPO BOOLEAN ===
  else if (parameter == "isLithium") {
    isLithium = (valueStr == "true" || valueStr == "1");
    success = true;
    Serial.println("🔋 [Orange Pi] Tipo de batería cambiado a: " + String(isLithium ? "Litio" : "GEL"));
  }
  else if (parameter == "useFuenteDC") {
    useFuenteDC = (valueStr == "true" || valueStr == "1");
    success = true;
    Serial.println("⚡ [Orange Pi] Fuente de energía cambiada a: " + String(useFuenteDC ? "DC" : "Solar"));
  }
  
  // === PARÁMETROS DE FUENTE DC ===
  else if (parameter == "fuenteDC_Amps") {
    if (value >= 0 && value <= 50) {
      fuenteDC_Amps = value;
      // Recalcular horas máximas en Bulk
      if (useFuenteDC && fuenteDC_Amps > 0) {
        maxBulkHours = batteryCapacity / fuenteDC_Amps;
      } else {
        maxBulkHours = 0.0;
      }
      success = true;
    }
  }
  
  // === PARÁMETROS AVANZADOS (agregar según necesites) ===
  else if (parameter == "LVD") {
    if (value >= 10.0 && value <= 13.0) {
      // Si tienes LVD como variable, descomenta:
      // LVD = value;
      success = true;
    }
  }
  else if (parameter == "LVR") {
    if (value >= 11.0 && value <= 14.0) {
      // Si tienes LVR como variable, descomenta:
      // LVR = value;
      success = true;
    }
  }
  
  else if (parameter == "factorDivider") {
    if (value >= 1 && value <= 10) {
      factorDivider = (int)value;
      currentLimitIntoFloatStage = absorptionCurrentThreshold_mA / factorDivider;
      success = true;
    }
  }

  // === PARÁMETRO NO RECONOCIDO ===
  else {
    response = "ERROR:Unknown parameter: " + parameter;
    Serial.println("❌ [Orange Pi] Parámetro no reconocido: " + parameter);
  }
  
  // === GUARDAR EN PREFERENCES SI FUE EXITOSO ===
  if (success) {
    preferences.begin("charger", false);
    
    // Guardar según el parámetro
    if (parameter == "batteryCapacity") preferences.putFloat("batteryCap", batteryCapacity);
    else if (parameter == "thresholdPercentage") preferences.putFloat("thresholdPerc", thresholdPercentage);
    else if (parameter == "maxAllowedCurrent") preferences.putFloat("maxCurrent", maxAllowedCurrent);
    else if (parameter == "bulkVoltage") preferences.putFloat("bulkV", bulkVoltage);
    else if (parameter == "absorptionVoltage") preferences.putFloat("absV", absorptionVoltage);
    else if (parameter == "floatVoltage") preferences.putFloat("floatV", floatVoltage);
    else if (parameter == "isLithium") preferences.putBool("isLithium", isLithium);
    else if (parameter == "useFuenteDC") preferences.putBool("useFuenteDC", useFuenteDC);
    else if (parameter == "fuenteDC_Amps") preferences.putFloat("fuenteDC_Amps", fuenteDC_Amps);
    
    preferences.end();
    
    response += parameter + " updated to " + valueStr;
    notaPersonalizada = "Parámetro " + parameter + " actualizado desde Orange Pi a " + valueStr;
    
    Serial.println("✅ [Orange Pi] " + response);
    Serial.println("💾 [Orange Pi] Parámetro guardado en Preferences");
  } else {
    response = "ERROR:Invalid value for " + parameter + " (received: " + valueStr + ")";
    Serial.println("❌ [Orange Pi] " + response);
  }
  
  // Enviar respuesta a Orange Pi
  OrangePiSerial.println(response);
}


void handleToggleLoad(String cmd) {
  int colonIndex = cmd.indexOf(':');
  if (colonIndex == -1) {
    OrangePiSerial.println("ERROR:Invalid TOGGLE_LOAD format");
    return;
  }
  
  int seconds = cmd.substring(colonIndex + 1).toInt();
  
  Serial.println("🔌 [Orange Pi] Solicitud de apagado temporal: " + String(seconds) + " segundos");
  
  // CAMBIO: Aumentar límite a 43200 segundos (12 horas)
  if (seconds >= 1 && seconds <= 43200) {
    if (digitalRead(LOAD_CONTROL_PIN) == HIGH) {
      digitalWrite(LOAD_CONTROL_PIN, LOW);
      temporaryLoadOff = true;
      loadOffStartTime = millis();
      loadOffDuration = seconds * 1000UL; // UL para evitar overflow
      
      notaPersonalizada = "Carga apagada por " + String(seconds) + " segundos (Orange Pi)";
      OrangePiSerial.println("OK:Load turned off for " + String(seconds) + " seconds");
      Serial.println("🔌 [Orange Pi] ✅ Carga apagada por " + String(seconds) + " segundos");
    } else {
      OrangePiSerial.println("OK:Load already off");
      Serial.println("⚠️ [Orange Pi] La carga ya estaba apagada");
    }
  } else {
    String errorMsg = "ERROR:Invalid time range (1-43200 seconds), received: " + String(seconds);
    OrangePiSerial.println(errorMsg);
    Serial.println("❌ [Orange Pi] Tiempo fuera de rango: " + String(seconds) + " segundos");
  }
}


void periodicSerialUpdate() {
  static unsigned long lastAutoUpdate = 0;
  unsigned long now = millis();
  
  if (now - lastAutoUpdate > 30000) {
    if (!commandReady && serialBuffer.length() == 0) {
      OrangePiSerial.println("HEARTBEAT:ESP32 Online");
      Serial.println("💓 [Orange Pi] Heartbeat enviado");
    }
    lastAutoUpdate = now;
  }
}





void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("Iniciando sensores INA219...");

  // Pines de control
  pinMode(LOAD_CONTROL_PIN, OUTPUT);
  pinMode(LED_SOLAR, OUTPUT);
  
  notaPersonalizada = "Sistema iniciado correctamente";
  
  digitalWrite(LED_SOLAR, LOW);

  pinMode(TEMP_PIN, INPUT);

  // Configuración del watchdog
  esp_task_wdt_config_t wdtConfig = {
      .timeout_ms = WDT_TIMEOUT * 1000
  };

  if (esp_task_wdt_init(&wdtConfig) == ESP_OK) {
    Serial.println("Watchdog iniciado correctamente.");
  } else {
    Serial.println("Error al iniciar el Watchdog.");
  }

  esp_task_wdt_add(NULL);

  // Inicializar I2C
  Wire.begin(SDA_PIN, SCL_PIN);

  // Inicializar sensores INA219
  if (!ina219_1.begin()) {
    Serial.println("No se pudo encontrar INA219 en 0x40.");
    while (1);
  }
  if (!ina219_2.begin()) {
    Serial.println("No se pudo encontrar INA219 en 0x41.");
    while (1);
  }

  ina219_1.setCalibration_32V_2A();
  ina219_2.setCalibration_32V_2A();

  Serial.println("Sensores INA219 listos.");

  // Configurar PWM
  bool success = ledcAttach(pwmPin, pwmFrequency, pwmResolution);
  if (!success) {
    Serial.println("Error al configurar el PWM");
    while (true);
  }

  // Iniciar PWM en cero
  setPWM(10);
  currentState = BULK_CHARGE;

  // Añadir detección inicial del estado de la batería
  float initialBatteryVoltage = ina219_2.getBusVoltage_V();

  if (initialBatteryVoltage >= chargedBatteryRestVoltage) {
    if (!isLithium) {
      currentState = FLOAT_CHARGE;
      Serial.println("Batería GEL detectada con carga alta - iniciando en FLOAT_CHARGE");
    } else {
      currentState = ABSORPTION_CHARGE;
      Serial.println("Batería LITIO detectada con carga alta - iniciando en FLOAT_CHARGE");
    }
  } else {
    currentState = BULK_CHARGE;
    Serial.println("Batería requiere carga - iniciando en BULK_CHARGE");
  }

  if(initialBatteryVoltage>=12.0){
    digitalWrite(LOAD_CONTROL_PIN, HIGH);
  } else{
    digitalWrite(LOAD_CONTROL_PIN, LOW);

  }

  // Configurar el punto de acceso
  WiFi.softAP(ssid, password);
  Serial.println("Punto de acceso iniciado");
  Serial.print("IP del servidor: ");
  Serial.println(WiFi.softAPIP());

  // Iniciar Preferences en modo lectura
 // Iniciar Preferences en modo lectura
  preferences.begin("charger", true);
  batteryCapacity = preferences.getFloat("batteryCap", 50.0);
  thresholdPercentage = preferences.getFloat("thresholdPerc", 1.0);
  maxAllowedCurrent = preferences.getFloat("maxCurrent", 6000.0);
  accumulatedAh = preferences.getFloat("accumulatedAh", 0.0);
  bulkVoltage = preferences.getFloat("bulkV", 14.4);
  absorptionVoltage = preferences.getFloat("absV", 14.4);
  floatVoltage = preferences.getFloat("floatV", 13.6);
  isLithium = preferences.getBool("isLithium", false);
  useFuenteDC = preferences.getBool("useFuenteDC", false);
  fuenteDC_Amps = preferences.getFloat("fuenteDC_Amps", 0.0);
  bulkStartTime = preferences.getULong("bulkStartTime", 0);
  preferences.end();

  // Actualizar absorptionCurrentThreshold_mA
  absorptionCurrentThreshold_mA = (batteryCapacity * thresholdPercentage) * 10;
  factorDivider = 5;
  currentLimitIntoFloatStage = absorptionCurrentThreshold_mA / factorDivider;


  // Calcular el tiempo máximo de Bulk si se usa fuente DC y los amperios son > 0
  if (useFuenteDC && fuenteDC_Amps > 0) {
    maxBulkHours = batteryCapacity / fuenteDC_Amps;
    notaPersonalizada = "Tiempo máx. en Bulk: " + String(maxBulkHours, 1) + " horas";
  } else {
    maxBulkHours = 0.0;
    notaPersonalizada = "Usando paneles solares";
  }

  // Iniciar el servidor web
  initWebServer();
  initSerialCommunication();
}

void loop() {
  esp_task_wdt_reset();

  unsigned long now = millis();

  updateAhTracking();
  saveChargingState();

  handleSerialCommands();
  periodicSerialUpdate();

  // Leer datos de sensores
  panelToBatteryCurrent = getAverageCurrent(ina219_1);
  batteryToLoadCurrent = getAverageCurrent(ina219_2);
  float voltagePanel = ina219_1.getBusVoltage_V();
  float voltageBatterySensor2 = ina219_2.getBusVoltage_V();

  // Encender LED si hay corriente desde el panel
  if (panelToBatteryCurrent > 50) {
    digitalWrite(LED_SOLAR, HIGH);
  } else {
    digitalWrite(LED_SOLAR, LOW);
  }

  // Mostrar en serial
  Serial.println("--------------------------------------------");
  Serial.print("Panel->Batería: Corriente = ");
  Serial.print(panelToBatteryCurrent);
  Serial.print(" mA, VoltajePanel = ");
  Serial.print(voltagePanel);
  Serial.println(" V");

  Serial.print("Batería->Carga : Corriente = ");
  Serial.print(batteryToLoadCurrent);
  Serial.print(" mA, VoltajeBat = ");
  Serial.print(voltageBatterySensor2);
  Serial.println(" V");

  Serial.print("Estado de carga: ");
  Serial.println(getChargeStateString(currentState));

  Serial.print("Voltaje etapa BULK: ");
  Serial.println(bulkVoltage);

  if (panelToBatteryCurrent <= 10.0 && currentPWM != 0) {
    currentPWM = 0;
    Serial.println("Forzando el PWM a 0 ya que NO se detecta presencia de corriente de páneles solares");
  }

  // Control de voltaje (LVD y LVR)
  if (!temporaryLoadOff) {
    if (voltageBatterySensor2 < LVD || voltageBatterySensor2 > maxBatteryVoltageAllowed) {
      digitalWrite(LOAD_CONTROL_PIN, LOW);
      Serial.println("Desactivando el sistema (voltaje < LVD | voltageBatterySensor2 > maxBatteryVoltageAllowed)");
    } else if (voltageBatterySensor2 > LVR && voltageBatterySensor2 < maxBatteryVoltageAllowed) {
      digitalWrite(LOAD_CONTROL_PIN, HIGH);
      Serial.println("Reactivando el sistema (voltaje > LVR && voltageBatterySensor2 < maxBatteryVoltageAllowed)");
    }
  } else {
    // Si hay un apagado temporal activo, verificar si debe terminar
    if (millis() - loadOffStartTime >= loadOffDuration) {
      temporaryLoadOff = false;
      digitalWrite(LOAD_CONTROL_PIN, HIGH);
      notaPersonalizada = "Apagado temporal completado, carga reactivada";
      Serial.println("⏰ Apagado temporal completado, carga reactivada");
    }
  }

  // RE-ENTRY CHECK
  const float reEnterBulkVoltage = 12.6;
  const unsigned long reEnterTime = 30000UL;
  static unsigned long lowVoltageStart = 0;
  static bool belowThreshold = false;

  if (voltageBatterySensor2 < reEnterBulkVoltage) {
    if (!belowThreshold) {
      belowThreshold = true;
      lowVoltageStart = millis();
    } else {
      if (millis() - lowVoltageStart >= reEnterTime) {
        if (currentState != BULK_CHARGE) {
          currentState = BULK_CHARGE;
          Serial.println("-> Forzando retorno a BULK_CHARGE (batería < 12.6 V por 30s)");
        }
      }
    }
  } else {
    belowThreshold = false;
    lowVoltageStart = 0;
  }

  updateChargeState(voltageBatterySensor2, panelToBatteryCurrent);


  // Recalcular horas máximas si se usa fuente DC
  if (useFuenteDC && fuenteDC_Amps > 0) {
    maxBulkHours = batteryCapacity / fuenteDC_Amps;
    
    // Solo actualizar la nota si no estamos en estado de ERROR
    if (currentState != ERROR) {
      if (currentState == BULK_CHARGE) {
        // La nota ya se actualiza en el control de Bulk
      } else {
        notaPersonalizada = "Tiempo máx. en Bulk: " + String(maxBulkHours, 1) + " horas";
      }
    }
  } else {
    maxBulkHours = 0.0;
    
    // Solo actualizar la nota si no estamos en estado de ERROR
    if (currentState != ERROR) {
      notaPersonalizada = "Usando paneles solares";
    }
  }


  temperature = readTemperature();
  Serial.print("Temperatura: ");
  Serial.print(temperature);
  Serial.println(" °C");
  if (temperature >= TEMP_THRESHOLD_SHUTDOWN) {
    Serial.println("¡Temperatura crítica! Apagando el circuito...");
    currentState = ERROR;
  }
  Serial.println("Panel->Batería: " + String(panelToBatteryCurrent) + " mA");
  Serial.println("Batería->Carga: " + String(batteryToLoadCurrent) + " mA");
  Serial.println("Voltaje Panel: " + String(ina219_1.getBusVoltage_V()) + " V");
  Serial.println("Voltaje Batería: " + String(ina219_2.getBusVoltage_V()) + " V");
  Serial.println("Estado: " + getChargeStateString(currentState));
  Serial.println("pwmValue: " + String(currentPWM));
  handleWebServer();

  delay(1000);
}

void saveChargingState() {
  if (millis() - lastSaveTime > SAVE_INTERVAL) {
    preferences.begin("charger", false);
    preferences.putFloat("accumulatedAh", accumulatedAh);
    preferences.putULong("bulkStartTime", bulkStartTime);
    preferences.end();
    lastSaveTime = millis();
  }
}

void updateAhTracking() {
  unsigned long now = millis();
  float deltaHours = (now - lastUpdateTime) / 3600000.0;
  float chargeCurrent = panelToBatteryCurrent / 1000.0;
  float dischargeCurrent = batteryToLoadCurrent / 1000.0;
  float ahChange = (chargeCurrent * deltaHours) - (dischargeCurrent * deltaHours);
  accumulatedAh += ahChange;
  lastUpdateTime = now;
}

void resetChargingCycle() {
  if (currentState == FLOAT_CHARGE) {
    accumulatedAh = batteryCapacity * 0.95;
  } else {
    accumulatedAh = 0;
  }
  saveChargingState();
}

float calculateAbsorptionTime() {
  float chargeCurrent = panelToBatteryCurrent / 1000.0;
  if (chargeCurrent <= 0) {
    return maxAbsorptionHours / 2;
  }
  float chargedPercentage = (accumulatedAh / batteryCapacity) * 100.0;
  float remainingCapacity = batteryCapacity * ((100.0 - chargedPercentage) / 100.0);
  remainingCapacity *= 1.1;
  float calculatedTime = remainingCapacity / chargeCurrent;
  return min(calculatedTime, maxAbsorptionHours);
}

float getSOCFromVoltage(float voltage) {
  if (voltage >= 14.4) return 90.0;
  else if (voltage >= 13.2) return map(voltage, 13.2, 14.4, 20.0, 90.0);
  else if (voltage >= 12.0) return map(voltage, 12.0, 13.2, 0.0, 20.0);
  return 0.0;
}

float getAverageCurrent(Adafruit_INA219 &ina) {
  float totalCurrent = 0;
  int validSamples = 0;
  for (int i = 0; i < numSamples; i++) {
    float current_mA = ina.getCurrent_mA() * 10; // shunt 10 mΩ
    if (current_mA >= 0 && current_mA <= maxAllowedCurrent) {
      totalCurrent += current_mA;
      validSamples++;
    }
    delay(5);
  }
  if (validSamples == 0) return 0;
  return totalCurrent / validSamples;
}

void updateChargeState(float batteryVoltage, float chargeCurrent) {
  float batteryNetCurrent;
  float batteryNetCurrentAmps;
  float initialSOC = 0.0;
  if (batteryVoltage >= maxBatteryVoltageAllowed) {
    currentState = ERROR;
    Serial.println("ERROR: Voltaje de batería demasiado alto");
    //return;
  }

  switch (currentState) {
    case BULK_CHARGE:
      bulkControl(batteryVoltage, chargeCurrent, bulkVoltage);
      
      // Agregar control de tiempo para fuente DC
      if (bulkStartTime == 0) {
        // Asegurarse de que bulkStartTime sea inicializado solo una vez al entrar en modo BULK
        bulkStartTime = millis();
        Serial.println("Inicializado bulkStartTime: " + String(bulkStartTime));
        
        // Guardar inmediatamente el valor inicial
        preferences.begin("charger", false);
        preferences.putULong("bulkStartTime", bulkStartTime);
        preferences.end();
      }
      
      // Verificar si debemos salir de BULK por voltaje
      if (batteryVoltage >= bulkVoltage) {
        initialSOC = (accumulatedAh / batteryCapacity) * 100.0;
        float socFromVoltage = getSOCFromVoltage(batteryVoltage);
        initialSOC = min(initialSOC, socFromVoltage);
        currentState = ABSORPTION_CHARGE;
        absorptionStartTime = millis();
        bulkStartTime = 0; // Resetear para próximo ciclo
        preferences.begin("charger", false);
        preferences.putULong("bulkStartTime", 0);
        preferences.end();
        Serial.println("-> Transición a ABSORPTION_CHARGE por voltaje");
      } 
      // Verificar si debemos salir de BULK por tiempo (solo con fuente DC)
      else if (useFuenteDC && fuenteDC_Amps > 0 && maxBulkHours > 0) {
        // Corregido: asegurar que el cálculo se realiza correctamente como float
        currentBulkHours = (float)(millis() - bulkStartTime) / 3600000.0f;
        
        // Actualizar nota con tiempo transcurrido
        notaPersonalizada = "Bulk: " + String(currentBulkHours, 1) + "h de " + String(maxBulkHours, 1) + "h máx";
        
        if (currentBulkHours >= maxBulkHours) {
          currentState = ABSORPTION_CHARGE;
          absorptionStartTime = millis();
          bulkStartTime = 0; // Resetear para próximo ciclo
          preferences.begin("charger", false);
          preferences.putULong("bulkStartTime", 0);
          preferences.end();
          notaPersonalizada = "Transición a ABSORPTION_CHARGE por tiempo máximo";
          Serial.println("-> Transición a ABSORPTION_CHARGE por tiempo máximo en BULK");
        }
      }
      break;

    case ABSORPTION_CHARGE:
      absorptionControl(batteryVoltage, chargeCurrent, absorptionVoltage);
      batteryNetCurrent = panelToBatteryCurrent - batteryToLoadCurrent;
      batteryNetCurrentAmps = batteryNetCurrent / 1000.0;
      if (batteryNetCurrentAmps <= 0) {
        calculatedAbsorptionHours = maxAbsorptionHours / 2;
        Serial.println("No hay carga neta en la batería, usando tiempo conservador");
      } else {
        float chargedPercentage = (accumulatedAh / batteryCapacity) * 100.0;
        float remainingCapacity = batteryCapacity * ((100.0 - chargedPercentage) / 100.0);
        remainingCapacity *= 1.1;
        calculatedAbsorptionHours = remainingCapacity / batteryNetCurrentAmps;
        if (calculatedAbsorptionHours > maxAbsorptionHours) {
          calculatedAbsorptionHours = maxAbsorptionHours;
          Serial.println("Tiempo calculado excede máximo, limitando a " + String(maxAbsorptionHours) + "h");
        }
      }
      Serial.println("Corriente neta en batería: " + String(batteryNetCurrent) + " mA");
      Serial.println("Tiempo de absorción calculado: " + String(calculatedAbsorptionHours) + " horas");
      if (batteryNetCurrent <= absorptionCurrentThreshold_mA) {
        if (!isLithium) {
          currentState = FLOAT_CHARGE;
          resetChargingCycle();
          Serial.println("-> Transición a FLOAT_CHARGE (corriente neta < threshold)");
        } else {
          Serial.println("Batería de litio: Ignorando etapa FLOAT");
          absorptionControlToLitium(chargeCurrent, batteryToLoadCurrent);
        }
      }
      else if ((millis() - absorptionStartTime) / 1000.0 / 3600.0 >= calculatedAbsorptionHours) {
        if (!isLithium) {
          currentState = FLOAT_CHARGE;
          resetChargingCycle();
          Serial.println("-> Transición a FLOAT_CHARGE (tiempo calculado alcanzado)");
        } else {
          Serial.println("Batería de litio: Ignorando etapa FLOAT");
          currentState = ABSORPTION_CHARGE;
          Serial.println("-> Transición a ABSORPTION_CHARGE");
        }
      }
      break;

    case FLOAT_CHARGE:
      if (!isLithium) {
        calculatedAbsorptionHours = 0;
        if (chargeCurrent <= (currentLimitIntoFloatStage + batteryToLoadCurrent)) {
          floatControl(batteryVoltage, floatVoltage);
        } else {
          Serial.println("Corriente excesiva detectada en FLOAT_CHARGE. Reduciendo PWM.");
          adjustPWM(-5);
        }
      } else {
        Serial.println("Batería de litio: Ignorando etapa FLOAT");
        currentState = ABSORPTION_CHARGE;
        Serial.println("-> Transición a ABSORPTION_CHARGE");
      }
      break;

    case ERROR:
      digitalWrite(LOAD_CONTROL_PIN, LOW);
      setPWM(20);
      notaPersonalizada = "estoy en la sección Error"; 
      while (temperature >= TEMP_THRESHOLD_SHUTDOWN ||
          batteryVoltage >= maxBatteryVoltageAllowed ) {
          pinMode(LED_SOLAR, OUTPUT);
          delay(100);
          digitalWrite(LED_SOLAR, LOW);
          delay(100);
          digitalWrite(LED_SOLAR, HIGH);
          delay(100);
          esp_task_wdt_reset();  // Reset para evitar reinicio
          batteryVoltage = ina219_2.getBusVoltage_V();
      }
      currentState =ABSORPTION_CHARGE;
      break;
  }
}

void bulkControl(float batteryVoltage, float chargeCurrent, float bulkVoltage) {
  if (chargeCurrent > maxAllowedCurrent) {
    adjustPWM(-5);
  } else if (batteryVoltage < bulkVoltage) {
    adjustPWM(+1);
  } else {
    adjustPWM(-1);
  }
}

void absorptionControl(float batteryVoltage, float chargeCurrent, float absorptionVoltage) {
  if (batteryVoltage > absorptionVoltage) {
    adjustPWM(-1);
  } else if (batteryVoltage < absorptionVoltage) {
    if (chargeCurrent < maxAllowedCurrent) {
      adjustPWM(+1);
    } else {
      adjustPWM(-2);
    }
  }
}

void absorptionControlToLitium(float chargeCurrent, float batteryToLoadCurrent) {
  if (chargeCurrent > batteryToLoadCurrent) {
    adjustPWM(-3);
  } else {
    adjustPWM(+1);
  }
}

void floatControl(float batteryVoltage, float floatVoltage) {
  if (batteryVoltage > floatVoltage) {
    adjustPWM(-1);
  } else if (batteryVoltage < floatVoltage) {
    adjustPWM(+1);
  }
}

void adjustPWM(int step) {
  currentPWM += step;
  currentPWM = constrain(currentPWM, 0, 255);
  setPWM(currentPWM);
}

void setPWM(int pwmValue) {
  pwmValue = constrain(pwmValue, 0, 255);
  int dutyCyclePercentage = map(pwmValue, 0, 255, 0, 100);
  int invertedDutyCycle = 255 - (dutyCyclePercentage * 255 / 100);
  ledcWrite(pwmPin, 255 - pwmValue);
  Serial.print("PWM calculado: ");
  Serial.print(pwmValue);
  Serial.print(" (");
  Serial.print(dutyCyclePercentage);
  Serial.print("%), invertido -> ");
  Serial.println(invertedDutyCycle);
}

String getChargeStateString(ChargeState state) {
  switch (state) {
    case BULK_CHARGE:
      return "BULK_CHARGE";
    case ABSORPTION_CHARGE:
      return "ABSORPTION_CHARGE";
    case FLOAT_CHARGE:
      return "FLOAT_CHARGE";
    case ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

float readTemperature() {
  float adcValue = 0.0;

  // Promediado de las lecturas del ADC
  for (int i = 0; i < NUM_SAMPLES; i++) {
    adcValue += analogRead(TEMP_PIN);
    delay(5);  // Pausa pequeña entre lecturas
  }
  adcValue /= NUM_SAMPLES;

  // Convertir el valor ADC a voltaje
  float voltage = (VCC * adcValue) / ADC_RESOLUTION;

  // Calcular la resistencia del NTC usando el divisor de voltaje
  float ntcResistance = (SERIES_RESISTOR * voltage) / (VCC - voltage);

  // Calcular la temperatura usando Steinhart-Hart
  float temperature;
  temperature = ntcResistance / NOMINAL_RESISTANCE;  // R/Ro
  temperature = log(temperature);                      // ln(R/Ro)
  temperature /= BETA;                                 // (1/B) * ln(R/Ro)
  temperature += 1.0 / (NOMINAL_TEMPERATURE + 273.15);   // + (1/To)
  temperature = 1.0 / temperature;                     // Invertir
  temperature -= 273.15;                               // Convertir a °C

  return temperature;
}
