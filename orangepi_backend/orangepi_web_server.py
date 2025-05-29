#!/usr/bin/env python3
"""
Orange Pi Web Server - Control de Cargador Solar/Litio
Servidor web completo con interfaz mejorada y control serial del ESP32
"""

import os
import json
import time
import serial
import logging
import threading
from datetime import datetime, timedelta
from flask import Flask, render_template, request, jsonify, send_file
from flask_cors import CORS
import io

# Configuración de logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('logs/cargador.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# Crear directorio de logs si no existe
os.makedirs('logs', exist_ok=True)

# Configuración
SERIAL_PORT = '/dev/ttyS5'
SERIAL_BAUDRATE = 9600
SERIAL_TIMEOUT = 2
UPDATE_INTERVAL = 2  # segundos

app = Flask(__name__)
CORS(app)
app.config['SECRET_KEY'] = 'your-secret-key-change-this'

class ChargerController:
    """Controlador principal para la comunicación con el cargador ESP32"""
    
    def __init__(self, port=SERIAL_PORT, baudrate=SERIAL_BAUDRATE):
        self.port = port
        self.baudrate = baudrate
        self.serial_connection = None
        self.data = {
            # Valores por defecto
            'connected': False,
            'panelToBatteryCurrent': 0,
            'batteryToLoadCurrent': 0,
            'voltagePanel': 0,
            'voltageBatterySensor2': 0,
            'currentPWM': 0,
            'temperature': 0,
            'chargeState': 'UNKNOWN',
            'bulkVoltage': 14.4,
            'absorptionVoltage': 14.4,
            'floatVoltage': 13.6,
            'LVD': 12.0,
            'LVR': 12.5,
            'batteryCapacity': 50.0,
            'thresholdPercentage': 1.0,
            'maxAllowedCurrent': 6000,
            'isLithium': False,
            'maxBatteryVoltageAllowed': 15.0,
            'absorptionCurrentThreshold_mA': 500,
            'currentLimitIntoFloatStage': 100,
            'calculatedAbsorptionHours': 0,
            'accumulatedAh': 0,
            'estimatedSOC': 0,
            'netCurrent': 0,
            'factorDivider': 5,
            'useFuenteDC': False,
            'fuenteDC_Amps': 0,
            'maxBulkHours': 0,
            'maxAbsorptionHours': 1.0,
            'chargedBatteryRestVoltage': 12.88,
            'reEnterBulkVoltage': 12.6,
            'pwmFrequency': 40000,
            'tempThreshold': 55,
            'temporaryLoadOff': False,
            'loadOffRemainingSeconds': 0,
            'loadOffDuration': 0,
            'loadControlState': False,
            'ledSolarState': False,
            'notaPersonalizada': '',
            'firmware_version': 'Unknown',
            'uptime': 0,
            'last_update': datetime.now().isoformat(),
            'last_communication': None,
            'communication_errors': 0
        }
        self.lock = threading.Lock()
        self.running = False
        self.update_thread = None
        
    def connect(self):
        """Establecer conexión serial con el ESP32"""
        try:
            if self.serial_connection and self.serial_connection.is_open:
                self.serial_connection.close()
                
            self.serial_connection = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                timeout=SERIAL_TIMEOUT,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE
            )
            
            # Limpiar buffer
            self.serial_connection.reset_input_buffer()
            self.serial_connection.reset_output_buffer()
            
            logger.info(f"✅ Conectado al puerto serial {self.port}")
            self.data['connected'] = True
            self.data['communication_errors'] = 0
            return True
            
        except Exception as e:
            logger.error(f"❌ Error al conectar: {e}")
            self.data['connected'] = False
            return False
    
    def disconnect(self):
        """Cerrar conexión serial"""
        if self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.close()
            logger.info("🔌 Desconectado del puerto serial")
        self.data['connected'] = False
    
    def send_command(self, command):
        """Enviar comando al ESP32"""
        if not self.serial_connection or not self.serial_connection.is_open:
            if not self.connect():
                return None
                
        try:
            # Agregar prefijo CMD: si no lo tiene
            if not command.startswith("CMD:"):
                command = f"CMD:{command}"
                
            self.serial_connection.write(f"{command}\n".encode())
            logger.info(f"📤 Comando enviado: {command}")
            
            # Esperar respuesta
            response = self.serial_connection.readline().decode().strip()
            
            if response:
                logger.info(f"📥 Respuesta: {response}")
                self.data['last_communication'] = datetime.now().isoformat()
                self.data['communication_errors'] = 0
                return response
            else:
                logger.warning("⚠️ Sin respuesta del ESP32")
                return None
                
        except Exception as e:
            logger.error(f"❌ Error al enviar comando: {e}")
            self.data['communication_errors'] += 1
            if self.data['communication_errors'] > 5:
                self.disconnect()
            return None
    
    def request_data(self):
        """Solicitar datos actuales del ESP32"""
        response = self.send_command("GET_DATA")
        if response and response.startswith("DATA:"):
            try:
                json_str = response[5:]  # Remover "DATA:"
                new_data = json.loads(json_str)
                
                with self.lock:
                    # Actualizar solo los campos recibidos
                    for key, value in new_data.items():
                        self.data[key] = value
                    
                    # Actualizar metadatos
                    self.data['last_update'] = datetime.now().isoformat()
                    self.data['connected'] = True
                    
                logger.debug(f"📊 Datos actualizados: {len(new_data)} campos")
                return True
                
            except json.JSONDecodeError as e:
                logger.error(f"❌ Error al decodificar JSON: {e}")
                logger.error(f"Respuesta recibida: {response}")
                return False
        return False
    
    def set_parameter(self, parameter, value):
        """Establecer un parámetro en el ESP32"""
        command = f"SET_{parameter}:{value}"
        response = self.send_command(command)
        
        if response and response.startswith("OK:"):
            logger.info(f"✅ Parámetro {parameter} actualizado a {value}")
            # Actualizar valor local
            with self.lock:
                self.data[parameter] = value
            return True
        else:
            logger.error(f"❌ Error al actualizar {parameter}: {response}")
            return False
    
    def toggle_load(self, seconds):
        """Apagar la carga temporalmente por X segundos"""
        if seconds < 1 or seconds > 43200:
            return False, "Tiempo fuera de rango (1-43200 segundos)"
            
        response = self.send_command(f"TOGGLE_LOAD:{seconds}")
        
        if response and response.startswith("OK:"):
            # Actualizar estado local
            with self.lock:
                self.data['temporaryLoadOff'] = True
                self.data['loadOffDuration'] = seconds
                self.data['loadOffRemainingSeconds'] = seconds
            return True, response[3:]
        else:
            return False, response or "Sin respuesta"
    
    def cancel_temporary_off(self):
        """Cancelar el apagado temporal de la carga"""
        response = self.send_command("CANCEL_TEMP_OFF")
        
        if response and response.startswith("OK:"):
            # Actualizar estado local
            with self.lock:
                self.data['temporaryLoadOff'] = False
                self.data['loadOffRemainingSeconds'] = 0
                self.data['loadOffDuration'] = 0
            return True, response[3:]
        else:
            return False, response or "Sin respuesta"
    
    def start_update_loop(self):
        """Iniciar el bucle de actualización de datos"""
        if self.running:
            return
            
        self.running = True
        self.update_thread = threading.Thread(target=self._update_loop)
        self.update_thread.daemon = True
        self.update_thread.start()
        logger.info("🔄 Bucle de actualización iniciado")
    
    def stop_update_loop(self):
        """Detener el bucle de actualización"""
        self.running = False
        if self.update_thread:
            self.update_thread.join()
        logger.info("🛑 Bucle de actualización detenido")
    
    def _update_loop(self):
        """Bucle principal de actualización de datos"""
        while self.running:
            try:
                if self.request_data():
                    # Si hay apagado temporal, actualizar tiempo restante localmente
                    with self.lock:
                        if self.data.get('temporaryLoadOff', False):
                            # El ESP32 ya envía el tiempo restante calculado
                            pass
                else:
                    logger.warning("⚠️ No se pudieron obtener datos del ESP32")
                    
            except Exception as e:
                logger.error(f"❌ Error en bucle de actualización: {e}")
                
            time.sleep(UPDATE_INTERVAL)
    
    def get_data(self):
        """Obtener copia de los datos actuales"""
        with self.lock:
            return self.data.copy()
    
    def export_config(self):
        """Exportar configuración actual a JSON"""
        config = {
            'batteryCapacity': self.data.get('batteryCapacity', 50),
            'thresholdPercentage': self.data.get('thresholdPercentage', 1),
            'maxAllowedCurrent': self.data.get('maxAllowedCurrent', 6000),
            'bulkVoltage': self.data.get('bulkVoltage', 14.4),
            'absorptionVoltage': self.data.get('absorptionVoltage', 14.4),
            'floatVoltage': self.data.get('floatVoltage', 13.6),
            'isLithium': self.data.get('isLithium', False),
            'useFuenteDC': self.data.get('useFuenteDC', False),
            'fuenteDC_Amps': self.data.get('fuenteDC_Amps', 0),
            'LVD': self.data.get('LVD', 12.0),
            'LVR': self.data.get('LVR', 12.5),
            'maxAbsorptionHours': self.data.get('maxAbsorptionHours', 1.0),
            'factorDivider': self.data.get('factorDivider', 5)
        }
        return config
    
    def import_config(self, config):
        """Importar configuración desde JSON"""
        success_count = 0
        errors = []
        
        for param, value in config.items():
            if self.set_parameter(param, value):
                success_count += 1
            else:
                errors.append(f"Error al configurar {param}")
                
        return success_count, errors

