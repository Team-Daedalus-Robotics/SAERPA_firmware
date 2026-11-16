/*
  HELTEC WIFI LORA 32 V3 - SENDER SIMULADO
  Base CSV em BaseSimulada.h (texto CSV embutido em flash).
  Uma linha CSV por pacote LoRa; nao usa sensores, Wi-Fi ou relogio.
  id_estacao,ds18b20,dht_temp,dht_ur,solo
*/
#include <Arduino.h>
#include "LoRaWan_APP.h"
#include <HT_SSD1306Wire.h>
#include "BaseSimulada.h"

#define RF_FREQUENCY 915000000
#define TX_OUTPUT_POWER 5
#define LORA_BANDWIDTH 0
#define LORA_SPREADING_FACTOR 7
#define LORA_CODINGRATE 1
#define LORA_PREAMBLE_LENGTH 8
#define LORA_IQ_INVERSION_ON false

static SSD1306Wire oled(0x3C, 400000, 17, 18, GEOMETRY_128_64, 21);
static RadioEvents_t eventosRadio;
static volatile bool radioLivre = true;
static uint32_t proximoEnvio = 0;
static uint32_t transmitidos = 0;
static uint8_t indice = 0;
static char pacote[96];
static char estado[20] = "INICIO";

void atualizarOLED() {
  char linha[36];
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(0, 0, "SENDER CSV | LoRa 915");
  snprintf(linha, sizeof(linha), "Registro: %u/%u", (unsigned)(indice + 1),
           (unsigned)QUANTIDADE_REGISTROS);
  oled.drawString(0, 12, linha);
  snprintf(linha, sizeof(linha), "TX concluidos: %lu", (unsigned long)transmitidos);
  oled.drawString(0, 24, linha);
  oled.drawString(0, 36, String(estado));
  oled.drawString(0, 50, "Sem sensores / NTP");
  oled.display();
}
void onTxDone() {
  ++transmitidos;
  radioLivre = true;
  snprintf(estado, sizeof(estado), "TX CONCLUIDO");
  Serial.println("[LoRa] TX concluido (sem ACK de recepcao).");
}
void onTxTimeout() {
  radioLivre = true;
  snprintf(estado, sizeof(estado), "TX TIMEOUT");
  Radio.Sleep();
  Serial.println("[LoRa] Timeout TX; registro sera repetido.");
  // Volta ao registro anterior, pois indice avanca ao iniciar o envio.
  indice = (indice == 0) ? QUANTIDADE_REGISTROS - 1 : indice - 1;
}
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SENDER CSV SIMULADO | SEM SENSORES / Wi-Fi / NTP ===");
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW);
  delay(100);
  oled.init();
  eventosRadio.TxDone = onTxDone;
  eventosRadio.TxTimeout = onTxTimeout;
  Radio.Init(&eventosRadio);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                    LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                    LORA_PREAMBLE_LENGTH, false, true, 0, 0,
                    LORA_IQ_INVERSION_ON, 3000);
  Serial.println("[LoRa] TX 915MHz, SF7, BW125, CR4/5, CRC ON.");
  Serial.printf("[CSV] %u registros de demonstracao disponiveis.\n",
                (unsigned)QUANTIDADE_REGISTROS);
  atualizarOLED();
}
void loop() {
  Radio.IrqProcess();
  if (!radioLivre || (int32_t)(millis() - proximoEnvio) < 0) return;
  const char* registro = REGISTROS_CSV[indice];
  const size_t n = strlen(registro);
  if (n == 0 || n >= sizeof(pacote)) {
    Serial.println("[CSV] Registro excede o buffer; descartado.");
    indice = (indice + 1) % QUANTIDADE_REGISTROS;
    proximoEnvio = millis() + 3000;
    return;
  }
  memcpy(pacote, registro, n + 1);
  Serial.printf("[TX CSV] linha=%u, bytes=%u, conteudo=%s\n",
                (unsigned)(indice + 1), (unsigned)n, pacote);
  snprintf(estado, sizeof(estado), "ENVIANDO CSV");
  radioLivre = false;
  atualizarOLED();
  Radio.Send((uint8_t*)pacote, (uint16_t)n);
  indice = (indice + 1) % QUANTIDADE_REGISTROS;
  proximoEnvio = millis() + 10000;
}
