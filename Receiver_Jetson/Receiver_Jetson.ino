/*
  Heltec WiFi LoRa 32 V3/V3.2 - Receiver / Gateway Wi-Fi -> REST POST
  Protocolo LoRa: id;ds18b20=VAL;dht_temp=VAL;dht_ur=VAL;solo=VAL
  Receiver calcula timestamp UTC da chegada por NTP (Wi-Fi).
  Um pacote LoRa se transforma em um unico HTTP POST com payload em array.
  Endpoint HTTP na LAN da Jetson (configurar IP/porta/caminho).
*/
#include <Arduino.h>
#include "LoRaWan_APP.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <HT_SSD1306Wire.h>
#include "Amostras.h"

// OLED SSD1306 embutido: SDA=17, SCL=18, RST=21, Vext=36.
// Usa a biblioteca de display fornecida pela Heltec ESP32 Dev-Boards.
static SSD1306Wire oled(0x3C, 400000, 17, 18, GEOMETRY_128_64, 21);
static bool oledPronto = false;
static uint32_t ultimaAtualizacaoOLED = 0;

void iniciarOLED() {
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW); // Vext ativo em nivel baixo
  delay(100);
  oled.init();
  oledPronto = true;
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.clear();
  oled.drawString(0, 0, "Heltec LoRa V3");
  oled.display();
}


const char* WIFI_SSID = "SPACE BC - VISITANTES";
//const char* WIFI_PASSWORD = "";
const char* JETSON_URL = "http://192.168.1.100:8000/api/medicoes";

#define RF_FREQUENCY 915000000
#define LORA_BANDWIDTH 0
#define LORA_SPREADING_FACTOR 7
#define LORA_CODINGRATE 1
#define LORA_PREAMBLE_LENGTH 8
#define LORA_IQ_INVERSION_ON false

static RadioEvents_t eventosRadio;
static bool iniciarRecepcao = true;
static constexpr size_t MAX_PACOTE = 180;
static constexpr uint8_t FILA_TAMANHO = 8;
struct PacotePendente {
  char dados[MAX_PACOTE + 1];
  char timestamp[21]; // UTC da chegada, ou preenchido depois da sincronizacao
  uint32_t chegadaMs;
};
static PacotePendente fila[FILA_TAMANHO];
static bool ntpConfigurado = false;
static bool horaPronta = false;
static unsigned long ultimaChecagemHora = 0;
static uint8_t inicioFila = 0;
static uint8_t fimFila = 0;
static uint8_t quantidadeFila = 0;
static unsigned long ultimaTentativaWiFi = 0;
static unsigned long proximoPOST = 0;
static uint32_t pacotesRecebidos = 0, pacotesDescartados = 0;
static unsigned long ultimaRecepcao = 0;
static int16_t ultimoRSSI = 0;
static int8_t ultimoSNR = 0;
static char ultimaMedicao[32] = "--";
static char ultimoPOST[16] = "PENDENTE";

void atualizarOLED(bool forcar = false) {
  if (!oledPronto || (!forcar && millis() - ultimaAtualizacaoOLED < 500)) return;
  ultimaAtualizacaoOLED = millis();
  char linha[40];
  oled.clear();
  oled.drawString(0, 0, "RECEIVER | LoRa 915");
  snprintf(linha, sizeof(linha), "RX: %lu  Fila: %u",
           (unsigned long)pacotesRecebidos, quantidadeFila);
  oled.drawString(0, 11, linha);
  if (!pacotesRecebidos) snprintf(linha, sizeof(linha), "LoRa: aguardando");
  else snprintf(linha, sizeof(linha), "LoRa: %s",
                millis() - ultimaRecepcao < 30000 ? "RX recente" : "sem RX recente");
  oled.drawString(0, 22, linha);
  oled.drawString(0, 33, String(ultimaMedicao));
  if (pacotesRecebidos) snprintf(linha, sizeof(linha), "RSSI:%d SNR:%d", ultimoRSSI, ultimoSNR);
  else snprintf(linha, sizeof(linha), "RSSI:-- SNR:--");
  oled.drawString(0, 44, linha);
  snprintf(linha, sizeof(linha), "W:%s N:%s H:%s",
           WiFi.status() == WL_CONNECTED ? "OK" : "OFF",
           horaPronta ? "OK" : "--", ultimoPOST);
  oled.drawString(0, 54, linha);
  oled.display();
}



// Nunca criar timestamps artificiais. Quando o NTP demora, a fila conserva
// millis() da chegada e reconstruimos a hora UTC apos a sincronizacao.
bool obterTimestampUTC(char* saida, size_t capacidade, time_t instante = 0) {
  const time_t agora = instante ? instante : time(nullptr);
  if (agora < 1704067200) return false;  // 2024-01-01 UTC
  struct tm utc;
  gmtime_r(&agora, &utc);
  return strftime(saida, capacidade, "%Y-%m-%dT%H:%M:%SZ", &utc) == 20;
}

