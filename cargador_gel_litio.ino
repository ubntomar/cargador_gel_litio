#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_task_wdt.h"
#include <Preferences.h>
#include "web_server.h"  // Incluir el archivo del servidor web------

Preferences preferences;
#define WDT_TIMEOUT 5

// Definición de pines I2C
#define SDA_PIN 8
#define SCL_PIN 9

// Pines de control
#define LOAD_CONTROL_PIN 7
#define LED_SOLAR 3

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

// Parámetros de carga para baterías de gel
float bulkVoltage = 14.4;
float absorptionVoltage = 14.4;
float floatVoltage = 13.6;

float absorptionCurrentThreshold = 350.0;
float currentLimitIntoFloatStage = 100.0;
int factorDivider;

const float chargedBatteryRestVoltage = 12.88;

// Variables para cálculo dinámico de tiempo de absorción
float accumulatedAh = 0.0;
unsigned long lastUpdateTime = 0;

unsigned long lastSaveTime = 0;
const unsigned long SAVE_INTERVAL = 300000;

// Límite máximo de absorción (respaldo)
const float maxAbsorptionHours = 8.0;

// Variables para tracking
float calculatedAbsorptionHours = 0.0;
unsigned long absorptionStartTime = 0;



ChargeState currentState = BULK_CHARGE;

// Variable global de PWM (0-255 antes de invertir)
int currentPWM = 0;

// Almacenar corrientes generadas
float panelToBatteryCurrent = 0;
float batteryToLoadCurrent = 0;

// Parámetros de control de voltaje
const float LVD = 12.0;
const float LVR = 12.5;

// Configuración del punto de acceso
const char *ssid = "Cargador";
const char *password = "12345678";

// Variables para almacenar los valores de entrada
float batteryCapacity = 50.0;
float thresholdPercentage = 1.0;
bool isLithium = false;

// Definiciones para el sensor de temperatura
#define TEMP_PIN A3
#define SERIES_RESISTOR 10000.0
#define NOMINAL_RESISTANCE 10000.0
#define NOMINAL_TEMPERATURE 25.0
#define BETA 3984.0
#define ADC_RESOLUTION 4095.0
#define VCC 3.3
#define NUM_SAMPLES 20
float temperature;

#define TEMP_THRESHOLD_SHUTDOWN 300

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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Iniciando sensores INA219...");

  // Pines de control
  pinMode(LOAD_CONTROL_PIN, OUTPUT);
  pinMode(LED_SOLAR, OUTPUT);
  digitalWrite(LOAD_CONTROL_PIN, HIGH);
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
  setPWM(0);
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
  preferences.end();

  // Actualizar absorptionCurrentThreshold
  absorptionCurrentThreshold = (batteryCapacity * thresholdPercentage) * 10;
  factorDivider = 5;
  currentLimitIntoFloatStage = absorptionCurrentThreshold / factorDivider;

  // Iniciar el servidor web
  initWebServer();
}

void loop() {
  esp_task_wdt_reset();

  unsigned long now = millis();

  updateAhTracking();
  saveChargingState();

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
  if (voltageBatterySensor2 < LVD) {
    digitalWrite(LOAD_CONTROL_PIN, LOW);
    Serial.println("Desactivando el sistema (voltaje < LVD)");
  } else if (voltageBatterySensor2 > LVR) {
    digitalWrite(LOAD_CONTROL_PIN, HIGH);
    Serial.println("Reactivando el sistema (voltaje > LVR)");
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
  if (batteryVoltage >= 14.8) {
    currentState = ERROR;
    Serial.println("ERROR: Voltaje de batería demasiado alto");
  }

  switch (currentState) {
    case BULK_CHARGE:
      bulkControl(batteryVoltage, chargeCurrent, bulkVoltage);
      if (batteryVoltage >= bulkVoltage) {
        initialSOC = (accumulatedAh / batteryCapacity) * 100.0;
        float socFromVoltage = getSOCFromVoltage(batteryVoltage);
        initialSOC = min(initialSOC, socFromVoltage);
        currentState = ABSORPTION_CHARGE;
        absorptionStartTime = millis();
        Serial.println("-> Transición a ABSORPTION_CHARGE");
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
      if (batteryNetCurrent <= absorptionCurrentThreshold) {
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
      setPWM(0);
      Serial.println("Estado de error detectado. Reiniciando el sistema.");
      if (temperature >= TEMP_THRESHOLD_SHUTDOWN) {
        Serial.println("Dejamos cortado el paso de corriente a través de Mosfets.");
        while(true) {
          delay(500);
          digitalWrite(LED_SOLAR, LOW);
          delay(500);
          digitalWrite(LED_SOLAR, HIGH);
          delay(500);
          esp_task_wdt_reset();  // Reset para evitar reinicio
        }
      }
      else {
        Serial.println("Error en el sistema de carga.");
        Serial.println("No es un error tan grave, vamos a dejar que el watchdog reinicie el ESP32.");
        while (true);
      }
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
