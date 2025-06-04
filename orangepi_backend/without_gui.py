#!/usr/bin/env python3
"""
ESP32 Cargador Solar - Monitor de Terminal
Comunicación serial robusta con el ESP32 para monitoreo y configuración
"""

import serial
import json
import time
import sys
import threading
from datetime import datetime
import argparse
from dataclasses import dataclass
from typing import Optional, Dict, Any
import logging

# Configuración de logging
logging.basicConfig(
    level=logging.WARNING,  # Solo mostrar warnings y errores por defecto
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

@dataclass
class ESP32Config:
    """Configuración para la comunicación con ESP32"""
    port: str = '/dev/ttyS5'
    baudrate: int = 9600
    timeout: float = 3.0
    max_retries: int = 3
    command_delay: float = 0.5  # Delay entre comandos para no saturar
    heartbeat_interval: float = 30.0  # Intervalo para heartbeat

class ESP32Monitor:
    """Monitor robusto para comunicación con ESP32"""
    
    def __init__(self, config: ESP32Config):
        self.config = config
        self.serial_conn: Optional[serial.Serial] = None
        self.connected = False
        self.last_data: Dict[str, Any] = {}
        self.last_command_time = 0
        self.lock = threading.Lock()
        self.running = False
        
    def connect(self) -> bool:
        """Establecer conexión serial con reintentos"""
        for attempt in range(self.config.max_retries):
            try:
                if self.serial_conn and self.serial_conn.is_open:
                    self.serial_conn.close()
                    
                self.serial_conn = serial.Serial(
                    port=self.config.port,
                    baudrate=self.config.baudrate,
                    timeout=self.config.timeout,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE
                )
                
                # Limpiar buffers
                self.serial_conn.reset_input_buffer()
                self.serial_conn.reset_output_buffer()
                
                # Enviar comando de prueba
                if self._send_command_raw("CMD:GET_DATA", expect_response=False):
                    self.connected = True
                    logger.info(f"✅ Conectado al ESP32 en {self.config.port}")
                    return True
                    
            except Exception as e:
                logger.warning(f"❌ Intento {attempt + 1}: Error conectando: {e}")
                time.sleep(1)
                
        self.connected = False
        return False
    
    def disconnect(self):
        """Cerrar conexión serial"""
        self.running = False
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
        self.connected = False
        logger.info("🔌 Desconectado del ESP32")
    
    def _enforce_rate_limit(self):
        """Asegurar que no se saturen las comunicaciones"""
        with self.lock:
            elapsed = time.time() - self.last_command_time
            if elapsed < self.config.command_delay:
                time.sleep(self.config.command_delay - elapsed)
            self.last_command_time = time.time()
    
    def _send_command_raw(self, command: str, expect_response: bool = True) -> Optional[str]:
        """Enviar comando raw al ESP32 con manejo de errores"""
        if not self.serial_conn or not self.serial_conn.is_open:
            return None
            
        try:
            # Limpiar buffer de entrada antes de enviar
            self.serial_conn.reset_input_buffer()
            
            # Enviar comando
            cmd_bytes = f"{command}\n".encode('utf-8')
            self.serial_conn.write(cmd_bytes)
            self.serial_conn.flush()
            
            if not expect_response:
                return "OK"
            
            # Leer respuesta con timeout
            response = ""
            start_time = time.time()
            
            while time.time() - start_time < self.config.timeout:
                if self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        response = line
                        break
                time.sleep(0.01)  # Small delay to prevent busy waiting
            
            return response if response else None
            
        except Exception as e:
            logger.error(f"❌ Error enviando comando '{command}': {e}")
            return None
    
    def send_command(self, command: str) -> Optional[str]:
        """Enviar comando con rate limiting y reintentos"""
        if not self.connected:
            if not self.connect():
                return None
        
        self._enforce_rate_limit()
        
        for attempt in range(self.config.max_retries):
            response = self._send_command_raw(command)
            if response:
                return response
            
            if attempt < self.config.max_retries - 1:
                logger.warning(f"⚠️ Reintentando comando (intento {attempt + 2})")
                time.sleep(0.5)
        
        logger.error(f"❌ Falló comando después de {self.config.max_retries} intentos: {command}")
        return None
    
    def get_data(self) -> Optional[Dict[str, Any]]:
        """Obtener datos del ESP32"""
        response = self.send_command("CMD:GET_DATA")
        if not response or not response.startswith("DATA:"):
            return None
            
        try:
            json_str = response[5:]  # Remover "DATA:"
            data = json.loads(json_str)
            self.last_data = data
            return data
        except json.JSONDecodeError as e:
            logger.error(f"❌ Error decodificando JSON: {e}")
            logger.error(f"Respuesta recibida: {response[:200]}...")
            return None
    
    def set_parameter(self, parameter: str, value: Any) -> bool:
        """Establecer un parámetro en el ESP32"""
        # Convertir booleanos a 'true'/'false' para compatibilidad con el firmware
        if isinstance(value, bool):
            value_str = str(value).lower()
        else:
            value_str = str(value)

        command = f"CMD:SET_{parameter}:{value_str}"
        response = self.send_command(command)
        
        if response and response.startswith("OK:"):
            logger.info(f"✅ {parameter} = {value}")
            return True
        else:
            logger.error(f"❌ Error configurando {parameter}: {response}")
            return False

def clear_screen():
    """Limpiar pantalla"""
    import os
    os.system('cls' if os.name == 'nt' else 'clear')

def format_value(key: str, value: Any) -> str:
    """Formatear valores para display"""
    if value is None:
        return "N/A"
    
    # Valores booleanos
    if isinstance(value, bool):
        return "SÍ" if value else "NO"
    
    # Voltajes
    if 'voltage' in key.lower() or 'V' in str(value):
        if isinstance(value, (int, float)):
            return f"{value:.2f} V"
    
    # Corrientes en mA, ignorando la variable PWM
    if 'current' in key.lower() and 'pwm' not in key.lower() and 'mA' not in str(value):
        if isinstance(value, (int, float)):
            return f"{value:.0f} mA"

    # Valor de PWM (0-255) mostrado como porcentaje
    if 'pwm' in key.lower():
        if isinstance(value, (int, float)):
            percent = (float(value) / 255.0) * 100.0
            return f"{value:.0f} ({percent:.0f}%)"
    
    # Temperaturas
    if 'temperature' in key.lower():
        if isinstance(value, (int, float)):
            return f"{value:.1f} °C"
    
    # Porcentajes
    if 'percentage' in key.lower() or 'soc' in key.lower():
        if isinstance(value, (int, float)):
            return f"{value:.1f}%"
    
    # Tiempo en horas
    if 'hours' in key.lower():
        if isinstance(value, (int, float)):
            return f"{value:.1f} h"
    
    # Amper-hora
    if 'ah' in key.lower():
        if isinstance(value, (int, float)):
            return f"{value:.2f} Ah"
    
    return str(value)

def display_dashboard(data: Dict[str, Any]):
    """Mostrar dashboard en terminal"""
    clear_screen()
    
    print("=" * 80)
    print("🔋 ESP32 CARGADOR SOLAR - MONITOR DE TERMINAL")
    print("=" * 80)
    
    # Timestamp
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"⏰ Última actualización: {now}")
    print()
    
    # Estado de conexión
    connected = data.get('connected', False)
    conn_status = "🟢 CONECTADO" if connected else "🔴 DESCONECTADO"
    print(f"📡 Estado: {conn_status}")
    
    if data.get('firmware_version'):
        print(f"🔧 Firmware: {data['firmware_version']}")
    print()
    
    # Métricas principales
    print("📊 MÉTRICAS PRINCIPALES")
    print("-" * 40)
    
    main_metrics = [
        ("🔋 Voltaje Batería", data.get('voltageBatterySensor2')),
        ("☀️ Voltaje Panel", data.get('voltagePanel')),
        ("⬇️ Corriente Panel", data.get('panelToBatteryCurrent')),
        ("⬆️ Corriente Carga", data.get('batteryToLoadCurrent')),
        ("↔️ Corriente Neta", data.get('netCurrent')),
        ("🌡️ Temperatura", data.get('temperature')),
        ("PWM Actual", data.get('currentPWM')),
    ]
    
    for label, value in main_metrics:
        formatted_value = format_value(label, value)
        print(f"{label:<20}: {formatted_value}")
    
    print()
    
    # Estado de carga
    print("⚡ ESTADO DE CARGA")
    print("-" * 40)
    
    charge_state = data.get('chargeState', 'UNKNOWN')
    state_colors = {
        'BULK_CHARGE': '🟠 BULK',
        'ABSORPTION_CHARGE': '🔵 ABSORCIÓN', 
        'FLOAT_CHARGE': '🟢 FLOTACIÓN',
        'ERROR': '🔴 ERROR'
    }
    
    display_state = state_colors.get(charge_state, f"⚪ {charge_state}")
    print(f"Estado Actual        : {display_state}")
    
    soc = data.get('estimatedSOC', 0)
    soc_bar = "█" * int(soc / 5) + "░" * (20 - int(soc / 5))
    print(f"SOC Estimado         : {soc:.1f}% [{soc_bar}]")
    
    print(f"Ah Acumulados        : {format_value('ah', data.get('accumulatedAh'))}")
    print(f"Horas Absorción Calc.: {format_value('hours', data.get('calculatedAbsorptionHours'))}")
    print()
    
    # Configuración de voltajes
    print("⚙️ CONFIGURACIÓN DE VOLTAJES")
    print("-" * 40)
    voltage_config = [
        ("Bulk", data.get('bulkVoltage')),
        ("Absorción", data.get('absorptionVoltage')),
        ("Flotación", data.get('floatVoltage')),
    ]
    
    for label, value in voltage_config:
        print(f"{label:<12}: {format_value('voltage', value)}")

    print()

    # Parámetros de batería
    print("🔋 PARÁMETROS DE BATERÍA")
    print("-" * 40)
    battery_params = [
        ("Capacidad", format_value('ah', data.get('batteryCapacity'))),
        ("Umbral Corriente", format_value('percentage', data.get('thresholdPercentage'))),
        ("Corriente Máx", format_value('current', data.get('maxAllowedCurrent'))),
        ("Factor Div", data.get('factorDivider')),
        ("Horas Bulk Máx", format_value('hours', data.get('maxBulkHours'))),
        ("Horas Abs Máx", format_value('hours', data.get('maxAbsorptionHours'))),
    ]

    for label, value in battery_params:
        print(f"{label:<16}: {value}")

    print()
    
    # Estado del sistema
    print("🔧 ESTADO DEL SISTEMA")
    print("-" * 40)
    
    system_status = [
        ("Carga Activada", data.get('loadControlState')),
        ("Apagado Temporal", data.get('temporaryLoadOff')),
        ("LED Solar", data.get('ledSolarState')),
        ("Tipo Batería", "Litio" if data.get('isLithium') else "GEL"),
        ("Fuente Energía", "DC" if data.get('useFuenteDC') else "Solar"),
    ]
    
    for label, value in system_status:
        formatted_value = format_value(label, value)
        print(f"{label:<16}: {formatted_value}")
    
    # Tiempo restante si hay apagado temporal
    remaining_time = data.get('loadOffRemainingSeconds', 0)
    if remaining_time > 0:
        hours = remaining_time // 3600
        minutes = (remaining_time % 3600) // 60
        seconds = remaining_time % 60
        print(f"Tiempo Restante     : {hours:02d}:{minutes:02d}:{seconds:02d}")
    
    print()
    
    # Nota personalizada
    nota = data.get('notaPersonalizada', '')
    if nota:
        print("📝 NOTA DEL SISTEMA")
        print("-" * 40)
        print(f"{nota}")
        print()
    
    print("=" * 80)
    print("Comandos: [c]onfigurar | [q] salir | [r] actualizar")
    print("=" * 80)

