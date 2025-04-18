#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <WebServer.h>
#include <Adafruit_INA219.h>
#include "config.h"

extern WebServer server;
extern String notaPersonalizada;
extern bool useFuenteDC;
extern float fuenteDC_Amps;
extern float maxBulkHours;

// Declarar estas variables como externas (definidas en el archivo principal)
extern unsigned long loadOffStartTime;
extern unsigned long loadOffDuration;
extern bool temporaryLoadOff;

void checkLoadOffTimer();

// Declaración de la función getChargeStateString
extern String getChargeStateString(ChargeState state);

// Declaraciones de las variables externas
extern float batteryCapacity;
extern float thresholdPercentage;
extern float maxAllowedCurrent;
extern bool isLithium;
extern float absorptionCurrentThreshold_mA;
extern float currentLimitIntoFloatStage;
extern int factorDivider;
extern float bulkVoltage;
extern float absorptionVoltage;
extern float floatVoltage;
extern float panelToBatteryCurrent;
extern float batteryToLoadCurrent;
extern int currentPWM;
extern float calculatedAbsorptionHours;
extern float accumulatedAh;
extern float temperature;
extern Adafruit_INA219 ina219_1;
extern Adafruit_INA219 ina219_2;
extern ChargeState currentState;

extern unsigned long bulkStartTime;

// Declaración de funciones
void initWebServer();
void handleWebServer();
String getHTML();
String getData();
float getSOCFromVoltage(float voltage);

#endif