# Instancia global del controlador
controller = ChargerController()

# ========== RUTAS FLASK ==========

@app.route('/')
def index():
    """Página principal"""
    return render_template('index.html')

@app.route('/api/data')
def get_data():
    """Obtener datos actuales del sistema"""
    data = controller.get_data()
    
    # Calcular valores adicionales
    data['power_panel'] = round(data['voltagePanel'] * data['panelToBatteryCurrent'] / 1000, 2)
    data['power_battery'] = round(data['voltageBatterySensor2'] * data['batteryToLoadCurrent'] / 1000, 2)
    
    # Estado de conexión
    if data.get('last_communication'):
        last_comm = datetime.fromisoformat(data['last_communication'])
        data['seconds_since_update'] = (datetime.now() - last_comm).total_seconds()
    else:
        data['seconds_since_update'] = 999
    
    return jsonify(data)

@app.route('/api/set_parameter', methods=['POST'])
def set_parameter():
    """Establecer un parámetro del sistema"""
    try:
        param = request.json.get('parameter')
        value = request.json.get('value')
        
        if not param or value is None:
            return jsonify({'success': False, 'error': 'Parámetros faltantes'}), 400
        
        # Validaciones según el parámetro
        validations = {
            'batteryCapacity': (0, 1000, float),
            'thresholdPercentage': (0.1, 5.0, float),
            'maxAllowedCurrent': (1000, 15000, float),
            'bulkVoltage': (12.0, 15.0, float),
            'absorptionVoltage': (12.0, 15.0, float),
            'floatVoltage': (12.0, 15.0, float),
            'LVD': (10.0, 13.0, float),
            'LVR': (11.0, 14.0, float),
            'fuenteDC_Amps': (0, 50, float),
            'maxAbsorptionHours': (0.1, 10.0, float),
            'factorDivider': (1, 10, int)
        }
        
        # Parámetros booleanos
        bool_params = ['isLithium', 'useFuenteDC']
        
        if param in bool_params:
            value = str(value).lower() in ['true', '1', 'yes', 'on']
            value = 'true' if value else 'false'
        elif param in validations:
            min_val, max_val, type_func = validations[param]
            try:
                value = type_func(value)
                if value < min_val or value > max_val:
                    return jsonify({
                        'success': False, 
                        'error': f'Valor fuera de rango ({min_val}-{max_val})'
                    }), 400
            except ValueError:
                return jsonify({
                    'success': False,
                    'error': 'Valor inválido'
                }), 400
        
        # Enviar al ESP32
        success = controller.set_parameter(param, value)
        
        if success:
            return jsonify({'success': True, 'message': f'{param} actualizado a {value}'})
        else:
            return jsonify({'success': False, 'error': 'Error al comunicarse con ESP32'}), 500
            
    except Exception as e:
        logger.error(f"Error en set_parameter: {e}")
        return jsonify({'success': False, 'error': str(e)}), 500