def configuration_menu(monitor: ESP32Monitor):
    """Menú de configuración interactivo"""
    clear_screen()
    print("⚙️ CONFIGURACIÓN DE BATERÍA")
    print("=" * 50)
    
    # Obtener datos actuales
    data = monitor.get_data()
    if not data:
        print("❌ Error obteniendo datos actuales del ESP32")
        input("Presiona Enter para continuar...")
        return
    
    # Parámetros configurables
    params = {
        'batteryCapacity': {
            'name': 'Capacidad de Batería (Ah)',
            'current': data.get('batteryCapacity', 50.0),
            'min': 1.0,
            'max': 1000.0,
            'step': 0.1
        },
        'thresholdPercentage': {
            'name': 'Umbral de Corriente (%)',
            'current': data.get('thresholdPercentage', 1.0),
            'min': 0.1,
            'max': 5.0,
            'step': 0.1
        },
        'maxAllowedCurrent': {
            'name': 'Corriente Máxima (mA)',
            'current': data.get('maxAllowedCurrent', 6000),
            'min': 1000,
            'max': 15000,
            'step': 100
        },
        'bulkVoltage': {
            'name': 'Voltaje BULK (V)',
            'current': data.get('bulkVoltage', 14.4),
            'min': 12.0,
            'max': 15.0,
            'step': 0.1
        },
        'absorptionVoltage': {
            'name': 'Voltaje ABSORCIÓN (V)',
            'current': data.get('absorptionVoltage', 14.4),
            'min': 12.0,
            'max': 15.0,
            'step': 0.1
        },
        'floatVoltage': {
            'name': 'Voltaje FLOTACIÓN (V)',
            'current': data.get('floatVoltage', 13.6),
            'min': 12.0,
            'max': 15.0,
            'step': 0.1
        },
        'isLithium': {
            'name': 'Tipo de Batería',
            'current': data.get('isLithium', False),
            'options': ['GEL (false)', 'Litio (true)']
        },
        'useFuenteDC': {
            'name': 'Fuente de Energía',
            'current': data.get('useFuenteDC', False),
            'options': ['Solar (false)', 'DC (true)']
        }
    }
    
    # Mostrar valores actuales
    print("Valores actuales:")
    print("-" * 30)
    for i, (key, config) in enumerate(params.items(), 1):
        current = config['current']
        if 'options' in config:
            display_val = config['options'][1 if current else 0]
        else:
            display_val = f"{current}"
        print(f"{i}. {config['name']}: {display_val}")
    
    print("\n0. Volver al dashboard")
    print("-" * 30)
    
    try:
        choice = input("\nSelecciona parámetro a modificar (0-8): ").strip()
        
        if choice == '0':
            return
        
        param_index = int(choice) - 1
        param_keys = list(params.keys())
        
        if 0 <= param_index < len(param_keys):
            param_key = param_keys[param_index]
            param_config = params[param_key]
            
            print(f"\nConfigurando: {param_config['name']}")
            print(f"Valor actual: {param_config['current']}")
            
            if 'options' in param_config:
                # Parámetro booleano
                print("Opciones:")
                for i, option in enumerate(param_config['options']):
                    print(f"  {i}: {option}")
                
                new_choice = input("Selecciona opción (0/1): ").strip()
                if new_choice in ['0', '1']:
                    new_value = new_choice == '1'
                else:
                    print("❌ Opción inválida")
                    input("Presiona Enter para continuar...")
                    return
            else:
                # Parámetro numérico
                min_val = param_config['min']
                max_val = param_config['max']
                print(f"Rango válido: {min_val} - {max_val}")
                
                new_val_str = input(f"Nuevo valor: ").strip()
                try:
                    new_value = float(new_val_str)
                    if new_value < min_val or new_value > max_val:
                        print(f"❌ Valor fuera de rango ({min_val}-{max_val})")
                        input("Presiona Enter para continuar...")
                        return
                except ValueError:
                    print("❌ Valor numérico inválido")
                    input("Presiona Enter para continuar...")
                    return
            
            # Confirmar cambio
            print(f"\n¿Confirmar cambio de {param_config['name']}?")
            print(f"De: {param_config['current']}")
            print(f"A:  {new_value}")
            
            confirm = input("Confirmar (s/N): ").strip().lower()
            if confirm in ['s', 'si', 'y', 'yes']:
                if monitor.set_parameter(param_key, new_value):
                    print("✅ Parámetro actualizado correctamente")
                    time.sleep(1)  # Dar tiempo al ESP32 para procesar
                else:
                    print("❌ Error actualizando parámetro")
                    input("Presiona Enter para continuar...")
            else:
                print("Cambio cancelado")
        else:
            print("❌ Opción inválida")
            input("Presiona Enter para continuar...")
            
    except (ValueError, KeyboardInterrupt):
        print("\nOperación cancelada")
        input("Presiona Enter para continuar...")