bool enfileirar(const uint8_t* dados, uint16_t tamanho) {
  if (tamanho == 0 || tamanho > MAX_PACOTE || quantidadeFila >= FILA_TAMANHO) return false;
  PacotePendente& p = fila[fimFila];
  memcpy(p.dados, dados, tamanho);
  p.dados[tamanho] = '\0';
  p.chegadaMs = millis();
  p.timestamp[0] = '\0';
  // Mesmo se o sinalizador ainda nao foi atualizado, use a hora valida.
  obterTimestampUTC(p.timestamp, sizeof(p.timestamp));
  fimFila = (fimFila + 1) % FILA_TAMANHO;
  ++quantidadeFila;
  return true;
}

void onRxDone(uint8_t* payload, uint16_t size, int16_t rssi, int8_t snr) {
  if (enfileirar(payload, size)) {
    ++pacotesRecebidos;
    ultimaRecepcao = millis();
    ultimoRSSI = rssi;
    ultimoSNR = snr;
    Serial.printf("[RX] Pacote unico recebido: %u bytes | RSSI=%d dBm | SNR=%d dB\n",
                  size, rssi, snr);
  } else {
    ++pacotesDescartados;
    Serial.println("[RX] Pacote descartado: tamanho invalido ou fila cheia.");
  }
  iniciarRecepcao = true;
}

void onRxTimeout() { iniciarRecepcao = true; }
void onRxError() { Serial.println("[LoRa] Erro de recepcao."); iniciarRecepcao = true; }

bool inteiro(const String& s, long& n, long minimo, long maximo) {
  if (s.isEmpty() || s.length() > 11) return false;
  size_t i = (s[0] == '-') ? 1 : 0;
  if (i == s.length()) return false;
  for (; i < s.length(); ++i) if (s[i] < '0' || s[i] > '9') return false;
  const long v = s.toInt();
  if (v < minimo || v > maximo) return false;
  n = v;
  return true;
}

const char* const CHAVES[4] = {"ds18b20=", "dht_temp=", "dht_ur=", "solo="};
const char* const MODELOS[4] = {"ds18b20", "dht11", "dht11", "solo_cap_v2"};
const char* const UNIDADES[4] = {"C", "C", "%", "%"};
const char* const MEDIDAS[4] = {"Temperatura", "Temperatura", "Umidade", "UmidadeSolo"};

// Formato estrito, cinco campos, com valores NA quando o sensor falhou.
bool decodificarPacote(const char* texto, Amostras& amostras) {
  String linha(texto);
  String partes[5];
  int inicio = 0;
  for (int i = 0; i < 4; ++i) {
    int sep = linha.indexOf(';', inicio);
    if (sep < 0) return false;
    partes[i] = linha.substring(inicio, sep);
    inicio = sep + 1;
  }
  partes[4] = linha.substring(inicio);
  if (partes[4].indexOf(';') >= 0) return false;

  long id;
  if (!inteiro(partes[0], id, 1, 2147483647L)) return false;
  amostras.id = (uint32_t)id;
  for (uint8_t i = 0; i < 4; ++i) {
    String campo = partes[i + 1];
    if (!campo.startsWith(CHAVES[i])) return false;
    String valor = campo.substring(strlen(CHAVES[i]));
    amostras.valido[i] = (valor != "NA");
    if (amostras.valido[i]) {
      long minimo = (i == 0) ? -55 : (i == 1 ? 0 : 0);
      long maximo = (i == 0) ? 125 : (i == 1 ? 50 : 100);
      if (!inteiro(valor, amostras.valor[i], minimo, maximo)) return false;
    } else {
      amostras.valor[i] = 0;
    }
  }
  return true;
}

// Contrato JSON atualizado: um unico documento por lote LoRa.
// Leituras NA sao omitidas. Retorna false quando nao existe nenhuma medida valida.
bool criarJSON(const Amostras& amostras, const char* timestamp, String& json) {
  json = "{\"id_estacao\":" + String(amostras.id) +
         ",\"timestamp\":\"" + String(timestamp) + "\",\"payload\":[";
  json.reserve(480);
  uint8_t incluidas = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    if (!amostras.valido[i]) continue;
    if (incluidas > 0) json += ',';
    json += "{\"valor\":" + String(amostras.valor[i]) +
            ",\"modelo_sensor\":\"" + String(MODELOS[i]) +
            "\",\"unidade_de_medida\":\"" + String(UNIDADES[i]) +
            "\",\"medida\":\"" + String(MEDIDAS[i]) + "\"}";
    ++incluidas;
  }
  json += "]}";
  return incluidas > 0;
}

