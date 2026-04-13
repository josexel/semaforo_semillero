/*
  ================================================================
   SEMAFORO ESP32 - HACKATHON EDITION v7
   Basado en: semaforo_hackathon_6_bluetoothoff
  ----------------------------------------------------------------
   NUEVAS FUNCIONES v7:
     - Botón SOS en el poste (GPIO 4, INPUT_PULLUP)
     - Detección de tipo de emergencia por número de pulsaciones:
         1 pulso  -> ACCIDENTE
         2 pulsos -> ATRACO / ROBO
         3 pulsos -> EMERGENCIA MEDICA
     - OLED: muestra "!!! SOS ACCIDENTE !!!" etc.
     - Buzzer: patrón SOS urgente no bloqueante
     - WebSocket: tarjeta de emergencia roja en el dashboard
     - UDP al ESP8266: "SOS:N:TIPO" (ej. "SOS:3:ATRACO")
     - Bluetooth: alerta con tipo y número de evento
     - Cooldown de 10 s + debounce 50 ms para evitar falsas pulsaciones
  ----------------------------------------------------------------
   HARDWARE:
     LED_GREEN  GPIO 2  |  LED_YELLOW GPIO 5  |  LED_RED  GPIO 18
     BUZZER     GPIO 15 |  SENSOR_IR   GPIO 23
     BTN_SOS    GPIO 4  (con resistencia pull-up interna)
  ================================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "BluetoothSerial.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
// PROTOTIPOS NECESARIOS
void iniciarScroll(const char* texto);
void enviarBT(String mensaje);

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
  #error "Habilita Bluetooth en el core del ESP32"
#endif

// ================================================================
// OBJETOS GLOBALES
// ================================================================
BluetoothSerial  SerialBT;
Preferences      prefs;
WebServer        webServer(80);
WebSocketsServer webSocket(81);
WiFiUDP          udp;

// Pantalla
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Pines
#define LED_GREEN   2
#define LED_YELLOW  5
#define LED_RED     18
#define BUZZER      15
#define SENSOR_IR   23
#define BTN_SOS     4       // ← NUEVO: botón de emergencia

// WiFi AP
#define WIFI_SSID   "Semaforo_Demo"
#define WIFI_PASS   "12345678"
#define ESP8266_IP  "192.168.4.2"
#define UDP_PORT    4210

// ================================================================
// CONFIGURACION (guardada en flash)
// ================================================================
struct Config {
  unsigned long tRojo;
  unsigned long tVerde;
  unsigned long tAmarillo;
  char msgRojo[61];
  char msgVerde[61];
  char msgAmarillo[61];
  int  beeps;
  int  beepDuracion;
  int  beepPausa;
  int  beepFrecuencia;
  int  scrollVelocidad;
  int           alarmaBeeps;
  int           alarmaFrecuencia;
  unsigned long alarmaCooldown;
};

Config cfg = {
  6000, 6000, 3500,
  "DETENGASE TE ESPERAN EN CASA",
  "SIGA CON PRECAUCION",
  "PRECAUCION ZONA ESCOLAR",
  3, 200, 200, 1000,
  4,
  5, 2500, 5000
};

// ================================================================
// ESTADISTICAS DE INVASIONES
// ================================================================
#define MAX_EVENTOS 50
struct Evento {
  unsigned long timestamp;
  unsigned long duracion;
};

Evento  historial[MAX_EVENTOS];
int     totalEventos      = 0;
int     indiceHistorial   = 0;
int     inversionesHora   = 0;
unsigned long tUltimaInvasion = 0;
int conteoHora[24] = {0};

void registrarEvento() {
  unsigned long ahora = millis();
  historial[indiceHistorial].timestamp = ahora;
  historial[indiceHistorial].duracion  = 0;
  indiceHistorial = (indiceHistorial + 1) % MAX_EVENTOS;
  totalEventos++;
  tUltimaInvasion = ahora;

  int hora = (int)((ahora / 60000UL) % 24);
  conteoHora[hora]++;

  inversionesHora = 0;
  for (int i = 0; i < MAX_EVENTOS; i++) {
    if (historial[i].timestamp > 0 &&
        (ahora - historial[i].timestamp) < 3600000UL) {
      inversionesHora++;
    }
  }
}

// ================================================================
// ESTADO SEMAFORO
// ================================================================
enum Estado { ROJO, VERDE, VERDE_PARPADEANDO, AMARILLO };
Estado estadoActual   = ROJO;
bool   pausado        = false;
unsigned long tInicio = 0;

int           parpadeoCount  = 0;
bool          parpadeoEstado = false;
unsigned long tParpadeo      = 0;

// ================================================================
// ALARMA PEATONAL (IR)
// ================================================================
enum AlarmEstado { ALARMA_INACTIVA, ALARMA_SONANDO, ALARMA_ENFRIANDO };
AlarmEstado   alarmaEstado    = ALARMA_INACTIVA;
int           alarmaBeepNum   = 0;
bool          alarmaBeepOn    = false;
unsigned long tAlarma         = 0;
unsigned long tCooldownInicio = 0;

#define ALARMA_BEEP_ON_MS   150
#define ALARMA_BEEP_OFF_MS  100

const char* MSG_ALARMA = "!!! VEHICULO EN PASO !!!";
char msgAnterior[61] = "";

// ================================================================
// BUZZER NO BLOQUEANTE
// ================================================================
bool          buzzerActivo     = false;
int           buzzerBeepActual = 0;
bool          buzzerBeepOn     = false;
unsigned long tBuzzer          = 0;

void sonarBuzzerNB() {
  ledcAttach(BUZZER, cfg.beepFrecuencia, 8);
  buzzerActivo     = true;
  buzzerBeepActual = 0;
  buzzerBeepOn     = false;
  tBuzzer          = millis();
}

void tickBuzzer() {
  if (!buzzerActivo) return;
  unsigned long ahora = millis();

  if (buzzerBeepActual >= cfg.beeps) {
    ledcWriteTone(BUZZER, 0);
    ledcDetach(BUZZER);
    buzzerActivo = false;
    return;
  }

  if (!buzzerBeepOn) {
    if (ahora - tBuzzer >= (unsigned long)cfg.beepPausa) {
      ledcWriteTone(BUZZER, cfg.beepFrecuencia);
      buzzerBeepOn = true;
      tBuzzer = ahora;
    }
  } else {
    if (ahora - tBuzzer >= (unsigned long)cfg.beepDuracion) {
      ledcWriteTone(BUZZER, 0);
      buzzerBeepOn = false;
      buzzerBeepActual++;
      tBuzzer = ahora;
    }
  }
}

// ================================================================
// BOTÓN SOS — NUEVAS VARIABLES
// ================================================================
#define SOS_DEBOUNCE_MS     50      // antirebote
#define SOS_VENTANA_MS     600      // ventana para contar pulsaciones extra
#define SOS_COOLDOWN_MS  10000      // 10 s entre activaciones

int           sosPulsaciones      = 0;      // pulsaciones contadas en la ventana
bool          sosEsperandoMas     = false;  // estamos dentro de la ventana?
bool          sosCooldownActivo   = false;
unsigned long tSosUltimoPulso     = 0;
unsigned long tSosVentana         = 0;
unsigned long tSosCooldown        = 0;
bool          sosBtnAnterior      = HIGH;   // estado previo (INPUT_PULLUP -> HIGH = no presionado)

int           totalEmergencias    = 0;

// Tipos de emergencia según número de pulsaciones
const char* tipoEmergencia(int pulsos) {
  switch (pulsos) {
    case 1: return "ACCIDENTE";
    case 2: return "ATRACO";
    case 3: return "EMERGENCIA MEDICA";
    default: return "EMERGENCIA";
  }
}

// -------- Buzzer SOS urgente no bloqueante --------
// Patrón: 3 beeps cortos rápidos (diferente al buzzer normal)
bool          sosBuzzerActivo   = false;
int           sosBuzzerStep     = 0;
unsigned long tSosBuzzer        = 0;
#define SOS_BEEP_FREQ  3000
#define SOS_BEEP_ON     100
#define SOS_BEEP_OFF     80
#define SOS_BEEP_NUM      6   // 6 beeps cortos

void iniciarSosBuzzer() {
  if (sosBuzzerActivo) return;
  ledcAttach(BUZZER, SOS_BEEP_FREQ, 8);
  ledcWriteTone(BUZZER, SOS_BEEP_FREQ);
  sosBuzzerActivo = true;
  sosBuzzerStep   = 0;
  tSosBuzzer      = millis();
}

void tickSosBuzzer() {
  if (!sosBuzzerActivo) return;
  unsigned long ahora = millis();

  bool enOn = (sosBuzzerStep % 2 == 0);
  unsigned long duracion = enOn ? SOS_BEEP_ON : SOS_BEEP_OFF;

  if (ahora - tSosBuzzer >= duracion) {
    sosBuzzerStep++;
    if (sosBuzzerStep >= SOS_BEEP_NUM * 2) {
      ledcWriteTone(BUZZER, 0);
      ledcDetach(BUZZER);
      sosBuzzerActivo = false;
      return;
    }
    if (sosBuzzerStep % 2 == 0) {
      ledcWriteTone(BUZZER, SOS_BEEP_FREQ);
    } else {
      ledcWriteTone(BUZZER, 0);
    }
    tSosBuzzer = ahora;
  }
}

// -------- Activar emergencia --------
void activarEmergencia(int pulsos) {
  totalEmergencias++;
  const char* tipo = tipoEmergencia(pulsos);

  // OLED
  char msgSos[61];
  snprintf(msgSos, 61, "!!! SOS %s !!!", tipo);
  iniciarScroll(msgSos);

  // Buzzer SOS
  iniciarSosBuzzer();

  // Bluetooth
  enviarBT("=============================");
  enviarBT("!!! SOS #" + String(totalEmergencias) + " DETECTADO !!!");
  enviarBT("Tipo: " + String(tipo));
  enviarBT("Pulsos: " + String(pulsos));
  enviarBT("Uptime: " + String(millis()/1000) + "s");
  enviarBT("=============================");

  // UDP al ESP8266
  udp.beginPacket(ESP8266_IP, UDP_PORT);
  String msgUdp = "SOS:" + String(totalEmergencias) + ":" + String(tipo);
  udp.print(msgUdp);
  udp.endPacket();
  Serial.println("[UDP-SOS] " + msgUdp);

  // WebSocket al dashboard
  enviarEstadoWS(false, true, tipo);

  // Iniciar cooldown
  sosCooldownActivo = true;
  tSosCooldown      = millis();

  Serial.print("[SOS] EMERGENCIA ACTIVADA: ");
  Serial.println(tipo);
}

// -------- Leer botón SOS en el loop --------
void tickBotonSos() {
  unsigned long ahora = millis();

  // Cooldown activo: no leer el botón
  if (sosCooldownActivo) {
    if (ahora - tSosCooldown >= SOS_COOLDOWN_MS) {
      sosCooldownActivo = false;
      Serial.println("[SOS] Listo para nueva emergencia");
    }
    return;
  }

  // Leer con debounce
  bool btnActual = digitalRead(BTN_SOS);

  // Flanco descendente (HIGH->LOW = botón presionado, porque INPUT_PULLUP)
  if (sosBtnAnterior == HIGH && btnActual == LOW) {
    if (ahora - tSosUltimoPulso >= SOS_DEBOUNCE_MS) {
      tSosUltimoPulso = ahora;
      sosPulsaciones++;
      sosEsperandoMas = true;
      tSosVentana     = ahora;
      Serial.print("[SOS] Pulso detectado: ");
      Serial.println(sosPulsaciones);
    }
  }
  sosBtnAnterior = btnActual;

  // Si estamos esperando más pulsaciones y se venció la ventana → disparar
  if (sosEsperandoMas && (ahora - tSosVentana >= SOS_VENTANA_MS)) {
    int pulsos = sosPulsaciones;
    sosPulsaciones  = 0;
    sosEsperandoMas = false;
    activarEmergencia(pulsos);
  }
}

// ================================================================
// SCROLL HORIZONTAL
// ================================================================
#define SCROLL_CHAR_W  12
#define SCROLL_Y       24
#define ICONO_R        5

String        scrollTexto = "";
int           scrollX     = SCREEN_WIDTH;
int           scrollAncho = 0;
unsigned long tScroll     = 0;
int           scrollPaso  = 2;

// ================================================================
// MENU BLUETOOTH
// ================================================================
enum MenuEstado {
  MENU_NINGUNO,
  MENU_PRINCIPAL,
  MENU_TIEMPOS,
  MENU_TIEMPOS_ROJO, MENU_TIEMPOS_VERDE, MENU_TIEMPOS_AMARILLO,
  MENU_MENSAJES,
  MENU_MSG_ROJO, MENU_MSG_VERDE, MENU_MSG_AMARILLO,
  MENU_BUZZER,
  MENU_BUZZER_BEEPS, MENU_BUZZER_DURACION, MENU_BUZZER_PAUSA, MENU_BUZZER_TONO,
  MENU_SCROLL,
  MENU_ALARMA,
  MENU_ALARMA_BEEPS, MENU_ALARMA_TONO, MENU_ALARMA_COOLDOWN
};
MenuEstado menuActual = MENU_NINGUNO;

// ================================================================
// PROTOTIPOS
// ================================================================
void cargarConfig();
void guardarConfig();
void restaurarDefaults();
void iniciarScroll(const char* texto);
void dibujarTextoScroll(int xInicio, int y);
void tickScroll();
void dibujarIcono(Estado e);
void apagarTodos();
void activarRojo();
void activarVerde();
void activarAmarillo();
void sonarBuzzer();
void sonarBuzzerNB();
void tickBuzzer();
void tickAlarma();
void activarAlarma();
void enviarBT(String msg);
void procesarEntrada(String entrada);
void procesarComandoDirecto(String cmd);
void mostrarMenuPrincipal();
void mostrarMenuTiempos();
void mostrarMenuMensajes();
void mostrarMenuBuzzer();
void mostrarMenuScroll();
void mostrarMenuAlarma();
void mostrarConfigActual();
// WiFi / Dashboard
void iniciarWiFi();
void iniciarWebServer();
void enviarEstadoWS(bool nuevaAlarma = false, bool nuevaSos = false, const char* tipoSos = "");
void enviarAlertaUDP();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
String buildJsonEstado(bool nuevaAlarma, bool nuevaSos, const char* tipoSos);
// SOS
void tickBotonSos();
void tickSosBuzzer();
void activarEmergencia(int pulsos);
void iniciarSosBuzzer();
const char* tipoEmergencia(int pulsos);

// ================================================================
// HTML DEL DASHBOARD EN PROGMEM
// ================================================================
const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Semaforo IoT - Dashboard</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: 'Segoe UI', sans-serif; background: #0f1117; color: #e0e0e0; min-height: 100vh; }
  header { background: #1a1d27; padding: 16px 24px; border-bottom: 2px solid #2d3148;
           display: flex; align-items: center; gap: 12px; }
  header h1 { font-size: 1.3rem; font-weight: 700; color: #fff; }
  .badge { padding: 4px 10px; border-radius: 12px; font-size: 0.75rem; font-weight: 600; }
  .badge-ok  { background: #1a3a1a; color: #4caf50; }
  .badge-err { background: #3a1a1a; color: #f44336; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
          gap: 16px; padding: 20px; }
  .card { background: #1a1d27; border-radius: 12px; padding: 20px;
          border: 1px solid #2d3148; }
  .card h2 { font-size: 0.85rem; text-transform: uppercase; letter-spacing: 1px;
             color: #7a7f99; margin-bottom: 14px; }
  /* Semaforo visual */
  .semaforo { display: flex; flex-direction: column; align-items: center; gap: 10px;
              background: #111; border-radius: 16px; padding: 16px; width: 80px; margin: auto; }
  .luz { width: 40px; height: 40px; border-radius: 50%; border: 2px solid #333; transition: all 0.3s; }
  .luz.on-rojo    { background: #ff3d3d; box-shadow: 0 0 18px #ff3d3d88; }
  .luz.on-amarillo{ background: #ffd700; box-shadow: 0 0 18px #ffd70088; }
  .luz.on-verde   { background: #4caf50; box-shadow: 0 0 18px #4caf5088; }
  .luz.off        { background: #222; }
  /* Estadisticas */
  .stat-row { display: flex; justify-content: space-between; align-items: center;
              padding: 10px 0; border-bottom: 1px solid #2d3148; }
  .stat-row:last-child { border-bottom: none; }
  .stat-val { font-size: 1.5rem; font-weight: 700; color: #fff; }
  .stat-lbl { font-size: 0.8rem; color: #7a7f99; }
  /* Grafica barras */
  .chart { display: flex; align-items: flex-end; gap: 3px; height: 80px; margin-top: 8px; }
  .bar-wrap { flex: 1; display: flex; flex-direction: column; align-items: center; gap: 2px; }
  .bar { width: 100%; background: #e53935; border-radius: 3px 3px 0 0; min-height: 2px; transition: height 0.5s; }
  .bar-lbl { font-size: 0.55rem; color: #555; }
  /* Eventos IR */
  .evento { display: flex; justify-content: space-between; padding: 8px 10px;
            background: #12151f; border-radius: 8px; margin-bottom: 6px;
            border-left: 3px solid #e53935; font-size: 0.82rem; }
  .evento-time { color: #7a7f99; }
  /* Alerta flash IR */
  #alerta { display: none; position: fixed; top: 0; left: 0; right: 0; bottom: 0;
            background: rgba(229,57,53,0.15); border: 3px solid #e53935;
            z-index: 998; pointer-events: none; animation: flash 0.5s ease; }
  /* Flash SOS - más llamativo */
  #alerta-sos { display: none; position: fixed; top: 0; left: 0; right: 0; bottom: 0;
               background: rgba(255,165,0,0.25); border: 5px solid #ff6d00;
               z-index: 999; pointer-events: none; animation: flashSos 0.8s ease; }
  @keyframes flash    { 0%{opacity:1} 100%{opacity:0} }
  @keyframes flashSos { 0%{opacity:1} 50%{opacity:0.6} 100%{opacity:0} }
  .estado-badge { display: inline-block; padding: 6px 14px; border-radius: 20px;
                  font-weight: 700; font-size: 0.9rem; margin-top: 10px; }
  .estado-ROJO    { background:#3a1a1a; color:#ff5252; }
  .estado-VERDE   { background:#1a3a1a; color:#69f0ae; }
  .estado-AMARILLO{ background:#3a2f0a; color:#ffd740; }
  .ws-dot { width: 8px; height: 8px; border-radius: 50%; background: #555; display: inline-block; margin-right: 6px; }
  .ws-dot.conectado { background: #4caf50; }
  /* Tarjeta SOS */
  .sos-item { display: flex; justify-content: space-between; align-items: center;
              padding: 10px 12px; background: #1e1208; border-radius: 8px; margin-bottom: 8px;
              border-left: 4px solid #ff6d00; font-size: 0.85rem; }
  .sos-tipo { font-weight: 700; color: #ff9800; }
  .sos-num  { font-size: 0.75rem; color: #7a7f99; }
  .sos-time { font-size: 0.75rem; color: #7a7f99; }
  .sos-count { font-size: 2rem; font-weight: 800; color: #ff9800; }
  footer { text-align: center; padding: 16px; color: #444; font-size: 0.75rem; }
</style>
</head>
<body>
<div id="alerta"></div>
<div id="alerta-sos"></div>

<header>
  <span style="font-size:1.6rem">🚦</span>
  <h1>Semaforo IoT &mdash; Dashboard</h1>
  <span class="badge badge-ok" id="ws-status"><span class="ws-dot" id="ws-dot"></span>Conectando...</span>
</header>

<div class="grid">

  <!-- Estado semaforo -->
  <div class="card">
    <h2>Estado actual</h2>
    <div class="semaforo">
      <div class="luz" id="luz-rojo"></div>
      <div class="luz" id="luz-amarillo"></div>
      <div class="luz" id="luz-verde"></div>
    </div>
    <div style="text-align:center">
      <span class="estado-badge" id="estado-lbl">---</span>
    </div>
    <div style="margin-top:14px; font-size:0.82rem; color:#7a7f99; text-align:center" id="uptime-lbl">Uptime: --</div>
  </div>

  <!-- Estadisticas IR -->
  <div class="card">
    <h2>Estadisticas de invasiones (IR)</h2>
    <div class="stat-row">
      <span class="stat-lbl">Total registradas</span>
      <span class="stat-val" id="total-eventos">0</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Ultima hora</span>
      <span class="stat-val" id="eventos-hora">0</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Ultima deteccion</span>
      <span class="stat-val" style="font-size:1rem" id="ultima-det">---</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Sensor IR ahora</span>
      <span class="stat-val" style="font-size:1rem" id="sensor-ir">---</span>
    </div>
  </div>

  <!-- NUEVO: Panel SOS Emergencias -->
  <div class="card" style="border-color:#ff6d0066;">
    <h2 style="color:#ff9800;">🆘 Emergencias SOS</h2>
    <div class="stat-row">
      <span class="stat-lbl">Total emergencias</span>
      <span class="sos-count" id="total-sos">0</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Última emergencia</span>
      <span class="stat-val" style="font-size:1rem;color:#ff9800" id="ultima-sos">Sin eventos</span>
    </div>
    <div style="margin-top:12px;font-size:0.78rem;color:#555;line-height:1.6">
      <b style="color:#7a7f99">Cómo usar el botón:</b><br>
      1 pulso &nbsp;→ 🚗 Accidente<br>
      2 pulsos → 🔫 Atraco / Robo<br>
      3 pulsos → 🏥 Emergencia médica
    </div>
  </div>

  <!-- Grafica por hora -->
  <div class="card">
    <h2>Invasiones por hora (simulada)</h2>
    <div class="chart" id="chart"></div>
  </div>

  <!-- Historial IR -->
  <div class="card">
    <h2>Historial invasiones IR</h2>
    <div id="historial-list"><p style="color:#555;font-size:0.85rem">Sin eventos aun...</p></div>
  </div>

  <!-- Historial SOS -->
  <div class="card" style="border-color:#ff6d0044;">
    <h2 style="color:#ff9800;">Historial emergencias SOS</h2>
    <div id="sos-list"><p style="color:#555;font-size:0.85rem">Sin emergencias aun...</p></div>
  </div>

</div>

<footer>ESP32 Semaforo IoT v7 &bull; Hackathon Edition &bull; WebSocket ws://192.168.4.1:81</footer>

<script>
  let ws;
  let eventos = [];
  let eventosSos = [];
  let conteoHora = new Array(24).fill(0);

  function conectar() {
    ws = new WebSocket('ws://' + location.hostname + ':81');
    ws.onopen = () => {
      document.getElementById('ws-status').innerHTML = '<span class="ws-dot conectado"></span>En vivo';
      document.getElementById('ws-status').className = 'badge badge-ok';
    };
    ws.onclose = () => {
      document.getElementById('ws-status').innerHTML = '<span class="ws-dot"></span>Desconectado';
      document.getElementById('ws-status').className = 'badge badge-err';
      setTimeout(conectar, 2000);
    };
    ws.onmessage = (ev) => {
      try { actualizar(JSON.parse(ev.data)); } catch(e) {}
    };
  }

  function actualizar(d) {
    // Luces
    ['rojo','amarillo','verde'].forEach(c => {
      document.getElementById('luz-' + c).className = 'luz off';
    });
    const estado = d.estado || '';
    if (estado === 'ROJO')     document.getElementById('luz-rojo').className    = 'luz on-rojo';
    if (estado === 'AMARILLO' || estado === 'VERDE_PARPADEANDO')
                               document.getElementById('luz-amarillo').className= 'luz on-amarillo';
    if (estado === 'VERDE' || estado === 'VERDE_PARPADEANDO')
                               document.getElementById('luz-verde').className   = 'luz on-verde';

    const lbl = document.getElementById('estado-lbl');
    lbl.textContent = estado.replace('_',' ');
    lbl.className   = 'estado-badge estado-' + estado.split('_')[0];

    // Stats IR
    document.getElementById('total-eventos').textContent = d.totalEventos || 0;
    document.getElementById('eventos-hora').textContent  = d.eventosHora  || 0;
    document.getElementById('sensor-ir').textContent     = d.sensorIR ? '🚗 DETECTANDO' : '✅ LIBRE';
    document.getElementById('uptime-lbl').textContent    = 'Uptime: ' + formatUptime(d.uptime || 0);

    const ms = d.msUltimaInvasion || 0;
    document.getElementById('ultima-det').textContent = ms > 0 ? 'hace ' + formatUptime(ms/1000|0) : 'Sin eventos';

    // Stats SOS
    document.getElementById('total-sos').textContent = d.totalSos || 0;

    // Grafica
    if (d.conteoHora) {
      conteoHora = d.conteoHora;
      renderChart();
    }

    // Flash IR
    if (d.nuevaAlarma) {
      flashAlarma();
      eventos.unshift({ t: Date.now(), uptime: d.uptime });
      if (eventos.length > 20) eventos.pop();
      renderHistorial();
    }

    // Flash SOS
    if (d.nuevaSos && d.tipoSos) {
      flashSos();
      eventosSos.unshift({ tipo: d.tipoSos, num: d.totalSos, uptime: d.uptime });
      if (eventosSos.length > 20) eventosSos.pop();
      document.getElementById('ultima-sos').textContent = d.tipoSos;
      renderSosList();
    }
  }

  function renderChart() {
    const max = Math.max(...conteoHora, 1);
    const ch  = document.getElementById('chart');
    ch.innerHTML = '';
    conteoHora.forEach((v, i) => {
      const h = Math.max(4, (v / max) * 76);
      ch.innerHTML += `<div class="bar-wrap">
        <div class="bar" style="height:${h}px" title="${v} eventos"></div>
        <div class="bar-lbl">${i}</div>
      </div>`;
    });
  }

  function renderHistorial() {
    const el = document.getElementById('historial-list');
    if (!eventos.length) { el.innerHTML = '<p style="color:#555;font-size:0.85rem">Sin eventos aun...</p>'; return; }
    el.innerHTML = eventos.map(e =>
      `<div class="evento">
        <span>🚗 Vehiculo detectado</span>
        <span class="evento-time">+${formatUptime(e.uptime|0)}</span>
       </div>`
    ).join('');
  }

  const iconoSos = { 'ACCIDENTE': '🚗', 'ATRACO': '🔫', 'EMERGENCIA MEDICA': '🏥' };
  function renderSosList() {
    const el = document.getElementById('sos-list');
    if (!eventosSos.length) { el.innerHTML = '<p style="color:#555;font-size:0.85rem">Sin emergencias aun...</p>'; return; }
    el.innerHTML = eventosSos.map(e =>
      `<div class="sos-item">
        <span class="sos-tipo">${iconoSos[e.tipo] || '🆘'} ${e.tipo}</span>
        <span class="sos-num">#${e.num}</span>
        <span class="sos-time">+${formatUptime(e.uptime|0)}</span>
       </div>`
    ).join('');
  }

  function flashAlarma() {
    const el = document.getElementById('alerta');
    el.style.display = 'block';
    setTimeout(() => el.style.display = 'none', 600);
  }

  function flashSos() {
    const el = document.getElementById('alerta-sos');
    el.style.display = 'block';
    setTimeout(() => el.style.display = 'none', 1200);
  }

  function formatUptime(s) {
    if (s < 60)   return s + 's';
    if (s < 3600) return Math.floor(s/60) + 'm ' + (s%60) + 's';
    return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
  }

  renderChart();
  conectar();
</script>
</body>
</html>
)rawhtml";

// ================================================================
// JSON de estado para WebSocket (ampliado con SOS)
// ================================================================
String buildJsonEstado(bool nuevaAlarma = false, bool nuevaSos = false, const char* tipoSos = "") {
  StaticJsonDocument<600> doc;
  switch (estadoActual) {
    case ROJO:              doc["estado"] = "ROJO";              break;
    case VERDE:             doc["estado"] = "VERDE";             break;
    case VERDE_PARPADEANDO: doc["estado"] = "VERDE_PARPADEANDO"; break;
    case AMARILLO:          doc["estado"] = "AMARILLO";          break;
  }
  doc["totalEventos"]     = totalEventos;
  doc["eventosHora"]      = inversionesHora;
  doc["sensorIR"]         = (digitalRead(SENSOR_IR) == LOW);
  doc["uptime"]           = (int)(millis() / 1000);
  doc["msUltimaInvasion"] = tUltimaInvasion > 0 ? (int)(millis() - tUltimaInvasion) : 0;
  doc["nuevaAlarma"]      = nuevaAlarma;
  // SOS
  doc["totalSos"]         = totalEmergencias;
  doc["nuevaSos"]         = nuevaSos;
  doc["tipoSos"]          = tipoSos;

  JsonArray arr = doc.createNestedArray("conteoHora");
  for (int i = 0; i < 24; i++) arr.add(conteoHora[i]);

  String out;
  serializeJson(doc, out);
  return out;
}

// ================================================================
// WEBSOCKET
// ================================================================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    String json = buildJsonEstado(false, false, "");
    webSocket.sendTXT(num, json);
  }
}

void enviarEstadoWS(bool nuevaAlarma, bool nuevaSos, const char* tipoSos) {
  String json = buildJsonEstado(nuevaAlarma, nuevaSos, tipoSos);
  webSocket.broadcastTXT(json);
}

// ================================================================
// UDP -> ESP8266
// ================================================================
void enviarAlertaUDP() {
  udp.beginPacket(ESP8266_IP, UDP_PORT);
  String msg = "ALARMA:" + String(totalEventos) + ":VEHICULO_EN_PASO";
  udp.print(msg);
  udp.endPacket();
  Serial.println("[UDP] Alerta IR enviada");
}

// ================================================================
// WIFI AP
// ================================================================
void iniciarWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, NULL, 1, 0, 4);
  delay(500);
  esp_wifi_set_max_tx_power(40);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(ip);
  enviarBT("WiFi AP: " + String(WIFI_SSID));
  enviarBT("Dashboard: http://" + ip.toString());
}

void servirDashboard() {
  size_t len   = strlen_P(DASHBOARD_HTML);
  size_t chunk = 1024;
  webServer.setContentLength(len);
  webServer.send(200, "text/html", "");
  WiFiClient client = webServer.client();
  size_t sent = 0;
  while (sent < len) {
    size_t toSend = min(chunk, len - sent);
    char buf[1025];
    memcpy_P(buf, DASHBOARD_HTML + sent, toSend);
    buf[toSend] = '\0';
    client.print(buf);
    sent += toSend;
  }
}

void iniciarWebServer() {
  webServer.on("/", servirDashboard);
  webServer.on("/api/stats", []() {
    webServer.send(200, "application/json", buildJsonEstado(false, false, ""));
  });
  webServer.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  udp.begin(UDP_PORT);
  Serial.println("WebServer y WebSocket iniciados");
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);

  pinMode(LED_RED,    OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(BUZZER,     OUTPUT);
  pinMode(SENSOR_IR,  INPUT);
  pinMode(BTN_SOS,    INPUT_PULLUP);   // ← NUEVO
  apagarTodos();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Error: pantalla no encontrada");
    while (true);
  }
  display.clearDisplay();
  display.display();

  cargarConfig();

  SerialBT.begin("ESP32_SEMAFORO");
  Serial.println("Bluetooth listo -> busca ESP32_SEMAFORO");

  iniciarWiFi();
  iniciarWebServer();

  xTaskCreatePinnedToCore(
    tareaWeb, "tareaWeb", 8192, NULL, 1, NULL, 0
  );

  iniciarScroll("ESP32 SEMAFORO v7 SOS LISTO");
  unsigned long t0 = millis();
  while (millis() - t0 < 2000) tickScroll();

  activarRojo();
  tInicio = millis();
}

// ================================================================
// TAREA WEB (Core 0)
// ================================================================
void tareaWeb(void* param) {
  for (;;) {
    webServer.handleClient();
    webSocket.loop();
    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}

// ================================================================
// LOOP
// ================================================================
unsigned long tUltimoWS = 0;

void loop() {
  // Bluetooth
  if (SerialBT.available()) {
    String entrada = SerialBT.readStringUntil('\n');
    entrada.trim();
    if (entrada.length() > 0) procesarEntrada(entrada);
  }

  // Alarma IR
  tickAlarma();

  // Buzzer normal
  tickBuzzer();

  // Buzzer SOS  ← NUEVO
  tickSosBuzzer();

  // Botón SOS   ← NUEVO
  tickBotonSos();

  // Scroll continuo
  tickScroll();

  // WS cada 1 segundo
  if (millis() - tUltimoWS >= 1000) {
    tUltimoWS = millis();
    enviarEstadoWS(false, false, "");
  }

  // Ciclo automatico
  if (!pausado) {
    unsigned long ahora        = millis();
    unsigned long transcurrido = ahora - tInicio;

    switch (estadoActual) {

      case ROJO:
        if (alarmaEstado == ALARMA_INACTIVA) {
          if (digitalRead(SENSOR_IR) == LOW) {
            activarAlarma();
          }
        }
        if (transcurrido >= cfg.tRojo) {
          alarmaEstado = ALARMA_INACTIVA;
          activarVerde();
          tInicio = ahora;
        }
        break;

      case VERDE:
        if (transcurrido >= cfg.tVerde) {
          estadoActual   = VERDE_PARPADEANDO;
          parpadeoCount  = 0;
          parpadeoEstado = true;
          tParpadeo      = ahora;
          tInicio        = ahora;
          iniciarScroll(cfg.msgAmarillo);
          enviarBT("Verde parpadeando...");
        }
        break;

      case VERDE_PARPADEANDO:
        if (ahora - tParpadeo >= 500) {
          parpadeoEstado = !parpadeoEstado;
          digitalWrite(LED_GREEN, parpadeoEstado ? HIGH : LOW);
          tParpadeo = ahora;
          parpadeoCount++;
        }
        if (parpadeoCount >= 12) {
          digitalWrite(LED_GREEN, LOW);
          activarAmarillo();
          tInicio = ahora;
        }
        break;

      case AMARILLO:
        if (transcurrido >= cfg.tAmarillo) {
          sonarBuzzerNB();
          activarRojo();
          tInicio = ahora;
        }
        break;
    }
  }
}

// ================================================================
// ALARMA PEATONAL (IR)
// ================================================================
void activarAlarma() {
  alarmaEstado  = ALARMA_SONANDO;
  alarmaBeepNum = 0;
  alarmaBeepOn  = false;
  tAlarma       = millis();

  strncpy(msgAnterior, scrollTexto.c_str(), 60);
  iniciarScroll(MSG_ALARMA);

  registrarEvento();
  enviarEstadoWS(true, false, "");
  enviarAlertaUDP();

  enviarBT("!!! ALARMA: VEHICULO EN PASO PEATONAL !!!");
  enviarBT("Total invasiones: " + String(totalEventos));
  Serial.println("ALARMA IR ACTIVADA");

  ledcAttach(BUZZER, cfg.alarmaFrecuencia, 8);
}

void tickAlarma() {
  unsigned long ahora = millis();

  switch (alarmaEstado) {

    case ALARMA_INACTIVA:
      break;

    case ALARMA_SONANDO:
      if (alarmaBeepNum >= cfg.alarmaBeeps) {
        ledcWriteTone(BUZZER, 0);
        ledcDetach(BUZZER);
        iniciarScroll(msgAnterior);
        alarmaEstado    = ALARMA_ENFRIANDO;
        tCooldownInicio = ahora;
        break;
      }
      if (!alarmaBeepOn) {
        ledcWriteTone(BUZZER, cfg.alarmaFrecuencia);
        alarmaBeepOn = true;
        tAlarma      = ahora;
      } else {
        if (ahora - tAlarma >= ALARMA_BEEP_ON_MS) {
          ledcWriteTone(BUZZER, 0);
          alarmaBeepOn = false;
          alarmaBeepNum++;
          tAlarma = ahora;
        }
      }
      if (!alarmaBeepOn && alarmaBeepNum < cfg.alarmaBeeps) {
        if (ahora - tAlarma < ALARMA_BEEP_OFF_MS) break;
      }
      break;

    case ALARMA_ENFRIANDO:
      if (ahora - tCooldownInicio >= cfg.alarmaCooldown) {
        alarmaEstado = ALARMA_INACTIVA;
        Serial.println("Alarma IR lista para re-activarse");
      }
      break;
  }
}

// ================================================================
// SCROLL
// ================================================================
int calcularPaso(int vel)      { return map(vel, 1, 10, 1, 6);  }
int calcularIntervalo(int vel) { return map(vel, 1, 10, 40, 10); }

void iniciarScroll(const char* texto) {
  scrollTexto = String(texto);
  scrollAncho = scrollTexto.length() * SCROLL_CHAR_W;
  scrollX     = SCREEN_WIDTH;
  scrollPaso  = calcularPaso(cfg.scrollVelocidad);
}

void dibujarTextoScroll(int xInicio, int y) {
  display.setTextSize(2);
  display.setTextColor(WHITE);
  for (int i = 0; i < (int)scrollTexto.length(); i++) {
    int xChar = xInicio + i * SCROLL_CHAR_W;
    if (xChar + SCROLL_CHAR_W < 0) continue;
    if (xChar >= SCREEN_WIDTH)     break;
    display.setCursor(xChar, y);
    display.print(scrollTexto[i]);
  }
}

void tickScroll() {
  unsigned long intervalo = (unsigned long)calcularIntervalo(cfg.scrollVelocidad);
  if (millis() - tScroll < intervalo) return;
  tScroll = millis();

  display.clearDisplay();
  display.setTextWrap(false);

  dibujarIcono(estadoActual);
  dibujarTextoScroll(scrollX, SCROLL_Y);
  dibujarTextoScroll(scrollX + scrollAncho + 24, SCROLL_Y);

  if (alarmaEstado == ALARMA_SONANDO) {
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(116, 2);
    display.print("!!!");
  }

  // Indicador SOS en pantalla (esquina superior derecha)
  if (sosCooldownActivo) {
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(100, 2);
    display.print("SOS");
  }

  // Contadores inferiores
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,  56);
  display.print("SOS:" + String(totalEmergencias));
  display.setCursor(64, 56);
  display.print("INV:" + String(totalEventos));

  display.display();

  scrollX -= scrollPaso;
  if (scrollX + scrollAncho < 0) scrollX = SCREEN_WIDTH;
}

void dibujarIcono(Estado e) {
  int cy   = 9;
  int cx[] = {10, 24, 38};

  display.fillCircle(cx[0], cy, ICONO_R, (e == ROJO)    ? WHITE : BLACK);
  display.drawCircle(cx[0], cy, ICONO_R, WHITE);

  display.fillCircle(cx[1], cy, ICONO_R, (e == AMARILLO || e == VERDE_PARPADEANDO) ? WHITE : BLACK);
  display.drawCircle(cx[1], cy, ICONO_R, WHITE);

  display.fillCircle(cx[2], cy, ICONO_R, (e == VERDE || e == VERDE_PARPADEANDO) ? WHITE : BLACK);
  display.drawCircle(cx[2], cy, ICONO_R, WHITE);

  display.drawFastHLine(0, 18, SCREEN_WIDTH, WHITE);
}

// ================================================================
// SEMAFORO
// ================================================================
void apagarTodos() {
  digitalWrite(LED_RED,    LOW);
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(BUZZER,     LOW);
}

void activarRojo() {
  apagarTodos();
  estadoActual = ROJO;
  digitalWrite(LED_RED, HIGH);
  iniciarScroll(cfg.msgRojo);
  enviarBT("ROJO -> " + String(cfg.msgRojo));
  enviarEstadoWS(false, false, "");
}

void activarVerde() {
  apagarTodos();
  estadoActual = VERDE;
  digitalWrite(LED_GREEN, HIGH);
  iniciarScroll(cfg.msgVerde);
  enviarBT("VERDE -> " + String(cfg.msgVerde));
  enviarEstadoWS(false, false, "");
}

void activarAmarillo() {
  apagarTodos();
  estadoActual = AMARILLO;
  digitalWrite(LED_YELLOW, HIGH);
  iniciarScroll(cfg.msgAmarillo);
  enviarBT("AMARILLO -> " + String(cfg.msgAmarillo));
  enviarEstadoWS(false, false, "");
}

void sonarBuzzer() {
  enviarBT("Buzzer: " + String(cfg.beeps) + " beeps @ " + String(cfg.beepFrecuencia) + "Hz");
  ledcAttach(BUZZER, cfg.beepFrecuencia, 8);
  for (int i = 0; i < cfg.beeps; i++) {
    ledcWriteTone(BUZZER, cfg.beepFrecuencia);
    delay(cfg.beepDuracion);
    ledcWriteTone(BUZZER, 0);
    delay(cfg.beepPausa);
  }
  ledcDetach(BUZZER);
}

// ================================================================
// CONFIGURACION FLASH
// ================================================================
void cargarConfig() {
  prefs.begin("semaforo", false);
  cfg.tRojo            = prefs.getULong ("tRojo",       cfg.tRojo);
  cfg.tVerde           = prefs.getULong ("tVerde",      cfg.tVerde);
  cfg.tAmarillo        = prefs.getULong ("tAmarillo",   cfg.tAmarillo);
  cfg.beeps            = prefs.getInt   ("beeps",       cfg.beeps);
  cfg.beepDuracion     = prefs.getInt   ("beepDur",     cfg.beepDuracion);
  cfg.beepPausa        = prefs.getInt   ("beepPausa",   cfg.beepPausa);
  cfg.beepFrecuencia   = prefs.getInt   ("beepFreq",    cfg.beepFrecuencia);
  cfg.scrollVelocidad  = prefs.getInt   ("scrollVel",   cfg.scrollVelocidad);
  cfg.alarmaBeeps      = prefs.getInt   ("alBeeps",     cfg.alarmaBeeps);
  cfg.alarmaFrecuencia = prefs.getInt   ("alFreq",      cfg.alarmaFrecuencia);
  cfg.alarmaCooldown   = prefs.getULong ("alCooldown",  cfg.alarmaCooldown);
  String mR = prefs.getString("msgRojo",     cfg.msgRojo);
  String mV = prefs.getString("msgVerde",    cfg.msgVerde);
  String mA = prefs.getString("msgAmarillo", cfg.msgAmarillo);
  mR.toCharArray(cfg.msgRojo,     61);
  mV.toCharArray(cfg.msgVerde,    61);
  mA.toCharArray(cfg.msgAmarillo, 61);
  prefs.end();
}

void guardarConfig() {
  prefs.begin("semaforo", false);
  prefs.putULong ("tRojo",       cfg.tRojo);
  prefs.putULong ("tVerde",      cfg.tVerde);
  prefs.putULong ("tAmarillo",   cfg.tAmarillo);
  prefs.putInt   ("beeps",       cfg.beeps);
  prefs.putInt   ("beepDur",     cfg.beepDuracion);
  prefs.putInt   ("beepPausa",   cfg.beepPausa);
  prefs.putInt   ("beepFreq",    cfg.beepFrecuencia);
  prefs.putInt   ("scrollVel",   cfg.scrollVelocidad);
  prefs.putInt   ("alBeeps",     cfg.alarmaBeeps);
  prefs.putInt   ("alFreq",      cfg.alarmaFrecuencia);
  prefs.putULong ("alCooldown",  cfg.alarmaCooldown);
  prefs.putString("msgRojo",     cfg.msgRojo);
  prefs.putString("msgVerde",    cfg.msgVerde);
  prefs.putString("msgAmarillo", cfg.msgAmarillo);
  prefs.end();
  enviarBT("Guardado en flash");
}

void restaurarDefaults() {
  cfg = {
    6000, 6000, 3500,
    "DETENGASE TE ESPERAN EN CASA",
    "SIGA CON PRECAUCION",
    "PRECAUCION ZONA ESCOLAR",
    3, 200, 200, 1000, 4,
    5, 2500, 5000
  };
  guardarConfig();
  enviarBT("Valores por defecto restaurados");
}

// ================================================================
// BLUETOOTH — ENVIAR
// ================================================================
void enviarBT(String msg) {
  SerialBT.println(msg);
  Serial.println("[BT] " + msg);
}

// ================================================================
// BLUETOOTH — MENU
// ================================================================
void mostrarMenuPrincipal() {
  enviarBT("====== MENU PRINCIPAL ======");
  enviarBT(" 1. Tiempos semaforo");
  enviarBT(" 2. Mensajes OLED");
  enviarBT(" 3. Buzzer (cambio de fase)");
  enviarBT(" 4. Velocidad scroll");
  enviarBT(" 5. Alarma IR vehiculo");
  enviarBT(" 6. Ver config actual");
  enviarBT(" 7. Restaurar defaults");
  enviarBT(" 0. Salir del menu");
  enviarBT("============================");
}
void mostrarMenuTiempos()  {
  enviarBT("-- Tiempos --");
  enviarBT(" 1. Rojo   (" + String(cfg.tRojo/1000)    + "s)");
  enviarBT(" 2. Verde  (" + String(cfg.tVerde/1000)   + "s)");
  enviarBT(" 3. Amarillo (" + String(cfg.tAmarillo/1000) + "s)");
  enviarBT(" 0. Volver");
}
void mostrarMenuMensajes() {
  enviarBT("-- Mensajes OLED --");
  enviarBT(" 1. Rojo:    " + String(cfg.msgRojo));
  enviarBT(" 2. Verde:   " + String(cfg.msgVerde));
  enviarBT(" 3. Amarillo:" + String(cfg.msgAmarillo));
  enviarBT(" 0. Volver");
}
void mostrarMenuBuzzer() {
  enviarBT("-- Buzzer (cambio de fase) --");
  enviarBT(" 1. Beeps (" + String(cfg.beeps) + ")");
  enviarBT(" 2. Duracion ON (" + String(cfg.beepDuracion) + "ms)");
  enviarBT(" 3. Pausa (" + String(cfg.beepPausa) + "ms)");
  enviarBT(" 4. Frecuencia (" + String(cfg.beepFrecuencia) + "Hz)");
  enviarBT(" t. Probar");
  enviarBT(" 0. Volver");
}
void mostrarMenuScroll() {
  enviarBT("-- Scroll (1=lento 10=rapido) --");
  enviarBT("Actual: " + String(cfg.scrollVelocidad));
  enviarBT("Ingresa 1-10 o 0 para volver");
}
void mostrarMenuAlarma() {
  enviarBT("-- Alarma IR vehiculo --");
  enviarBT(" 1. Beeps (" + String(cfg.alarmaBeeps) + ")");
  enviarBT(" 2. Tono (" + String(cfg.alarmaFrecuencia) + "Hz)");
  enviarBT(" 3. Cooldown (" + String(cfg.alarmaCooldown/1000) + "s)");
  enviarBT(" t. Probar alarma");
  enviarBT(" 0. Volver");
}
void mostrarConfigActual() {
  enviarBT("=== CONFIG ACTUAL ===");
  enviarBT("Rojo: "    + String(cfg.tRojo/1000)    + "s | Verde: " + String(cfg.tVerde/1000)   + "s | Amarillo: " + String(cfg.tAmarillo/1000) + "s");
  enviarBT("Scroll: "  + String(cfg.scrollVelocidad));
  enviarBT("Buzzer: "  + String(cfg.beeps) + "x " + String(cfg.beepFrecuencia) + "Hz");
  enviarBT("Alarma IR: "+ String(cfg.alarmaBeeps) + "x " + String(cfg.alarmaFrecuencia) + "Hz cd:" + String(cfg.alarmaCooldown/1000) + "s");
  enviarBT("Emergencias SOS: " + String(totalEmergencias));
  enviarBT("Invasiones IR:   " + String(totalEventos));
}

// ================================================================
// BLUETOOTH — PROCESAR ENTRADA
// ================================================================
void procesarEntrada(String entrada) {
  entrada.trim();
  if (menuActual == MENU_NINGUNO) {
    if (entrada == "menu") { menuActual = MENU_PRINCIPAL; pausado = true; mostrarMenuPrincipal(); }
    else procesarComandoDirecto(entrada);
    return;
  }

  switch (menuActual) {

    case MENU_PRINCIPAL:
      if      (entrada == "1") { menuActual = MENU_TIEMPOS;  mostrarMenuTiempos();  }
      else if (entrada == "2") { menuActual = MENU_MENSAJES; mostrarMenuMensajes(); }
      else if (entrada == "3") { menuActual = MENU_BUZZER;   mostrarMenuBuzzer();   }
      else if (entrada == "4") { menuActual = MENU_SCROLL;   mostrarMenuScroll();   }
      else if (entrada == "5") { menuActual = MENU_ALARMA;   mostrarMenuAlarma();   }
      else if (entrada == "6") { mostrarConfigActual(); mostrarMenuPrincipal(); }
      else if (entrada == "7") { restaurarDefaults(); mostrarMenuPrincipal(); }
      else if (entrada == "0") {
        menuActual = MENU_NINGUNO;
        pausado    = false;
        tInicio    = millis();
        enviarBT("Ciclo reanudado");
      }
      else { enviarBT("Opcion invalida"); mostrarMenuPrincipal(); }
      break;

    case MENU_TIEMPOS:
      if      (entrada == "1") { menuActual = MENU_TIEMPOS_ROJO;     enviarBT("Tiempo ROJO en segundos (1-60):"); }
      else if (entrada == "2") { menuActual = MENU_TIEMPOS_VERDE;    enviarBT("Tiempo VERDE en segundos (1-60):"); }
      else if (entrada == "3") { menuActual = MENU_TIEMPOS_AMARILLO; enviarBT("Tiempo AMARILLO en segundos (1-30):"); }
      else if (entrada == "0") { menuActual = MENU_PRINCIPAL; mostrarMenuPrincipal(); }
      else { enviarBT("Opcion invalida"); mostrarMenuTiempos(); }
      break;

    case MENU_TIEMPOS_ROJO: {
      int v = entrada.toInt();
      if (v >= 1 && v <= 60) { cfg.tRojo = (unsigned long)v*1000; guardarConfig(); enviarBT("OK Rojo -> " + String(v) + "s"); }
      else enviarBT("Valor invalido (1-60)");
      menuActual = MENU_TIEMPOS; mostrarMenuTiempos(); break;
    }
    case MENU_TIEMPOS_VERDE: {
      int v = entrada.toInt();
      if (v >= 1 && v <= 60) { cfg.tVerde = (unsigned long)v*1000; guardarConfig(); enviarBT("OK Verde -> " + String(v) + "s"); }
      else enviarBT("Valor invalido (1-60)");
      menuActual = MENU_TIEMPOS; mostrarMenuTiempos(); break;
    }
    case MENU_TIEMPOS_AMARILLO: {
      int v = entrada.toInt();
      if (v >= 1 && v <= 30) { cfg.tAmarillo = (unsigned long)v*1000; guardarConfig(); enviarBT("OK Amarillo -> " + String(v) + "s"); }
      else enviarBT("Valor invalido (1-30)");
      menuActual = MENU_TIEMPOS; mostrarMenuTiempos(); break;
    }

    case MENU_MENSAJES:
      if      (entrada == "1") { menuActual = MENU_MSG_ROJO;     enviarBT("Mensaje ROJO (max 60 chars):"); }
      else if (entrada == "2") { menuActual = MENU_MSG_VERDE;    enviarBT("Mensaje VERDE (max 60 chars):"); }
      else if (entrada == "3") { menuActual = MENU_MSG_AMARILLO; enviarBT("Mensaje AMARILLO (max 60 chars):"); }
      else if (entrada == "0") { menuActual = MENU_PRINCIPAL; mostrarMenuPrincipal(); }
      else { enviarBT("Opcion invalida"); mostrarMenuMensajes(); }
      break;

    case MENU_MSG_ROJO:
      if (entrada.length() >= 1 && entrada.length() <= 60) {
        entrada.toUpperCase(); entrada.toCharArray(cfg.msgRojo, 61);
        guardarConfig(); enviarBT("OK mensaje ROJO guardado");
        iniciarScroll(cfg.msgRojo);
      } else enviarBT("Entre 1 y 60 caracteres");
      menuActual = MENU_MENSAJES; mostrarMenuMensajes(); break;

    case MENU_MSG_VERDE:
      if (entrada.length() >= 1 && entrada.length() <= 60) {
        entrada.toUpperCase(); entrada.toCharArray(cfg.msgVerde, 61);
        guardarConfig(); enviarBT("OK mensaje VERDE guardado");
        iniciarScroll(cfg.msgVerde);
      } else enviarBT("Entre 1 y 60 caracteres");
      menuActual = MENU_MENSAJES; mostrarMenuMensajes(); break;

    case MENU_MSG_AMARILLO:
      if (entrada.length() >= 1 && entrada.length() <= 60) {
        entrada.toUpperCase(); entrada.toCharArray(cfg.msgAmarillo, 61);
        guardarConfig(); enviarBT("OK mensaje AMARILLO guardado");
        iniciarScroll(cfg.msgAmarillo);
      } else enviarBT("Entre 1 y 60 caracteres");
      menuActual = MENU_MENSAJES; mostrarMenuMensajes(); break;

    case MENU_BUZZER:
      if      (entrada == "1") { menuActual = MENU_BUZZER_BEEPS;    enviarBT("Beeps (1-10):"); }
      else if (entrada == "2") { menuActual = MENU_BUZZER_DURACION; enviarBT("Duracion ON ms (50-1000):"); }
      else if (entrada == "3") { menuActual = MENU_BUZZER_PAUSA;    enviarBT("Pausa ms (50-1000):"); }
      else if (entrada == "4") { menuActual = MENU_BUZZER_TONO;     enviarBT("Frecuencia Hz (200-4000):"); }
      else if (entrada == "t") { sonarBuzzer(); mostrarMenuBuzzer(); }
      else if (entrada == "0") { menuActual = MENU_PRINCIPAL; mostrarMenuPrincipal(); }
      else { enviarBT("Opcion invalida"); mostrarMenuBuzzer(); }
      break;

    case MENU_BUZZER_BEEPS: {
      int v = entrada.toInt();
      if (v >= 1 && v <= 10) { cfg.beeps = v; guardarConfig(); enviarBT("OK Beeps -> " + String(v)); }
      else enviarBT("Valor invalido (1-10)");
      menuActual = MENU_BUZZER; mostrarMenuBuzzer(); break;
    }
    case MENU_BUZZER_DURACION: {
      int v = entrada.toInt();
      if (v >= 50 && v <= 1000) { cfg.beepDuracion = v; guardarConfig(); enviarBT("OK Duracion -> " + String(v) + "ms"); }
      else enviarBT("Valor invalido (50-1000)");
      menuActual = MENU_BUZZER; mostrarMenuBuzzer(); break;
    }
    case MENU_BUZZER_PAUSA: {
      int v = entrada.toInt();
      if (v >= 50 && v <= 1000) { cfg.beepPausa = v; guardarConfig(); enviarBT("OK Pausa -> " + String(v) + "ms"); }
      else enviarBT("Valor invalido (50-1000)");
      menuActual = MENU_BUZZER; mostrarMenuBuzzer(); break;
    }
    case MENU_BUZZER_TONO: {
      int v = entrada.toInt();
      if (v >= 200 && v <= 4000) { cfg.beepFrecuencia = v; guardarConfig(); enviarBT("OK Tono -> " + String(v) + "Hz"); sonarBuzzer(); }
      else enviarBT("Valor invalido (200-4000)");
      menuActual = MENU_BUZZER; mostrarMenuBuzzer(); break;
    }

    case MENU_SCROLL: {
      if (entrada == "0") { menuActual = MENU_PRINCIPAL; mostrarMenuPrincipal(); break; }
      int v = entrada.toInt();
      if (v >= 1 && v <= 10) { cfg.scrollVelocidad = v; scrollPaso = calcularPaso(v); guardarConfig(); enviarBT("OK Scroll -> " + String(v) + "/10"); }
      else enviarBT("Valor invalido (1-10)");
      mostrarMenuScroll(); break;
    }

    case MENU_ALARMA:
      if      (entrada == "1") { menuActual = MENU_ALARMA_BEEPS;    enviarBT("Beeps de alarma (1-20):"); }
      else if (entrada == "2") { menuActual = MENU_ALARMA_TONO;     enviarBT("Tono alarma Hz (200-4000):"); }
      else if (entrada == "3") { menuActual = MENU_ALARMA_COOLDOWN; enviarBT("Cooldown en segundos (1-30):"); }
      else if (entrada == "t") { activarAlarma(); enviarBT("Probando alarma IR..."); mostrarMenuAlarma(); }
      else if (entrada == "0") { menuActual = MENU_PRINCIPAL; mostrarMenuPrincipal(); }
      else { enviarBT("Opcion invalida"); mostrarMenuAlarma(); }
      break;

    case MENU_ALARMA_BEEPS: {
      int v = entrada.toInt();
      if (v >= 1 && v <= 20) { cfg.alarmaBeeps = v; guardarConfig(); enviarBT("OK Beeps alarma -> " + String(v)); }
      else enviarBT("Valor invalido (1-20)");
      menuActual = MENU_ALARMA; mostrarMenuAlarma(); break;
    }
    case MENU_ALARMA_TONO: {
      int v = entrada.toInt();
      if (v >= 200 && v <= 4000) { cfg.alarmaFrecuencia = v; guardarConfig(); enviarBT("OK Tono alarma -> " + String(v) + "Hz"); }
      else enviarBT("Valor invalido (200-4000)");
      menuActual = MENU_ALARMA; mostrarMenuAlarma(); break;
    }
    case MENU_ALARMA_COOLDOWN: {
      int v = entrada.toInt();
      if (v >= 1 && v <= 30) { cfg.alarmaCooldown = (unsigned long)v*1000; guardarConfig(); enviarBT("OK Cooldown -> " + String(v) + "s"); }
      else enviarBT("Valor invalido (1-30)");
      menuActual = MENU_ALARMA; mostrarMenuAlarma(); break;
    }

    default:
      menuActual = MENU_NINGUNO;
      break;
  }
}

// ================================================================
// COMANDOS DIRECTOS
// ================================================================
void procesarComandoDirecto(String cmd) {
  cmd.toLowerCase();
  if      (cmd == "estado")  { mostrarConfigActual(); }
  else if (cmd == "pausa")   { pausado = true;  enviarBT("Pausado"); }
  else if (cmd == "resume" || cmd == "reanudar") { pausado = false; tInicio = millis(); enviarBT("Reanudado"); }
  else if (cmd == "rojo")    { if (!pausado) { enviarBT("Escribe pausa primero"); return; } activarRojo(); }
  else if (cmd == "verde")   { if (!pausado) { enviarBT("Escribe pausa primero"); return; } activarVerde(); }
  else if (cmd == "amarillo"){ if (!pausado) { enviarBT("Escribe pausa primero"); return; } activarAmarillo(); }
  else if (cmd == "buzzer")  { sonarBuzzer(); }
  else if (cmd == "sensor")  { enviarBT("Sensor IR: " + String(digitalRead(SENSOR_IR) == LOW ? "DETECTANDO VEHICULO" : "LIBRE")); }
  else if (cmd == "sos")     { activarEmergencia(1); enviarBT("SOS manual (tipo ACCIDENTE) activado"); }  // prueba manual
  else if (cmd == "stats")   {
    enviarBT("--- ESTADISTICAS ---");
    enviarBT("Total invasiones IR: " + String(totalEventos));
    enviarBT("Ultima hora         : " + String(inversionesHora));
    enviarBT("Emergencias SOS     : " + String(totalEmergencias));
    enviarBT("Uptime              : " + String(millis()/1000) + "s");
  }
  else if (cmd == "btoff") {
    enviarBT("Bluetooth desactivado.");
    enviarBT("Dashboard: http://192.168.4.1");
    enviarBT("Reinicia el ESP32 para volver a activar BT.");
    delay(300);
    SerialBT.end();
    btStop();
  }
  else if (cmd == "ayuda" || cmd == "help") {
    enviarBT("-- COMANDOS RAPIDOS --");
    enviarBT(" menu      -> configurar todo");
    enviarBT(" estado    -> ver config + stats");
    enviarBT(" stats     -> estadisticas");
    enviarBT(" pausa     -> pausar ciclo");
    enviarBT(" resume    -> reanudar");
    enviarBT(" rojo/verde/amarillo");
    enviarBT(" buzzer    -> probar buzzer");
    enviarBT(" sensor    -> estado del IR");
    enviarBT(" sos       -> probar boton SOS (tipo ACCIDENTE)");
    enviarBT(" btoff     -> apagar BT");
    enviarBT("Dashboard: http://192.168.4.1");
    enviarBT("Boton SOS GPIO4: 1p=ACCIDENTE 2p=ATRACO 3p=MED");
  }
  else { enviarBT("Escribe ayuda o menu"); }
}