@app.route('/api/toggle_load', methods=['POST'])
def toggle_load():
    """Apagar la carga temporalmente"""
    try:
        hours = int(request.json.get('hours', 0))
        minutes = int(request.json.get('minutes', 0))
        seconds = int(request.json.get('seconds', 0))
        
        total_seconds = hours * 3600 + minutes * 60 + seconds
        
        if total_seconds < 1 or total_seconds > 43200:
            return jsonify({
                'success': False,
                'error': 'Tiempo fuera de rango (1 segundo - 12 horas)'
            }), 400
        
        success, message = controller.toggle_load(total_seconds)
        
        if success:
            return jsonify({
                'success': True,
                'message': message,
                'duration': total_seconds
            })
        else:
            return jsonify({
                'success': False,
                'error': message
            }), 500
            
    except Exception as e:
        logger.error(f"Error en toggle_load: {e}")
        return jsonify({'success': False, 'error': str(e)}), 500

@app.route('/api/cancel_temp_off', methods=['POST'])
def cancel_temp_off():
    """Cancelar el apagado temporal de la carga"""
    try:
        success, message = controller.cancel_temporary_off()
        
        if success:
            return jsonify({'success': True, 'message': message})
        else:
            return jsonify({'success': False, 'error': message}), 500
            
    except Exception as e:
        logger.error(f"Error en cancel_temp_off: {e}")
        return jsonify({'success': False, 'error': str(e)}), 500