void atualizarRelogio() {
  if (WiFi.status() != WL_CONNECTED) {
    ntpConfigurado = false;
    // Uma vez acertado, o RTC do ESP32 segue contando mesmo sem Wi-Fi.
    horaPronta = (time(nullptr) >= 1704067200);
    return;
  }
  if (!ntpConfigurado) {
    configTime(0, 0, "a.ntp.br", "pool.ntp.org", "time.google.com");
    ntpConfigurado = true;
    ultimaChecagemHora = 0;
    Serial.println("[NTP] Solicitando horario UTC...");
  }
  if (millis() - ultimaChecagemHora >= 2000 || ultimaChecagemHora == 0) {
    ultimaChecagemHora = millis();
    char ts[21];
    bool valido = obterTimestampUTC(ts, sizeof(ts));
    if (valido && !horaPronta) Serial.printf("[NTP] UTC sincronizado: %s\n", ts);
    horaPronta = valido;
  }
}

void iniciarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[Wi-Fi] Conectando em %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID); // rede aberta; para rede protegida use WiFi.begin(ssid, senha)
  ultimaTentativaWiFi = millis();
}

bool enviarPOST(const String& json) {
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(ultimoPOST, sizeof(ultimoPOST), "OFF");
    return false;
  }
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2500);
  if (!http.begin(JETSON_URL)) {
    snprintf(ultimoPOST, sizeof(ultimoPOST), "URL ERRO");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const int codigo = http.POST(json);
  const bool sucesso = (codigo >= 200 && codigo < 300);
  snprintf(ultimoPOST, sizeof(ultimoPOST), "%s", sucesso ? "OK" : "ERRO");
  Serial.printf("[HTTP] POST %s: codigo=%d\n", sucesso ? "OK" : "FALHOU", codigo);
  http.end();
  return sucesso;
}

void retirarPacote() {
  inicioFila = (inicioFila + 1) % FILA_TAMANHO;
  --quantidadeFila;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== RECEIVER HELTEC V3 -> JETSON / PACOTE UNICO ===");
  // Inicializar a placa e OLED antes do Wi-Fi.
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);
  iniciarOLED();
  atualizarOLED(true);
  iniciarWiFi();
  eventosRadio.RxDone = onRxDone;
  eventosRadio.RxTimeout = onRxTimeout;
  eventosRadio.RxError = onRxError;
  Radio.Init(&eventosRadio);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetRxConfig(MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                    LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH, 0,
                    false, 0, true, 0, 0, LORA_IQ_INVERSION_ON, true);
}

void loop() {
  Radio.IrqProcess();
  if (iniciarRecepcao) {
    iniciarRecepcao = false;
    Radio.Rx(0);
  }
  if (WiFi.status() != WL_CONNECTED && millis() - ultimaTentativaWiFi >= 10000) {
    iniciarWiFi();
  }
  atualizarRelogio();
  atualizarOLED();

  if (quantidadeFila == 0 || (int32_t)(millis() - proximoPOST) < 0) return;
  PacotePendente& p = fila[inicioFila];
  Amostras amostras{};
  if (!decodificarPacote(p.dados, amostras)) {
    Serial.printf("[PROTOCOLO] Lote invalido: %s\n", p.dados);
    retirarPacote();
    return;
  }
  snprintf(ultimaMedicao, sizeof(ultimaMedicao), "DS:%s DHT:%s",
           amostras.valido[0] ? String(amostras.valor[0]).c_str() : "--",
           amostras.valido[1] ? String(amostras.valor[1]).c_str() : "--");

  if (!p.timestamp[0]) {
    if (!horaPronta) {
      snprintf(ultimoPOST, sizeof(ultimoPOST), "AGUARDA NTP");
      // Nao transmitir timestamp UNSYNCED ou null: aguardar a hora real.
      atualizarOLED(true);
      return;
    }
    const time_t agora = time(nullptr);
    // millis() conserva o instante da chegada. Reconstruimos a hora de RX
    // em vez de marcar o horario posterior de envio HTTP.
    const uint32_t idadeSegundos = (millis() - p.chegadaMs) / 1000UL;
    if (!obterTimestampUTC(p.timestamp, sizeof(p.timestamp), agora - idadeSegundos)) return;
  }

  // Uma recepcao LoRa gera somente um POST com todas as medidas validas.
  String json;
  if (!criarJSON(amostras, p.timestamp, json)) {
    Serial.println("[RX] Pacote sem medicoes validas; nenhum POST enviado.");
    retirarPacote();
    atualizarOLED(true);
    return;
  }
  Serial.printf("[RX] Lote %lu: %s\n", (unsigned long)amostras.id, p.dados);
  Serial.printf("[JSON] %s\n", json.c_str());
  if (!enviarPOST(json)) {
    // Mantem o lote na fila e repete um unico POST na proxima tentativa.
    proximoPOST = millis() + 5000;
    atualizarOLED(true);
    return;
  }
  Serial.printf("[RX] Lote confirmado via HTTP, UTC da chegada: %s\n", p.timestamp);
  retirarPacote();
  proximoPOST = millis();
  atualizarOLED(true);
}
