#ifndef CONFIG_H
#define CONFIG_H

// Definición de pines I2C
#define SDA_PIN 8
#define SCL_PIN 9

// Pines de control
#define LOAD_CONTROL_PIN 7
#define LED_SOLAR 3

// Parámetros de control de voltaje
#define LVD 12.0
#define LVR 12.5

// Definiciones para el sensor de temperatura
#define TEMP_PIN A3
#define SERIES_RESISTOR 10000.0
#define NOMINAL_RESISTANCE 10000.0
#define NOMINAL_TEMPERATURE 25.0
#define BETA 3984.0
#define ADC_RESOLUTION 4095.0
#define VCC 3.3
#define NUM_SAMPLES 20
#define TEMP_THRESHOLD_SHUTDOWN 90

// Estados de carga
enum ChargeState {
  BULK_CHARGE,
  ABSORPTION_CHARGE,
  FLOAT_CHARGE,
  ERROR
};

#endif // CONFIG_H