#!/usr/bin/env python3
"""
ESP32 Cargador Solar - Monitor Web Minimal
Interfaz web simple con CSS minimal y JavaScript vanilla
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
from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver
import urllib.parse
import os
import functools

# Configuración de logging
logging.basicConfig(
    level=logging.WARNING,
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
    command_delay: float = 0.5
    heartbeat_interval: float = 30.0

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
                
                self.serial_conn.reset_input_buffer()
                self.serial_conn.reset_output_buffer()
                
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
            self.serial_conn.reset_input_buffer()
            cmd_bytes = f"{command}\n".encode('utf-8')
            self.serial_conn.write(cmd_bytes)
            self.serial_conn.flush()
            
            if not expect_response:
                return "OK"
            
            response = ""
            start_time = time.time()
            
            while time.time() - start_time < self.config.timeout:
                if self.serial_conn.in_waiting > 0:
                    line = self.serial_conn.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        response = line
                        break
                time.sleep(0.01)
            
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
            json_str = response[5:]
            data = json.loads(json_str)
            data['connected'] = True
            data['last_update'] = datetime.now().isoformat()
            self.last_data = data
            return data
        except json.JSONDecodeError as e:
            logger.error(f"❌ Error decodificando JSON: {e}")
            return None
    
    def set_parameter(self, parameter: str, value: Any) -> bool:
        """Establecer un parámetro en el ESP32"""
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

    def toggle_load(self, seconds: int) -> bool:
        """Apagar la carga temporalmente"""
        if seconds < 1 or seconds > 43200:
            logger.error("❌ Tiempo fuera de rango (1-43200 segundos)")
            return False

        response = self.send_command(f"CMD:TOGGLE_LOAD:{seconds}")

        if response and response.startswith("OK:"):
            logger.info(f"✅ Apagado temporal por {seconds} segundos")
            return True
        else:
            logger.error(f"❌ Error en apagado temporal: {response}")
            return False

    def cancel_temporary_off(self) -> bool:
        """Cancelar el apagado temporal de la carga"""
        response = self.send_command("CMD:CANCEL_TEMP_OFF")

        if response and response.startswith("OK:"):
            logger.info("✅ Apagado temporal cancelado")
            return True
        else:
            logger.error(f"❌ Error cancelando apagado temporal: {response}")
            return False

class WebHandler(BaseHTTPRequestHandler):
    """Manejador HTTP para la interfaz web"""
    
    def do_GET(self):
        """Manejar peticiones GET"""
        if self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-type', 'text/html')
            self.end_headers()
            self.wfile.write(self.get_html().encode('utf-8'))
        
        elif self.path == '/api/data':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            
            data = self.server.monitor.get_data()
            if data:
                self.wfile.write(json.dumps(data).encode('utf-8'))
            else:
                error_data = {'connected': False, 'error': 'No data available'}
                self.wfile.write(json.dumps(error_data).encode('utf-8'))
        
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b'Not Found')
    
    def do_POST(self):
        """Manejar peticiones POST"""
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        
        if self.path == '/api/set_parameter':
            try:
                data = json.loads(post_data.decode('utf-8'))
                parameter = data.get('parameter')
                value = data.get('value')
                
                if parameter and value is not None:
                    success = self.server.monitor.set_parameter(parameter, value)
                    response = {'success': success}
                    if success:
                        response['message'] = f'Parámetro {parameter} actualizado'
                    else:
                        response['error'] = f'Error actualizando {parameter}'
                else:
                    response = {'success': False, 'error': 'Parámetros inválidos'}
                
                self.send_response(200)
                self.send_header('Content-type', 'application/json')
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
                self.wfile.write(json.dumps(response).encode('utf-8'))
                
            except Exception as e:
                self.send_response(400)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                error_response = {'success': False, 'error': str(e)}
                self.wfile.write(json.dumps(error_response).encode('utf-8'))
        
        elif self.path == '/api/toggle_load':
            try:
                data = json.loads(post_data.decode('utf-8'))
                hours = int(data.get('hours', 0))
                minutes = int(data.get('minutes', 0))
                seconds = int(data.get('seconds', 0))
                
                total_seconds = hours * 3600 + minutes * 60 + seconds
                
                if total_seconds > 0:
                    success = self.server.monitor.toggle_load(total_seconds)
                    response = {'success': success, 'duration': total_seconds}
                    if success:
                        response['message'] = f'Carga apagada por {total_seconds} segundos'
                    else:
                        response['error'] = 'Error apagando la carga'
                else:
                    response = {'success': False, 'error': 'Duración inválida'}
                
                self.send_response(200)
                self.send_header('Content-type', 'application/json')
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
                self.wfile.write(json.dumps(response).encode('utf-8'))
                
            except Exception as e:
                self.send_response(400)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                error_response = {'success': False, 'error': str(e)}
                self.wfile.write(json.dumps(error_response).encode('utf-8'))
        
        elif self.path == '/api/cancel_temp_off':
            try:
                success = self.server.monitor.cancel_temporary_off()
                response = {'success': success}
                if success:
                    response['message'] = 'Apagado temporal cancelado'
                else:
                    response['error'] = 'Error cancelando apagado temporal'
                
                self.send_response(200)
                self.send_header('Content-type', 'application/json')
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
                self.wfile.write(json.dumps(response).encode('utf-8'))
                
            except Exception as e:
                self.send_response(400)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                error_response = {'success': False, 'error': str(e)}
                self.wfile.write(json.dumps(error_response).encode('utf-8'))
        
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b'Not Found')
    
    def log_message(self, format, *args):
        """Silenciar logs del servidor HTTP"""
        pass
    
    def get_html(self):
        """Generar HTML de la interfaz"""
        return '''<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Cargador Solar - Monitor</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: Arial, sans-serif; background: #f5f5f5; padding: 10px; }
        .container { max-width: 1200px; margin: 0 auto; }
        .header { background: #2c3e50; color: white; padding: 15px; border-radius: 8px; margin-bottom: 20px; text-align: center; }
        .status { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
        .connected { background: #27ae60; }
        .disconnected { background: #e74c3c; }
        .tabs { display: flex; background: white; border-radius: 8px; overflow: hidden; margin-bottom: 20px; }
        .tab { flex: 1; padding: 15px; text-align: center; cursor: pointer; border: none; background: #ecf0f1; }
        .tab.active { background: #3498db; color: white; }
        .content { background: white; padding: 20px; border-radius: 8px; margin-bottom: 20px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 15px; }
        .metric { background: #f8f9fa; padding: 15px; border-radius: 6px; border-left: 4px solid #3498db; }
        .metric-label { font-size: 14px; color: #666; margin-bottom: 5px; }
        .metric-value { font-size: 24px; font-weight: bold; color: #2c3e50; }
        .metric-unit { font-size: 14px; color: #666; }
        .form-group { margin-bottom: 15px; }
        .form-group label { display: block; margin-bottom: 5px; font-weight: bold; }
        .form-group input, .form-group select { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }
        .btn { padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; }
        .btn-primary { background: #3498db; color: white; }
        .btn-success { background: #27ae60; color: white; }
        .btn-warning { background: #f39c12; color: white; }
        .btn-danger { background: #e74c3c; color: white; }
        .btn:hover { opacity: 0.9; }
        .alert { padding: 10px; border-radius: 4px; margin-bottom: 15px; }
        .alert-success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
        .alert-error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
        .countdown { text-align: center; padding: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; border-radius: 8px; }
        .countdown-timer { font-size: 36px; font-weight: bold; font-family: monospace; margin: 10px 0; }
        .state-bulk { color: #f39c12; }
        .state-absorption { color: #3498db; }
        .state-float { color: #27ae60; }
        .state-error { color: #e74c3c; }
        .hidden { display: none; }
        @media (max-width: 768px) { 
            .grid { grid-template-columns: 1fr; } 
            .tabs { flex-direction: column; }
            .countdown-timer { font-size: 28px; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1><span id="status-indicator" class="status disconnected"></span>ESP32 Cargador Solar</h1>
            <div>Última actualización: <span id="last-update">--:--:--</span></div>
        </div>

        <div class="tabs">
            <button class="tab active" onclick="showTab('dashboard')">Dashboard</button>
            <button class="tab" onclick="showTab('config')">Configuración</button>
            <button class="tab" onclick="showTab('control')">Control de Carga</button>
        </div>

        <div id="alerts"></div>

        <!-- Dashboard Tab -->
        <div id="dashboard" class="content">
            <div class="grid">
                <div class="metric">
                    <div class="metric-label">Voltaje Batería</div>
                    <div class="metric-value"><span id="battery-voltage">0.00</span><span class="metric-unit">V</span></div>
                </div>
                <div class="metric">
                    <div class="metric-label">Voltaje Panel</div>
                    <div class="metric-value"><span id="panel-voltage">0.00</span><span class="metric-unit">V</span></div>
                </div>
                <div class="metric">
                    <div class="metric-label">Corriente Panel→Batería</div>
                    <div class="metric-value"><span id="panel-current">0.00</span><span class="metric-unit">A</span></div>
                </div>
                <div class="metric">
                    <div class="metric-label">Corriente Batería→Carga</div>
                    <div class="metric-value"><span id="load-current">0.00</span><span class="metric-unit">A</span></div>
                </div>
                <div class="metric">
                    <div class="metric-label">Corriente Neta</div>
                    <div class="metric-value"><span id="net-current">0.00</span><span class="metric-unit">A</span></div>
                </div>
                <div class="metric">
                    <div class="metric-label">Temperatura</div>
                    <div class="metric-value"><span id="temperature">0.0</span><span class="metric-unit">°C</span></div>
                </div>
                <div class="metric">
                    <div class="metric-label">PWM Control</div>
                    <div class="metric-value"><span id="current-pwm-detailed">0</span></div>
                </div>
                <div class="metric">
                    <div class="metric-label">Estado de Carga</div>
                    <div class="metric-value"><span id="charge-state" class="state-bulk">UNKNOWN</span></div>
                </div>
            </div>
        </div>

        <!-- Configuration Tab -->
        <div id="config" class="content hidden">
            <h3>Configuración de Parámetros</h3>
            <div class="grid">
                <div class="form-group">
                    <label>Capacidad Batería (Ah):</label>
                    <input type="number" id="batteryCapacity" step="0.1" min="1" max="1000">
                </div>
                <div class="form-group">
                    <label>Umbral Corriente (%):</label>
                    <input type="number" id="thresholdPercentage" step="0.1" min="0.1" max="5">
                </div>
                <div class="form-group">
                    <label>Voltaje BULK (V):</label>
                    <input type="number" id="bulkVoltage" step="0.1" min="12" max="15">
                </div>
                <div class="form-group">
                    <label>Voltaje ABSORCIÓN (V):</label>
                    <input type="number" id="absorptionVoltage" step="0.1" min="12" max="15">
                </div>
                <div class="form-group">
                    <label>Voltaje FLOTACIÓN (V):</label>
                    <input type="number" id="floatVoltage" step="0.1" min="12" max="15">
                </div>
                <div class="form-group">
                    <label>Tipo de Batería:</label>
                    <select id="isLithium">
                        <option value="false">GEL</option>
                        <option value="true">Litio</option>
                    </select>
                </div>
            </div>
            <button class="btn btn-primary" onclick="saveConfiguration()">Guardar Configuración</button>
        </div>

        <!-- Control Tab -->
        <div id="control" class="content hidden">
            <div class="countdown">
                <h3>Control de Carga</h3>
                <div id="countdown-timer" class="countdown-timer">00:00:00</div>
                <div id="countdown-status">Carga Activa</div>
            </div>

            <div class="content">
                <h4>Apagar Carga Temporalmente</h4>
                <div class="grid">
                    <div class="form-group">
                        <label>Horas:</label>
                        <input type="number" id="loadOffHours" min="0" max="12" value="0">
                    </div>
                    <div class="form-group">
                        <label>Minutos:</label>
                        <input type="number" id="loadOffMinutes" min="0" max="59" value="0">
                    </div>
                    <div class="form-group">
                        <label>Segundos:</label>
                        <input type="number" id="loadOffSeconds" min="0" max="59" value="0">
                    </div>
                </div>
                <button class="btn btn-warning" onclick="toggleLoad()">Apagar Carga</button>
                <button class="btn btn-success" onclick="cancelTempOff()">Cancelar Apagado</button>
            </div>
        </div>
    </div>

    <script>
        let currentData = {};
        let countdownInterval = null;

        function showTab(tabName) {
            document.querySelectorAll('.content').forEach(c => c.classList.add('hidden'));
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.getElementById(tabName).classList.remove('hidden');
            event.target.classList.add('active');
        }

        function showAlert(message, type = 'success') {
            const alerts = document.getElementById('alerts');
            const alert = document.createElement('div');
            alert.className = `alert alert-${type}`;
            alert.textContent = message;
            alerts.appendChild(alert);
            setTimeout(() => alert.remove(), 5000);
        }

        function formatTime(seconds) {
            const h = Math.floor(seconds / 3600);
            const m = Math.floor((seconds % 3600) / 60);
            const s = seconds % 60;
            return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
        }

        function updateCountdown() {
            if (currentData.temporaryLoadOff && currentData.loadOffRemainingSeconds > 0) {
                document.getElementById('countdown-timer').textContent = formatTime(currentData.loadOffRemainingSeconds);
                document.getElementById('countdown-status').textContent = 'Carga Temporalmente Apagada';
            } else {
                document.getElementById('countdown-timer').textContent = '00:00:00';
                document.getElementById('countdown-status').textContent = 'Carga Activa';
            }
        }

        function updateDashboard() {
            if (!currentData.connected) {
                document.getElementById('status-indicator').className = 'status disconnected';
                return;
            }

            document.getElementById('status-indicator').className = 'status connected';
            document.getElementById('last-update').textContent = new Date().toLocaleTimeString();

            // Actualizar métricas
            document.getElementById('battery-voltage').textContent = (currentData.voltageBatterySensor2 || 0).toFixed(2);
            document.getElementById('panel-voltage').textContent = (currentData.voltagePanel || 0).toFixed(2);
            document.getElementById('panel-current').textContent = ((currentData.panelToBatteryCurrent || 0) / 1000).toFixed(2);
            document.getElementById('load-current').textContent = ((currentData.batteryToLoadCurrent || 0) / 1000).toFixed(2);
            document.getElementById('net-current').textContent = ((currentData.netCurrent || 0) / 1000).toFixed(2);
            document.getElementById('temperature').textContent = (currentData.temperature || 0).toFixed(1);

            const pwm = currentData.currentPWM || 0;
            const pwmPercent = Math.round((pwm / 255) * 100);
            document.getElementById('current-pwm-detailed').textContent = `${pwm} (${pwmPercent}%)`;

            const stateElement = document.getElementById('charge-state');
            const state = currentData.chargeState || 'UNKNOWN';
            stateElement.textContent = state;
            stateElement.className = `state-${state.toLowerCase().replace('_', '-')}`;

            updateCountdown();
        }

        async function fetchData() {
            try {
                const response = await fetch('/api/data');
                const data = await response.json();
                currentData = data;
                updateDashboard();
            } catch (error) {
                console.error('Error fetching data:', error);
                currentData = { connected: false };
                updateDashboard();
            }
        }

        async function setParameter(parameter, value) {
            try {
                const response = await fetch('/api/set_parameter', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ parameter, value })
                });
                const result = await response.json();
                if (result.success) {
                    showAlert(result.message || `${parameter} actualizado`);
                    setTimeout(fetchData, 500);
                } else {
                    showAlert(result.error || 'Error actualizando parámetro', 'error');
                }
            } catch (error) {
                showAlert(`Error: ${error.message}`, 'error');
            }
        }

        async function saveConfiguration() {
            const params = [
                ['batteryCapacity', parseFloat(document.getElementById('batteryCapacity').value)],
                ['thresholdPercentage', parseFloat(document.getElementById('thresholdPercentage').value)],
                ['bulkVoltage', parseFloat(document.getElementById('bulkVoltage').value)],
                ['absorptionVoltage', parseFloat(document.getElementById('absorptionVoltage').value)],
                ['floatVoltage', parseFloat(document.getElementById('floatVoltage').value)],
                ['isLithium', document.getElementById('isLithium').value === 'true']
            ];

            let success = true;
            for (const [param, value] of params) {
                if (isNaN(value) && typeof value !== 'boolean') {
                    showAlert(`Valor inválido para ${param}`, 'error');
                    success = false;
                    break;
                }
                
                try {
                    await setParameter(param, value);
                    await new Promise(resolve => setTimeout(resolve, 200));
                } catch (error) {
                    success = false;
                    break;
                }
            }

            if (success) {
                showAlert('✅ Configuración guardada correctamente');
            }
        }

        async function toggleLoad() {
            const hours = parseInt(document.getElementById('loadOffHours').value) || 0;
            const minutes = parseInt(document.getElementById('loadOffMinutes').value) || 0;
            const seconds = parseInt(document.getElementById('loadOffSeconds').value) || 0;

            if (hours === 0 && minutes === 0 && seconds === 0) {
                showAlert('Debe especificar al menos 1 segundo', 'error');
                return;
            }

            try {
                const response = await fetch('/api/toggle_load', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ hours, minutes, seconds })
                });
                const result = await response.json();
                
                if (result.success) {
                    showAlert(result.message || 'Carga apagada temporalmente');
                    setTimeout(fetchData, 500);
                } else {
                    showAlert(result.error || 'Error apagando la carga', 'error');
                }
            } catch (error) {
                showAlert(`Error: ${error.message}`, 'error');
            }
        }

        async function cancelTempOff() {
            try {
                const response = await fetch('/api/cancel_temp_off', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' }
                });
                const result = await response.json();
                
                if (result.success) {
                    showAlert(result.message || 'Apagado temporal cancelado');
                    setTimeout(fetchData, 500);
                } else {
                    showAlert(result.error || 'Error cancelando apagado temporal', 'error');
                }
            } catch (error) {
                showAlert(`Error: ${error.message}`, 'error');
            }
        }

        // Inicialización
        document.addEventListener('DOMContentLoaded', function() {
            fetchData();
            setInterval(fetchData, 2000);
        });
    </script>
</body>
</html>'''

class CustomHTTPServer(HTTPServer):
    """Servidor HTTP personalizado que incluye el monitor"""
    def __init__(self, server_address, RequestHandlerClass, monitor):
        self.monitor = monitor
        super().__init__(server_address, RequestHandlerClass)

def run_web_server(monitor, port=8080, host="0.0.0.0"):
    """Ejecutar servidor web"""
    try:
        # Usar servidor personalizado que incluye el monitor
        with CustomHTTPServer((host, port), WebHandler, monitor) as httpd:
            # Obtener la IP local para mostrar al usuario
            import socket
            hostname = socket.gethostname()
            local_ip = socket.gethostbyname(hostname)
            
            print(f"🌐 Servidor web iniciado y accesible desde:")
            print(f"   📍 Local: http://localhost:{port}")
            print(f"   📍 Red local: http://{local_ip}:{port}")
            print(f"   📍 Todas las interfaces: http://0.0.0.0:{port}")
            print(f"📡 Puerto ESP32: {monitor.config.port}")
            print("🔄 Presiona Ctrl+C para detener")
            
            # Mostrar IPs disponibles
            try:
                import subprocess
                result = subprocess.run(['hostname', '-I'], capture_output=True, text=True, timeout=2)
                if result.returncode == 0:
                    ips = result.stdout.strip().split()
                    print(f"🌐 IPs disponibles en este dispositivo:")
                    for ip in ips:
                        if ip.strip():
                            print(f"   📍 http://{ip.strip()}:{port}")
            except:
                pass
            
            print("=" * 60)
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n⏹️ Servidor detenido")
    except Exception as e:
        print(f"❌ Error en servidor web: {e}")

def main():
    """Función principal"""
    parser = argparse.ArgumentParser(description='ESP32 Cargador Solar - Monitor Web')
    parser.add_argument('--port', default='/dev/ttyS5', help='Puerto serial (default: /dev/ttyS5)')
    parser.add_argument('--baudrate', type=int, default=9600, help='Velocidad serial (default: 9600)')
    parser.add_argument('--web-host', default='0.0.0.0', help='Dirección del servidor web (default: 0.0.0.0)')
    parser.add_argument('--web-port', type=int, default=8080, help='Puerto del servidor web (default: 8080)')
    parser.add_argument('--debug', action='store_true', help='Habilitar logging debug')
    
    args = parser.parse_args()
    
    if args.debug:
        logging.getLogger().setLevel(logging.DEBUG)
    
    # Configuración ESP32
    config = ESP32Config(
        port=args.port,
        baudrate=args.baudrate,
        command_delay=0.5
    )
    
    # Crear monitor
    monitor = ESP32Monitor(config)
    
    print("🚀 Iniciando ESP32 Web Monitor...")
    
    # Intentar conectar al ESP32
    if not monitor.connect():
        print(f"⚠️ No se pudo conectar al ESP32 en {config.port}")
        print("El servidor web se iniciará de todos modos.")
        print("Verifica la conexión del ESP32 y recarga la página.")
    
    try:
        # Ejecutar servidor web
        run_web_server(monitor, args.web_port, args.web_host)
    except KeyboardInterrupt:
        print("\n⏹️ Interrumpido por usuario")
    except Exception as e:
        print(f"❌ Error inesperado: {e}")
    finally:
        monitor.disconnect()
        print("👋 ¡Hasta luego!")

if __name__ == "__main__":
    main()