@app.route('/api/export_config')
def export_config():
    """Exportar configuración actual"""
    try:
        config = controller.export_config()
        
        # Crear archivo en memoria
        output = io.BytesIO()
        output.write(json.dumps(config, indent=2).encode())
        output.seek(0)
        
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"cargador_config_{timestamp}.json"
        
        return send_file(
            output,
            mimetype='application/json',
            as_attachment=True,
            download_name=filename
        )
        
    except Exception as e:
        logger.error(f"Error al exportar configuración: {e}")
        return jsonify({'error': str(e)}), 500

@app.route('/api/import_config', methods=['POST'])
def import_config():
    """Importar configuración desde archivo"""
    try:
        if 'file' not in request.files:
            return jsonify({'success': False, 'error': 'No se proporcionó archivo'}), 400
            
        file = request.files['file']
        if file.filename == '':
            return jsonify({'success': False, 'error': 'Archivo vacío'}), 400
            
        # Leer y parsear JSON
        config = json.load(file)
        
        # Aplicar configuración
        success_count, errors = controller.import_config(config)
        
        return jsonify({
            'success': success_count > 0,
            'message': f'{success_count} parámetros actualizados',
            'errors': errors
        })
        
    except json.JSONDecodeError:
        return jsonify({'success': False, 'error': 'Archivo JSON inválido'}), 400
    except Exception as e:
        logger.error(f"Error al importar configuración: {e}")
        return jsonify({'success': False, 'error': str(e)}), 500

@app.route('/api/system_info')
def system_info():
    """Información del sistema"""
    try:
        import platform
        import psutil
        
        info = {
            'hostname': platform.node(),
            'platform': platform.platform(),
            'python_version': platform.python_version(),
            'cpu_percent': psutil.cpu_percent(interval=1),
            'memory_percent': psutil.virtual_memory().percent,
            'disk_percent': psutil.disk_usage('/').percent,
            'boot_time': datetime.fromtimestamp(psutil.boot_time()).isoformat(),
            'server_uptime': (datetime.now() - server_start_time).total_seconds()
        }
        
        return jsonify(info)
        
    except Exception as e:
        logger.error(f"Error al obtener información del sistema: {e}")
        return jsonify({'error': str(e)}), 500

@app.route('/api/logs')
def get_logs():
    """Obtener últimas líneas del log"""
    try:
        lines = int(request.args.get('lines', 100))
        
        with open('logs/cargador.log', 'r') as f:
            all_lines = f.readlines()
            recent_lines = all_lines[-lines:] if len(all_lines) > lines else all_lines
            
        return jsonify({
            'logs': ''.join(recent_lines),
            'total_lines': len(all_lines)
        })
        
    except Exception as e:
        logger.error(f"Error al leer logs: {e}")
        return jsonify({'error': str(e)}), 500

# ========== FUNCIONES DE INICIALIZACIÓN ==========

def check_serial_port():
    """Verificar disponibilidad del puerto serial"""
    if os.path.exists(SERIAL_PORT):
        logger.info(f"✅ Puerto serial {SERIAL_PORT} encontrado")
        return True
    else:
        logger.warning(f"⚠️ Puerto serial {SERIAL_PORT} no encontrado")
        logger.info("Puertos disponibles:")
        
        import glob
        ports = glob.glob('/dev/tty*')
        serial_ports = [p for p in ports if 'ttyS' in p or 'ttyUSB' in p or 'ttyACM' in p]
        
        for port in serial_ports:
            logger.info(f"  - {port}")
            
        return False

def initialize_controller():
    """Inicializar el controlador y establecer conexión"""
    if check_serial_port():
        if controller.connect():
            controller.start_update_loop()
            logger.info("✅ Sistema inicializado correctamente")
            return True
        else:
            logger.error("❌ No se pudo conectar al ESP32")
            return False
    else:
        logger.warning("⚠️ Ejecutando en modo demo sin conexión serial")
        return False

# ========== MAIN ==========

if __name__ == '__main__':
    server_start_time = datetime.now()
    
    logger.info("🚀 Iniciando servidor de control de cargador...")
    logger.info(f"📡 Puerto serial configurado: {SERIAL_PORT}")
    
    # Verificar dependencias
    try:
        import psutil
    except ImportError:
        logger.warning("⚠️ psutil no instalado, algunas funciones estarán limitadas")
        logger.info("Instalar con: pip install psutil")
    
    # Inicializar controlador
    initialize_controller()
    
    # Obtener IP de la Orange Pi
    import socket
    hostname = socket.gethostname()
    local_ip = socket.gethostbyname(hostname)
    
    logger.info(f"🌐 Servidor web iniciado en http://{local_ip}:5000")
    logger.info("Presiona Ctrl+C para detener el servidor")
    
    try:
        # Iniciar servidor Flask
        app.run(host='0.0.0.0', port=5000, debug=False)
    except KeyboardInterrupt:
        logger.info("\n⏹️ Deteniendo servidor...")
        controller.stop_update_loop()
        controller.disconnect()
        logger.info("✅ Servidor detenido correctamente")
