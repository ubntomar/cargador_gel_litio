# Cargador Gel Litio

## Description
This project is an Arduino-based charger for gel and lithium batteries.

## Features
- Supports gel and lithium battery charging.
- Includes safety mechanisms to prevent overcharging.
- Implements specific charging logic based on the battery type.

## Installation
1. Clone the repository.
2. Upload the code to your Arduino board using the Arduino IDE.

## Usage
Connect the charger to the battery and power it on. The system will automatically detect the battery type and start charging using the appropriate logic:
- **Gel Batteries**: The charger applies a constant voltage with a limited current to ensure safe charging.
- **Lithium Batteries**: The charger uses a constant current/constant voltage (CC/CV) method to optimize charging efficiency and safety.

## License
This project is licensed under the MIT License. See the LICENSE file for details.