def main():
    """Función principal"""
    parser = argparse.ArgumentParser(description='Monitor de terminal para ESP32 Cargador Solar')
    parser.add_argument('--port', default='/dev/ttyS5', help='Puerto serial (default: /dev/ttyS5)')
    parser.add_argument('--baudrate', type=int, default=9600, help='Velocidad serial (default: 9600)')
    parser.add_argument('--debug', action='store_true', help='Habilitar logging debug')
    parser.add_argument('--interval', type=float, default=3.0, help='Intervalo de actualización (default: 3.0s)')
    
    args = parser.parse_args()
    
    if args.debug:
        logging.getLogger().setLevel(logging.DEBUG)
    
    # Configuración
    config = ESP32Config(
        port=args.port,
        baudrate=args.baudrate,
        command_delay=0.5  # Medio segundo entre comandos
    )
    
    # Crear monitor
    monitor = ESP32Monitor(config)
    
    print("🚀 Iniciando ESP32 Terminal Monitor...")
    print(f"📡 Puerto: {config.port} @ {config.baudrate} bps")
    
    # Intentar conectar
    if not monitor.connect():
        print(f"❌ No se pudo conectar al ESP32 en {config.port}")
        print("Verifica:")
        print("- Que el ESP32 esté conectado y encendido")
        print("- Que el puerto serial sea correcto")
        print("- Que tengas permisos para acceder al puerto")
        sys.exit(1)
    
    try:
        while True:
            # Obtener datos
            data = monitor.get_data()
            if data:
                display_dashboard(data)
            else:
                clear_screen()
                print("❌ Error obteniendo datos del ESP32")
                print("Reintentando conexión...")
                if not monitor.connect():
                    print("❌ Reconexión fallida")
                    time.sleep(2)
                    continue
            
            # Esperar entrada del usuario o timeout
            import select
            ready, _, _ = select.select([sys.stdin], [], [], args.interval)
            
            if ready:
                user_input = sys.stdin.readline().strip().lower()
                
                if user_input in ['q', 'quit', 'exit']:
                    break
                elif user_input in ['c', 'config', 'configurar']:
                    configuration_menu(monitor)
                elif user_input in ['r', 'refresh', 'actualizar']:
                    continue  # Forzar actualización inmediata
                # Cualquier otra tecla también actualiza
            
    except KeyboardInterrupt:
        print("\n\n⏹️ Interrumpido por usuario")
    except Exception as e:
        print(f"\n❌ Error inesperado: {e}")
    finally:
        monitor.disconnect()
        print("👋 ¡Hasta luego!")

if __name__ == "__main__":
    main()