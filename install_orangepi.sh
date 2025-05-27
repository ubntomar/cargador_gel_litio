#!/bin/bash
# Script de instalación para Orange Pi Zero3 - Control Cargador
# Instala dependencias y configura el sistema

set -e

echo "=================================="
echo "🍊 Orange Pi - Instalador Cargador"
echo "=================================="

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Función para mostrar mensajes
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Verificar si se ejecuta como root
if [[ $EUID -eq 0 ]]; then
   print_error "No ejecutes este script como root"
   exit 1
fi

print_status "Iniciando instalación del sistema de control de cargador..."

# Actualizar sistema
print_status "Actualizando el sistema..."
sudo apt update && sudo apt upgrade -y

# Instalar dependencias del sistema
print_status "Instalando dependencias del sistema..."
sudo apt install -y \
    python3 \
    python3-pip \
    python3-venv \
    git \
    curl \
    nano \
    htop \
    screen \
    ufw

# Verificar puerto serial
print_status "Verificando puerto serial /dev/ttyS5..."
if [ -e "/dev/ttyS5" ]; then
    print_success "Puerto /dev/ttyS5 encontrado"
    ls -la /dev/ttyS5
else
    print_warning "Puerto /dev/ttyS5 no encontrado"
    print_status "Puertos seriales disponibles:"
    ls -la /dev/tty* | grep -E "(ttyS|ttyUSB|ttyACM)" || true
fi

# Configurar permisos de usuario
print_status "Configurando permisos de usuario para puertos seriales..."
sudo usermod -a -G dialout $USER
sudo usermod -a -G tty $USER

# Crear directorio del proyecto
PROJECT_DIR="$HOME/cargador-control"
print_status "Creando directorio del proyecto: $PROJECT_DIR"
mkdir -p $PROJECT_DIR
cd $PROJECT_DIR

# Crear entorno virtual de Python
print_status "Creando entorno virtual de Python..."
python3 -m venv venv
source venv/bin/activate

# Instalar dependencias de Python
print_status "Instalando dependencias de Python..."
pip install --upgrade pip
pip install \
    flask \
    pyserial \
    gunicorn \
    requests

# Crear archivo requirements.txt
print_status "Creando requirements.txt..."
cat > requirements.txt << EOF
Flask==2.3.3
pyserial==3.5
gunicorn==21.2.0
requests==2.31.0
Jinja2==3.1.2
Werkzeug==2.3.7
EOF

# Crear directorio templates
print_status "Creando estructura de directorios..."
mkdir -p templates
mkdir -p static
mkdir -p logs

# Crear archivo de configuración
print_status "Creando archivo de configuración..."
cat > config.py << EOF
#!/usr/bin/env python3
"""
Configuración para el servidor web Orange Pi
"""

# Configuración del puerto serial
SERIAL_PORT = '/dev/ttyS5'
SERIAL_BAUDRATE = 9600
SERIAL_TIMEOUT = 2

# Configuración del servidor web
WEB_HOST = '0.0.0.0'
WEB_PORT = 5000
DEBUG = False

# Configuración de logging
LOG_FILE = 'logs/cargador.log'
LOG_LEVEL = 'INFO'

# Timeouts y intervalos
DATA_UPDATE_INTERVAL = 2  # segundos
CONNECTION_RETRY_INTERVAL = 5  # segundos
HEARTBEAT_INTERVAL = 30  # segundos

# Configuración de seguridad
SECRET_KEY = 'your-secret-key-change-this'
EOF

# Crear script de inicio/parada
print_status "Creando scripts de control..."
cat > start_server.sh << 'EOF'
#!/bin/bash
# Script para iniciar el servidor de control de cargador

PROJECT_DIR="$HOME/cargador-control"
cd $PROJECT_DIR

# Activar entorno virtual
source venv/bin/activate

# Verificar puerto serial
if [ ! -e "/dev/ttyS5" ]; then
    echo "❌ Puerto /dev/ttyS5 no disponible"
    echo "Puertos disponibles:"
    ls -la /dev/tty* | grep -E "(ttyS|ttyUSB|ttyACM)" || echo "No se encontraron puertos seriales"
    exit 1
fi

# Crear logs si no existe
mkdir -p logs

echo "🚀 Iniciando servidor de control de cargador..."
echo "📡 Puerto serial: /dev/ttyS5"
echo "🌐 Servidor web: http://$(hostname -I | awk '{print $1}'):5000"
echo ""
echo "Presiona Ctrl+C para detener el servidor"
echo "=================================="

# Iniciar servidor
python3 orangepi_web_server.py
EOF

cat > stop_server.sh << 'EOF'
#!/bin/bash
# Script para detener el servidor de control de cargador

echo "🛑 Deteniendo servidor de control de cargador..."

# Buscar y matar procesos del servidor
pkill -f "orangepi_web_server.py" && echo "✅ Servidor Python detenido"
pkill -f "gunicorn.*orangepi" && echo "✅ Servidor Gunicorn detenido"

echo "✅ Servidor detenido completamente"
EOF

# Hacer ejecutables los scripts
chmod +x start_server.sh
chmod +x stop_server.sh

# Crear servicio systemd
print_status "Creando servicio systemd..."
sudo tee /etc/systemd/system/cargador-control.service > /dev/null << EOF
[Unit]
Description=Orange Pi Cargador Control Server
After=network.target

[Service]
Type=simple
User=$USER
WorkingDirectory=$PROJECT_DIR
Environment=PATH=$PROJECT_DIR/venv/bin
ExecStart=$PROJECT_DIR/venv/bin/python $