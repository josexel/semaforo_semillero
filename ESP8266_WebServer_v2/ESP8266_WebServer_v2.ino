/*
  ================================================================
   ESP8266 RECEPTOR - SEMAFORO IoT v2
   Actualizado para recibir alertas SOS del ESP32
  ----------------------------------------------------------------
   Mensajes UDP que entiende:
     ALARMA:N:VEHICULO_EN_PASO  -> alerta IR (como antes)
     SOS:N:TIPO                 -> emergencia del botón SOS
       TIPO puede ser: ACCIDENTE, ATRACO, EMERGENCIA MEDICA
  ----------------------------------------------------------------
   HARDWARE:
     Buzzer en D5
  ================================================================
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

const char* ssid     = "Semaforo_Demo";
const char* password = "";         // Red abierta (igual que el ESP32 AP)

WiFiServer server(80);
WiFiUDP    udp;
unsigned int localPort = 4210;

// Estado
String estadoIR  = "SIN DATOS";
String alertaIR  = "";
String estadoSOS = "";
String tipoSOS   = "";
int    totalSOS  = 0;
unsigned long tUltimoSOS = 0;

int buzzerPin = D5;

// ================================================================
// Buzzer SOS: patrón urgente (3 beeps rápidos dobles)
// ================================================================
void sonarSOS() {
  // Patrón: duh-duh  duh-duh  duh-duh  (como sirena corta)
  for (int i = 0; i < 3; i++) {
    digitalWrite(buzzerPin, HIGH); delay(80);
    digitalWrite(buzzerPin, LOW);  delay(60);
    digitalWrite(buzzerPin, HIGH); delay(80);
    digitalWrite(buzzerPin, LOW);  delay(200);
  }
}

// Buzzer normal para alarma IR
void sonarAlarmaIR() {
  digitalWrite(buzzerPin, HIGH); delay(500);
  digitalWrite(buzzerPin, LOW);
}

// ================================================================
void setup() {
  Serial.begin(115200);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, LOW);

  WiFi.begin(ssid, password);
  Serial.print("Conectando a Semaforo_Demo");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nConectado! IP: " + WiFi.localIP().toString());

  udp.begin(localPort);
  server.begin();
  Serial.println("Servidor UDP y HTTP listos");
}

// ================================================================
void loop() {
  recibirUDP();
  manejarWeb();
}

// ================================================================
// UDP
// ================================================================
void recibirUDP() {
  int packetSize = udp.parsePacket();
  if (!packetSize) return;

  char incoming[255];
  int len = udp.read(incoming, 254);
  if (len > 0) incoming[len] = '\0';

  String msg = String(incoming);
  Serial.println("[UDP] Recibido: " + msg);

  if (msg.startsWith("ALARMA:")) {
    // Formato: ALARMA:N:VEHICULO_EN_PASO
    estadoIR = msg;
    alertaIR = msg;
    sonarAlarmaIR();
  }
  else if (msg.startsWith("SOS:")) {
    // Formato: SOS:N:TIPO
    int sep1 = msg.indexOf(':', 4);
    if (sep1 > 0) {
      totalSOS  = msg.substring(4, sep1).toInt();
      tipoSOS   = msg.substring(sep1 + 1);
      estadoSOS = msg;
      tUltimoSOS = millis();
      Serial.println("[SOS] Emergencia #" + String(totalSOS) + " tipo: " + tipoSOS);
      sonarSOS();
    }
  }
}

// ================================================================
// WEB PAGE
// ================================================================
void manejarWeb() {
  WiFiClient client = server.available();
  if (!client) return;

  unsigned long tEspera = millis();
  while (!client.available() && millis() - tEspera < 200) delay(1);

  // Leer request (ignorado, siempre respondemos lo mismo)
  while (client.available()) client.read();

  String colorSOS = tipoSOS == "ACCIDENTE" ? "#ff9800"
                  : tipoSOS == "ATRACO"    ? "#f44336"
                  : tipoSOS.length() > 0   ? "#e040fb"
                  : "#555";

  String iconoSOS = tipoSOS == "ACCIDENTE"         ? "🚗"
                  : tipoSOS == "ATRACO"             ? "🔫"
                  : tipoSOS == "EMERGENCIA MEDICA"  ? "🏥"
                  : "🆘";

  unsigned long segs = tUltimoSOS > 0 ? (millis() - tUltimoSOS) / 1000 : 0;

  String html = R"(<!DOCTYPE html>
<html lang='es'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<meta http-equiv='refresh' content='3'>
<title>ESP8266 - Semaforo Receptor</title>
<style>
  body { font-family: sans-serif; background:#0f1117; color:#e0e0e0; padding:20px; }
  h1   { color:#fff; font-size:1.4rem; }
  .card{ background:#1a1d27; border-radius:12px; padding:18px; margin:12px 0; border:1px solid #2d3148; }
  .alerta { border-left: 4px solid #e53935; }
  .sos    { border-left: 4px solid )" + colorSOS + R"(; }
  .lbl { font-size:0.78rem; color:#7a7f99; }
  .val { font-size:1.2rem; font-weight:700; margin-top:4px; }
  .badge { display:inline-block; padding:4px 12px; border-radius:20px;
           background:#1e1208; font-weight:700; margin-top:8px; }
</style>
</head>
<body>
<h1>🛰️ ESP8266 — Receptor Semaforo</h1>
<p style='color:#555;font-size:0.8rem'>Se actualiza cada 3 segundos</p>
)";

  // Tarjeta IR
  html += "<div class='card alerta'>";
  html += "<div class='lbl'>SENSOR IR — ULTIMA ALERTA</div>";
  html += "<div class='val'>" + (alertaIR.length() > 0 ? alertaIR : "Sin alertas IR") + "</div>";
  html += "</div>";

  // Tarjeta SOS
  html += "<div class='card sos'>";
  html += "<div class='lbl'>BOTON SOS — ULTIMA EMERGENCIA</div>";
  if (tipoSOS.length() > 0) {
    html += "<div class='val' style='color:" + colorSOS + "'>" + iconoSOS + " " + tipoSOS + "</div>";
    html += "<div class='badge' style='color:" + colorSOS + "'>Evento #" + String(totalSOS) + "</div>";
    html += "<div style='font-size:0.8rem;color:#7a7f99;margin-top:8px'>hace " + String(segs) + "s</div>";
  } else {
    html += "<div class='val' style='color:#555'>Sin emergencias SOS</div>";
  }
  html += "</div>";

  html += "</body></html>";

  client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n");
  client.print(html);
  client.stop();
}
