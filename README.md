# 🚦 Semáforo IoT con Botón SOS de Emergencias

> Sistema de semáforo inteligente ESP32 v7 con botón de emergencias multifunción por número de pulsaciones, detección vehicular IR, dashboard WebSocket en tiempo real y receptor ESP8266 con alerta sonora y página web.

![Version](https://img.shields.io/badge/Version-v7_SOS-red)
![ESP32](https://img.shields.io/badge/ESP32-Arduino-blue?logo=arduino)
![ESP8266](https://img.shields.io/badge/ESP8266-Receptor-orange?logo=arduino)
![WebSocket](https://img.shields.io/badge/WebSocket-Real--Time-green)
![Bluetooth](https://img.shields.io/badge/Bluetooth-Classic-blueviolet)
![WiFi](https://img.shields.io/badge/WiFi-AP%20Mode-teal)

---

## 📋 Tabla de Contenidos

- [¿Qué hace este proyecto?](#-qué-hace-este-proyecto)
- [Arquitectura del Sistema](#-arquitectura-del-sistema)
- [Hardware Requerido](#-hardware-requerido)
- [Diagrama de Conexiones](#-diagrama-de-conexiones)
- [Botón SOS — Funcionamiento](#-botón-sos--funcionamiento-principal)
- [Protocolo UDP entre ESP32 y ESP8266](#-protocolo-udp-entre-esp32-y-esp8266)
- [Dashboard Web ESP32](#-dashboard-web-esp32)
- [Dashboard Web ESP8266](#-dashboard-web-esp8266)
- [Configuración por Bluetooth](#-configuración-por-bluetooth)
- [Máquinas de Estado](#-máquinas-de-estado)
- [Instalación](#-instalación)
- [Flujo de Demo](#-flujo-de-demo)
- [Comandos de referencia rápida](#-comandos-de-referencia-rápida)
- [Solución de Problemas](#-solución-de-problemas)
- [Estructura del Repositorio](#-estructura-del-repositorio)

---

## 🧠 ¿Qué hace este proyecto?

Este sistema convierte un semáforo IoT en una **estación de respuesta a emergencias** integrada al poste. El usuario puede presionar un botón físico en el poste un número determinado de veces para reportar distintos tipos de emergencia — sin aplicaciones, sin teléfono, sin internet.

### Dos dispositivos coordinados

**ESP32 — Semáforo principal (`semaforo_hackathon_7_sos.ino`)**

- Controla el ciclo semafórico no bloqueante (Rojo → Verde → Amarillo)
- Lee el sensor IR para detectar vehículos en el paso peatonal
- Lee el **botón SOS** (GPIO 4) y clasifica la emergencia según el número de pulsaciones
- Muestra mensajes en pantalla OLED con scroll horizontal
- Sirve un **dashboard web** en tiempo real vía WebSocket (puerto 81)
- Genera su propia red WiFi AP (`Semaforo_Demo`)
- Envía alertas UDP al ESP8266 receptor para ambos tipos de evento (IR y SOS)
- Permite configuración completa por **Bluetooth Classic**

**ESP8266 — Receptor de alertas (`ESP8266_WebServer_v2.ino`)**

- Se conecta a la red `Semaforo_Demo` generada por el ESP32
- Recibe paquetes UDP en el puerto 4210 y los clasifica (ALARMA IR o SOS)
- Activa el buzzer con patrones diferenciados según el tipo de alerta
- Sirve una **página web de estado** que se auto-actualiza cada 3 segundos

---

## 🏗️ Arquitectura del Sistema

```
┌───────────────────────────────────────────────────────────────┐
│              Red WiFi AP "Semaforo_Demo" (ESP32)              │
│                                                               │
│  ┌─────────────────────────────────────┐                      │
│  │              ESP32                  │                      │
│  │                                     │  UDP :4210           │
│  │  • Semáforo R/V/A (millis)          │ ─────────────────►  │
│  │  • Sensor IR GPIO 23                │                      │
│  │  • Botón SOS GPIO 4 (INPUT_PULLUP)  │  ┌───────────────┐  │
│  │  • OLED SSD1306 (I2C)              │  │   ESP8266     │  │
│  │  • Buzzer GPIO 15 (LEDC)           │  │               │  │
│  │  • Bluetooth "ESP32_SEMAFORO"      │  │  Buzzer D5    │  │
│  │  • WebServer HTTP :80              │  │  WebServer:80 │  │
│  │  • WebSocket    :81                │  │  UDP :4210    │  │
│  │  • NVS flash config                │  └───────────────┘  │
│  └─────────────────────────────────────┘                      │
│                                                               │
│  📱 Celular/PC ─── WiFi ─── http://192.168.4.1    (ESP32)    │
│  📱 Celular/PC ─── WiFi ─── http://192.168.4.2    (ESP8266)  │
│  📱 Celular/PC ─── WebSocket ws://192.168.4.1:81  (en vivo)  │
└───────────────────────────────────────────────────────────────┘

  📲 Configuración → Bluetooth Classic → "ESP32_SEMAFORO"
```

---

## 🔧 Hardware Requerido

### ESP32 — Semáforo principal

| Componente | Cantidad | GPIO | Notas |
|---|---|---|---|
| ESP32 DevKit v1 | 1 | — | Con soporte BT Classic |
| LED Rojo | 1 | 18 | Con resistencia 220Ω |
| LED Verde | 1 | 2 | Con resistencia 220Ω |
| LED Amarillo | 1 | 5 | Con resistencia 220Ω |
| Buzzer activo/pasivo | 1 | 15 | Control LEDC PWM |
| Sensor IR MH-B | 1 | 23 | LOW = objeto detectado |
| **Botón SOS** | **1** | **4** | **Pull-up interno (INPUT_PULLUP)** |
| Display OLED SSD1306 128x64 | 1 | 21/22 | I2C SDA/SCL |
| Resistencias 220Ω | 3 | — | Una por LED |

### ESP8266 — Receptor de alertas

| Componente | Cantidad | Pin | Notas |
|---|---|---|---|
| ESP8266 NodeMCU | 1 | — | |
| Buzzer | 1 | D5 | Indicador de alerta |

---

## 🔌 Diagrama de Conexiones

### ESP32

```
ESP32 GPIO  │  Componente              │  Notas
────────────┼──────────────────────────┼──────────────────────────
GPIO  2     │  LED Verde               │  → 220Ω → GND
GPIO  5     │  LED Amarillo            │  → 220Ω → GND
GPIO 18     │  LED Rojo                │  → 220Ω → GND
GPIO 15     │  Buzzer (+)              │  GND del buzzer a GND
GPIO 23     │  Sensor IR OUT           │  LOW = objeto detectado
GPIO  4     │  Botón SOS (un extremo)  │  Otro extremo a GND
            │                          │  (Pull-up interno activo)
GPIO 21     │  OLED SDA                │  I2C Data
GPIO 22     │  OLED SCL                │  I2C Clock
3.3V        │  VCC OLED / Sensor IR    │
GND         │  GND común               │
```

> **Botón SOS:** Al presionar, el pin cae de HIGH a LOW (flanco descendente). El firmware usa `INPUT_PULLUP`, por lo que **no necesita resistencia externa**.

### ESP8266

```
ESP8266 Pin │  Componente    │  Notas
────────────┼────────────────┼──────────────────
D5          │  Buzzer (+)    │  GND del buzzer a GND
GND         │  GND           │
3.3V / VCC  │  Alimentación  │
```

---

## 🆘 Botón SOS — Funcionamiento Principal

Esta es la funcionalidad diferenciadora del proyecto v7. Un único botón físico en el poste permite reportar **tres tipos de emergencia** distintas según cuántas veces se presiona en una ventana de 600 ms.

### Tabla de pulsaciones

| Pulsaciones | Tipo de emergencia | Ícono | Color en dashboard |
|:-----------:|---|:-----:|:------------------:|
| **1 pulso** | ACCIDENTE | 🚗 | Naranja `#ff9800` |
| **2 pulsos** | ATRACO / ROBO | 🔫 | Rojo `#f44336` |
| **3 pulsos** | EMERGENCIA MÉDICA | 🏥 | Morado `#e040fb` |
| **4+ pulsos** | EMERGENCIA (genérica) | 🆘 | Gris |

### Parámetros de temporización

| Parámetro | Valor | Descripción |
|---|---|---|
| `SOS_DEBOUNCE_MS` | 50 ms | Antirebote del botón |
| `SOS_VENTANA_MS` | 600 ms | Tiempo de espera para contar pulsaciones adicionales |
| `SOS_COOLDOWN_MS` | 10 000 ms (10 s) | Tiempo mínimo entre activaciones para evitar falsas alarmas |

### Flujo interno al detectar pulsaciones

```
Pulso detectado (flanco HIGH→LOW)
        │
        ▼
sosPulsaciones++ → tSosVentana = millis()
        │
        ▼  (transcurren SOS_VENTANA_MS sin más pulsos)
        │
        ▼
activarEmergencia(sosPulsaciones)
   ├── OLED: "!!! SOS ACCIDENTE !!!" (scroll)
   ├── Buzzer SOS: 6 beeps cortos @ 3000 Hz (no bloqueante)
   ├── Bluetooth: "!!! SOS #N DETECTADO !!!" + tipo + uptime
   ├── UDP al ESP8266: "SOS:N:TIPO" (ej. "SOS:3:ATRACO")
   ├── WebSocket broadcast → tarjeta naranja en dashboard
   └── Cooldown: 10 s sin aceptar nuevas pulsaciones
```

### Indicadores visuales durante SOS activo

- **OLED:** muestra `"!!! SOS ACCIDENTE !!!"` (o el tipo correspondiente) en scroll horizontal
- **Esquina superior derecha OLED:** texto `SOS` parpadeando mientras el cooldown está activo
- **Contadores en la parte inferior del OLED:** `SOS:N` e `INV:N` siempre visibles
- **Dashboard web:** tarjeta naranja con borde resaltado + flash de pantalla más intenso que el IR

### Patrón del buzzer SOS (diferente al IR)

```
Frecuencia : 3000 Hz
Patrón     : 6 beeps cortos  (SOS_BEEP_NUM = 6)
ON  por    : 100 ms  (SOS_BEEP_ON)
OFF por    :  80 ms  (SOS_BEEP_OFF)
Total      : ~1.1 s de alerta sonora no bloqueante
```

---

## 📡 Protocolo UDP entre ESP32 y ESP8266

El ESP32 envía paquetes UDP al ESP8266 (`192.168.4.2`, puerto `4210`) para dos tipos de evento:

### Formato de mensajes

```
Evento IR:    "ALARMA:N:VEHICULO_EN_PASO"
              │      │  └── descripción fija
              │      └───── número total de invasiones
              └──────────── prefijo identificador

Evento SOS:   "SOS:N:TIPO"
              │   │  └── ACCIDENTE | ATRACO | EMERGENCIA MEDICA
              │   └───── número total de emergencias
              └────────── prefijo identificador
```

### Ejemplos reales

```
ALARMA:5:VEHICULO_EN_PASO       → invasión IR número 5
SOS:1:ACCIDENTE                 → primera emergencia, tipo accidente
SOS:2:ATRACO                    → segunda emergencia, tipo atraco
SOS:3:EMERGENCIA MEDICA         → tercera emergencia, emergencia médica
```

### Reacción del ESP8266 por tipo

| Mensaje recibido | Respuesta del ESP8266 |
|---|---|
| `ALARMA:...` | 1 beep largo (500 ms) en D5 |
| `SOS:...:ACCIDENTE` | 3 dobles-beeps rápidos (patrón sirena corta) |
| `SOS:...:ATRACO` | 3 dobles-beeps rápidos |
| `SOS:...:EMERGENCIA MEDICA` | 3 dobles-beeps rápidos |

---

## 🌐 Dashboard Web ESP32

**URL:** `http://192.168.4.1` (conectado a la red `Semaforo_Demo`)

Actualización en tiempo real vía **WebSocket** (`ws://192.168.4.1:81`). Si se desconecta, intenta reconectar automáticamente cada 2 segundos.

### Tarjetas del dashboard

**Estado actual**
- Semáforo animado con efecto glow (rojo, amarillo, verde)
- Badge con el estado textual (ROJO / VERDE / VERDE PARPADEANDO / AMARILLO)
- Uptime del ESP32 formateado (segundos → minutos → horas)

**Estadísticas de invasiones IR**
- Total registradas en la sesión
- Invasiones en la última hora
- Tiempo transcurrido desde la última detección
- Estado actual del sensor IR (🚗 DETECTANDO / ✅ LIBRE)

**🆘 Emergencias SOS** *(tarjeta con borde naranja)*
- Contador grande de total de emergencias
- Última emergencia recibida con su tipo
- Guía visual del botón: 1 pulso → 🚗 Accidente, 2 pulsos → 🔫 Atraco, 3 pulsos → 🏥 Médica

**Invasiones por hora**
- Gráfica de barras de 24 columnas (0h–23h)
- Altura proporcional al número de invasiones en cada hora

**Historial invasiones IR**
- Lista cronológica inversa de las últimas 20 detecciones con uptime

**Historial emergencias SOS**
- Lista cronológica inversa de las últimas 20 emergencias con ícono, tipo, número y uptime

### Efectos visuales de alerta

| Evento | Efecto en pantalla |
|---|---|
| Invasión IR | Flash rojo semitransparente (0.5 s) + borde rojo |
| Emergencia SOS | Flash naranja más intenso y duradero (1.2 s) + borde naranja grueso |

### JSON enviado por WebSocket (broadcast cada 1 segundo)

```json
{
  "estado":           "ROJO",
  "totalEventos":     5,
  "eventosHora":      2,
  "sensorIR":         false,
  "uptime":           342,
  "msUltimaInvasion": 15000,
  "nuevaAlarma":      false,
  "totalSos":         1,
  "nuevaSos":         false,
  "tipoSos":          "ACCIDENTE",
  "conteoHora":       [0,0,0,0,0,0,0,0,2,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
}
```

---

## 🌐 Dashboard Web ESP8266

**URL:** `http://192.168.4.2` (conectado a la red `Semaforo_Demo`)

Página HTML simple que se **auto-actualiza cada 3 segundos** (`<meta http-equiv='refresh' content='3'>`).

### Contenido

**Tarjeta IR** (borde rojo izquierdo)
- Última alerta UDP recibida con el string completo del mensaje

**Tarjeta SOS** (borde del color del tipo de emergencia)
- Tipo de emergencia con ícono y color: naranja (accidente), rojo (atraco), morado (médica)
- Número del evento (`Evento #N`)
- Tiempo transcurrido desde el último SOS (en segundos)

---

## 🔵 Configuración por Bluetooth

**Nombre del dispositivo:** `ESP32_SEMAFORO`  
**App recomendada:** Serial Bluetooth Terminal (Android/iOS)

### Menú jerárquico completo

```
Comando: menu  (pausa el ciclo mientras se configura)
════════════════════════════════
MENÚ PRINCIPAL
════════════════════════════════
 1. Tiempos semáforo
    ├── 1. Rojo   (1–60 s)
    ├── 2. Verde  (1–60 s)
    └── 3. Amarillo (1–30 s)

 2. Mensajes OLED
    ├── 1. Mensaje Rojo    (max 60 chars)
    ├── 2. Mensaje Verde   (max 60 chars)
    └── 3. Mensaje Amarillo (max 60 chars)

 3. Buzzer (cambio de fase)
    ├── 1. Número de beeps (1–10)
    ├── 2. Duración ON     (50–1000 ms)
    ├── 3. Pausa           (50–1000 ms)
    ├── 4. Frecuencia      (200–4000 Hz)
    └── t. Probar

 4. Velocidad scroll OLED   (1=lento … 10=rápido)

 5. Alarma IR vehículo
    ├── 1. Beeps de alarma  (1–20)
    ├── 2. Tono             (200–4000 Hz)
    ├── 3. Cooldown         (1–30 s)
    └── t. Probar alarma

 6. Ver config actual
 7. Restaurar defaults
 0. Salir del menú (reanuda el ciclo)
```

Todos los parámetros se guardan en **NVS (flash)** y sobreviven reinicios.

### Comandos directos (sin entrar al menú)

| Comando | Acción |
|---|---|
| `menu` | Abrir menú de configuración (pausa el ciclo) |
| `estado` | Ver configuración actual + estadísticas completas |
| `stats` | Invasiones IR, emergencias SOS, uptime |
| `pausa` | Pausar el ciclo semafórico |
| `resume` / `reanudar` | Reanudar el ciclo |
| `rojo` / `verde` / `amarillo` | Forzar fase (requiere `pausa` primero) |
| `buzzer` | Probar el buzzer de cambio de fase |
| `sensor` | Leer estado actual del sensor IR |
| `sos` | Probar botón SOS manualmente (dispara tipo ACCIDENTE) |
| `stats` | Estadísticas de invasiones y emergencias |
| `btoff` | Desactivar Bluetooth (libera recursos WiFi) |
| `ayuda` / `help` | Ver todos los comandos disponibles |

---

## 🔄 Máquinas de Estado

### Ciclo del Semáforo

```
        tRojo (configurable)          tVerde (configurable)
  ┌──────────────────────┐      ┌──────────────────────────┐
  │                      │      │                          │
  ▼                      │      ▼                          │
🔴 ROJO ────────────────►  🟢 VERDE ──────────────────► 🟢✨ VERDE_PARPADEANDO
  ▲     IR activo aquí                                      │
  │                                              tAmarillo  │
  │                                                         ▼
  └──────────────────────────── 🟡 AMARILLO ◄───────────────┘
                                 (buzzer NB al entrar en rojo)

  VERDE_PARPADEANDO: 12 parpadeos cada 500 ms antes de pasar a AMARILLO
```

### Máquina de Estados — Alarma IR

```
                [Solo activa en fase ROJA]
                         │
                  IR detecta (LOW)
                         │
                         ▼
          ┌─────────── INACTIVA ◄───────────────────────┐
          │                                             │
          │ objeto detectado                  cooldown completado
          ▼                                             │
       SONANDO ─────────────────────────────────► ENFRIANDO
    (N beeps @ freq Hz,                         (cfg.alarmaCooldown ms)
     OLED "!!! VEHICULO EN PASO !!!",
     WebSocket nuevaAlarma=true,
     UDP "ALARMA:N:VEHICULO_EN_PASO",
     Bluetooth alerta)

  * Cambio de fase → alarma cancelada inmediatamente
  * ALARMA_BEEP_ON_MS  = 150 ms
  * ALARMA_BEEP_OFF_MS = 100 ms
  * Cooldown default = 5 000 ms
```

### Máquina de Estados — Botón SOS

```
  ┌──────────────────────────────────────────────────────┐
  │                    COOLDOWN_ACTIVO                   │
  │           (10 s sin leer el botón)                   │
  └───────────────────────┬──────────────────────────────┘
                          │ 10 s transcurridos
                          ▼
               ┌─────── IDLE ──────────────────────┐
               │                                   │
               │ flanco HIGH→LOW (pulso)           │ (sin pulsos)
               ▼                                   │
          sosPulsaciones++                         │
          tSosVentana = millis()                   │
          sosEsperandoMas = true                   │
               │                                   │
               │ SOS_VENTANA_MS (600 ms) expirados │
               ▼                                   │
        activarEmergencia(N) ──────────────────────┘
        sosCooldownActivo = true
```

---

## ⚙️ Instalación

### Firmware ESP32

**1. Librerías requeridas (Arduino IDE → Library Manager)**

```
Adafruit GFX Library
Adafruit SSD1306
WebSocketsServer  (Markus Sattler)
ArduinoJson
```
*(Wire, BluetoothSerial, Preferences, WiFi, WebServer, WiFiUdp, HTTPClient → incluidas en el core ESP32)*

**2. Partición (OBLIGATORIO)**

```
Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)
```

**3. Configuración de red** (verificar las constantes al inicio del `.ino`)

```cpp
#define WIFI_SSID  "Semaforo_Demo"
#define WIFI_PASS  "12345678"
#define ESP8266_IP "192.168.4.2"
#define UDP_PORT    4210
```

**4. Cargar y verificar en Serial Monitor (115200 baud)**

```
AP IP: 192.168.4.1
Bluetooth listo -> busca ESP32_SEMAFORO
WebServer y WebSocket iniciados
ESP32 SEMAFORO v7 SOS LISTO
```

### Firmware ESP8266

**1. Librerías** (incluidas en el core ESP8266)

```
ESP8266WiFi
WiFiUdp
```

**2. Configuración de red** (verificar al inicio del `.ino`)

```cpp
const char* ssid     = "Semaforo_Demo";
const char* password = "";            // red abierta
unsigned int localPort = 4210;
int buzzerPin = D5;
```

**3. Cargar y verificar en Serial Monitor (115200 baud)**

```
Conectando a Semaforo_Demo...
Conectado! IP: 192.168.4.2
Servidor UDP y HTTP listos
```

---

## 🎬 Flujo de Demo

```
1. Cargar firmware en ESP32 y ESP8266
        │
        ▼
2. Encender el ESP32
   → Genera la red WiFi "Semaforo_Demo"
   → OLED muestra "ESP32 SEMAFORO v7 SOS LISTO"
   → Bluetooth "ESP32_SEMAFORO" disponible
        │
        ▼
3. Encender el ESP8266
   → Se conecta automáticamente a "Semaforo_Demo"
   → Queda escuchando en UDP :4210
        │
        ▼
4. (Opcional) Conectar app BT para configurar tiempos/mensajes
   → Enviar "menu" → ajustar → "0" para reanudar
        │
        ▼
5. Conectar celular a la red "Semaforo_Demo"
   → Abrir http://192.168.4.1   → Dashboard ESP32 (en vivo)
   → Abrir http://192.168.4.2   → Página estado ESP8266
        │
        ▼
6. DEMO Sensor IR:
   → Acercar objeto al sensor durante fase ROJA
   → Buzzer IR activo, OLED "!!! VEHICULO EN PASO !!!"
   → Flash rojo en dashboard, contador +1
   → ESP8266: 1 beep largo + página actualizada
        │
        ▼
7. DEMO Botón SOS — ACCIDENTE (1 pulso):
   → Presionar el botón 1 vez
   → OLED: "!!! SOS ACCIDENTE !!!"
   → Buzzer: 6 beeps cortos urgentes @ 3000 Hz
   → Dashboard: tarjeta SOS naranja 🚗, flash intenso
   → ESP8266: patrón doble-beep × 3 + página actualizada
        │
        ▼
8. DEMO Botón SOS — ATRACO (2 pulsos):
   → Presionar el botón 2 veces rápido (< 600 ms entre pulsos)
   → Igual que arriba pero tipo "ATRACO" 🔫 en rojo
        │
        ▼
9. DEMO Botón SOS — EMERGENCIA MÉDICA (3 pulsos):
   → Presionar el botón 3 veces rápido
   → Tipo "EMERGENCIA MEDICA" 🏥 en morado
        │
        ▼
10. Mostrar tab "Historial emergencias SOS" en el dashboard
    → Muestra todos los eventos con tipo, número y uptime
```

---

## 📖 Comandos de Referencia Rápida

### Por Bluetooth (Serial Bluetooth Terminal)

```
ayuda          → ver todos los comandos
menu           → configurar tiempos, mensajes, buzzer, alarma
estado         → config actual + estadísticas
stats          → invasiones IR + emergencias SOS + uptime
sensor         → leer sensor IR ahora
sos            → probar SOS manual (tipo ACCIDENTE, 1 pulso)
pausa          → pausar ciclo
resume         → reanudar ciclo
rojo           → forzar rojo (requiere pausa)
verde          → forzar verde (requiere pausa)
amarillo       → forzar amarillo (requiere pausa)
buzzer         → probar buzzer de fase
btoff          → apagar Bluetooth
```

### Endpoints HTTP (REST)

| URL | Descripción |
|---|---|
| `http://192.168.4.1/` | Dashboard completo ESP32 |
| `http://192.168.4.1/api/stats` | JSON con el estado actual |
| `http://192.168.4.2/` | Página de estado ESP8266 |

---

## 🛠️ Solución de Problemas

| Síntoma | Causa probable | Solución |
|---|---|---|
| `Sketch too large` al compilar | Partición por defecto | Cambiar a **Huge APP (3MB)** |
| Botón SOS no responde | GPIO sin pull-up externo | Verificar `pinMode(BTN_SOS, INPUT_PULLUP)` — no necesita resistencia |
| Botón genera múltiples emergencias con 1 pulso | `SOS_DEBOUNCE_MS` muy bajo o botón de mala calidad | Aumentar `SOS_DEBOUNCE_MS` de 50 a 100 ms |
| Dashboard se desconecta del WebSocket | Red inestable | El cliente reconecta automáticamente cada 2 segundos |
| ESP8266 no recibe UDP | IP destino incorrecta en el ESP32 | Verificar `#define ESP8266_IP` — debe ser `192.168.4.2` |
| Buzzer SOS y alarma IR suenan a la vez | Ambos eventos simultáneos | El firmware los gestiona en máquinas de estado independientes |
| OLED no enciende | Dirección I2C incorrecta | Usar I2C scanner — confirmar `0x3C` o `0x3D` |
| WebServer lento / dashboard no carga | WebServer en Core 1 bloqueado | Verificar que `xTaskCreatePinnedToCore` está asignando la tarea web al **Core 0** |
| Bluetooth no aparece en el celular | BT desactivado por `btoff` | Reiniciar el ESP32 para reactivar BT |
| Sensor IR siempre activo | Módulo apunta a superficie cercana | Ajustar potenciómetro del módulo MH-B |
| `ledcAttach` no reconocido | Core 3.x vs 2.x | Verificar versión del ESP32 Arduino Core |

---

## 📁 Estructura del Repositorio

```
semaforo-sos-v7/
│
├── firmware_esp32/
│   └── semaforo_hackathon_7_sos.ino      # Semáforo principal + botón SOS
│
├── firmware_esp8266/
│   └── ESP8266_WebServer_v2.ino          # Receptor UDP + web de estado
│
└── README.md
```

---

## 🛰️ Stack Tecnológico

| Capa | Tecnología |
|---|---|
| Microcontrolador principal | ESP32 (Arduino framework) |
| Microcontrolador receptor | ESP8266 (Arduino framework) |
| Conectividad | WiFi AP (ESP32 genera la red), WiFi STA (ESP8266 se conecta) |
| Dashboard en vivo | WebSocket (`WebSocketsServer.h`, puerto 81) + HTML/CSS/JS |
| Alertas entre dispositivos | UDP broadcast (`WiFiUdp.h`, puerto 4210) |
| Configuración | Bluetooth Classic + NVS flash (`Preferences.h`) |
| Pantalla | OLED SSD1306 I2C (Adafruit GFX) con scroll horizontal custom |
| Audio | LEDC PWM (`ledcAttach` / `ledcWriteTone` / `ledcDetach`) |
| Tarea web | FreeRTOS `xTaskCreatePinnedToCore` → Core 0 |
| HTML embebido | `PROGMEM` + chunked streaming (evita agotamiento de RAM) |

---

## 🤝 Créditos

Desarrollado por **Jose** — Hackathon Edition v7.  
Basado en el ecosistema Arduino para ESP32/ESP8266 y librerías de la comunidad open source.

---

> *"Un pulso para reportar un accidente. Dos para un atraco. Tres para una emergencia médica. El poste que responde."*
