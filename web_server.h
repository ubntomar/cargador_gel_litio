#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <Adafruit_INA219.h>

extern WebServer server;
extern String notaPersonalizada;
extern bool useFuenteDC;
extern float fuenteDC_Amps;
extern float maxBulkHours;

// Estados de carga (debe coincidir con la definición en el archivo principal)
enum ChargeState {
  BULK_CHARGE,
  ABSORPTION_CHARGE,
  FLOAT_CHARGE,
  ERROR
};

// Declaración de la función getChargeStateString
extern String getChargeStateString(ChargeState state);

// Declaraciones de las variables externas
extern float batteryCapacity;
extern float thresholdPercentage;
extern float maxAllowedCurrent;
extern bool isLithium;
extern float absorptionCurrentThreshold;
extern float currentLimitIntoFloatStage;
extern int factorDivider;
extern float bulkVoltage;
extern float absorptionVoltage;
extern float floatVoltage;
extern float panelToBatteryCurrent;
extern float batteryToLoadCurrent;
extern int currentPWM;
extern const float LVD;
extern const float LVR;
extern float calculatedAbsorptionHours;
extern float accumulatedAh;
extern float temperature;
extern Adafruit_INA219 ina219_1;
extern Adafruit_INA219 ina219_2;
extern ChargeState currentState;

// Declaración de funciones
void initWebServer();
void handleWebServer();
String getHTML();
String getData();
float getSOCFromVoltage(float voltage);

#endif