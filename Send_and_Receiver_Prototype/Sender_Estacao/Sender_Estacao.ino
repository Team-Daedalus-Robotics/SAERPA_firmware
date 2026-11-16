/*
  Heltec WiFi LoRa 32 V3/V3.2 - Estacao transmissora
  Somente sensores fisicos: DS18B20, DHT11 e solo capacitivo V2.0.
  Protocolo: id_estacao;ds18b20=VAL;dht_temp=VAL;dht_ur=VAL;solo=VAL
  Exemplo: 1001;ds18b20=25;dht_temp=26;dht_ur=62;solo=37
  Leituras invalidas/nao calibradas sao codificadas como NA
  Sender nao usa Wi-Fi nem relogio; timestamp e responsabilidade do Receiver.
  Um unico pacote LoRa por ciclo, com as quatro medicoes concatenadas.
*/

#include <Arduino.h>
#include "LoRaWan_APP.h"
#include <Preferences.h>
#include <HT_SSD1306Wire.h>

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


#include <OneWireESP32.h>
#include <DHT.h>
#include <math.h>

const uint32_t ID_ESTACAO = 1001;

// Configuracao LoRa DEVE ser igual a do receptor.
#define RF_FREQUENCY 915000000
#define TX_OUTPUT_POWER 5
#define LORA_BANDWIDTH 0        // 125 kHz
#define LORA_SPREADING_FACTOR 7
#define LORA_CODINGRATE 1       // 4/5
#define LORA_PREAMBLE_LENGTH 8
#define LORA_IQ_INVERSION_ON false

// Heltec WiFi LoRa 32 V3.2, conector J3. Sensores alimentados em 3V3.
// DS18B20: GPIO4 + resistor 4k7 entre DATA e 3V3.
// DHT11: GPIO5 + pull-up se o modulo nao tiver.
// Solo capacitivo V2.0: AOUT no GPIO6 (ADC1_CH5), sinal maximo 3,3 V.
#define PIN_DS18B20 7
#define PIN_DHT11 5
#define PIN_SOLO 6
// A calibracao do solo e armazenada na NVS, nao usa valores ficticios.
// Comando 's' no Monitor Serial: capturar leitura SECA.
// Comando 'm' no Monitor Serial: capturar leitura MOLHADA.
// Use fim de linha configurado como "Nova linha" ou "Nenhum".

// DS18B20: biblioteca esp32-ds18b20 (junkfix), baseada no periferico RMT.
// Criamos o driver no setup(), depois da inicializacao da placa.
OneWire32* ds18b20 = nullptr;
uint64_t enderecoDS18B20 = 0;
bool dsEncontrado = false;
DHT dht(PIN_DHT11, DHT11);

struct Leitura {
  const char* modelo;
  int valor;
  const char* unidade;
  const char* medida;
};

static RadioEvents_t eventosRadio;
static volatile bool radioLivre = true;
static Leitura leituras[4];
static uint8_t qtdLeituras = 0;
static unsigned long proximaTentativa = 0;
static char pacote[180];
static Preferences preferencias;
static int adcSeco = -1;
static int adcMolhado = -1;
static int ultimaLeituraSolo = -1;
static constexpr int DIFERENCA_MINIMA_ADC = 100;
static constexpr uint8_t AMOSTRAS_ADC = 32;
static int oledTempDS = -999, oledTempDHT = -999, oledUmidade = -1, oledSolo = -1;
static uint32_t totalTx = 0;
static char estadoTX[16] = "AGUARDANDO";

void atualizarOLED(bool forcar = false) {
  if (!oledPronto || (!forcar && millis() - ultimaAtualizacaoOLED < 500)) return;
  ultimaAtualizacaoOLED = millis();
  char linha[32];
  oled.clear();
  oled.drawString(0, 0, "SENDER 1001 | LoRa 915");
  if (oledTempDS == -999) snprintf(linha, sizeof(linha), "DS18: -- C");
  else snprintf(linha, sizeof(linha), "DS18: %d C", oledTempDS);
  oled.drawString(0, 11, linha);
  if (oledTempDHT == -999) snprintf(linha, sizeof(linha), "DHT: -- C  UR: --%%");
  else if (oledUmidade < 0) snprintf(linha, sizeof(linha), "DHT: %d C  UR: --%%", oledTempDHT);
  else snprintf(linha, sizeof(linha), "DHT: %d C  UR: %d%%", oledTempDHT, oledUmidade);
  oled.drawString(0, 22, linha);
  if (oledSolo < 0) snprintf(linha, sizeof(linha), "Solo: calibrar");
  else snprintf(linha, sizeof(linha), "Solo: %d%%", oledSolo);
  oled.drawString(0, 33, linha);
  if (ultimaLeituraSolo >= 0) snprintf(linha, sizeof(linha), "ADC solo: %d", ultimaLeituraSolo);
  else snprintf(linha, sizeof(linha), "ADC solo: --");
  oled.drawString(0, 44, linha);
  snprintf(linha, sizeof(linha), "TX: %s #%lu", estadoTX, (unsigned long)totalTx);
  oled.drawString(0, 54, linha);
  oled.display();
}


void onTxDone() {
  Serial.println("[LoRa] Transmissao concluida (nao confirma recebimento remoto).");
  radioLivre = true;
  ++totalTx;
  snprintf(estadoTX, sizeof(estadoTX), "OK");
}

void onTxTimeout() {
  Radio.Sleep();
  Serial.println("[LoRa] Timeout de transmissao.");
  snprintf(estadoTX, sizeof(estadoTX), "TIMEOUT");
  radioLivre = true;
}

// Media de leituras com descarte das primeiras amostras para estabilizar o ADC.
int medirADCsolo(int& minimo, int& maximo) {
  (void)analogRead(PIN_SOLO);
  delay(10);
  uint32_t soma = 0;
  minimo = 4095;
  maximo = 0;
  for (uint8_t i = 0; i < AMOSTRAS_ADC; ++i) {
    int leitura = analogRead(PIN_SOLO);
    soma += leitura;
    if (leitura < minimo) minimo = leitura;
    if (leitura > maximo) maximo = leitura;
    delay(3);
  }
  return (int)((soma + AMOSTRAS_ADC / 2) / AMOSTRAS_ADC);
}

bool soloCalibrado() {
  return adcSeco >= 0 && adcSeco <= 4095 &&
         adcMolhado >= 0 && adcMolhado <= 4095 &&
         abs(adcSeco - adcMolhado) >= DIFERENCA_MINIMA_ADC;
}

void mostrarCalibracao() {
  Serial.printf("[Solo] Calibracao: seco=%d | molhado=%d | diferenca=%d | %s\n",
                adcSeco, adcMolhado, abs(adcSeco - adcMolhado),
                soloCalibrado() ? "VALIDA" : "PENDENTE/INVALIDA");
  Serial.println("[Solo] Comandos: 's' captura SECO; 'm' captura MOLHADO; 'r' redefine a calibracao.");
}

void comandosCalibracao() {
  while (Serial.available()) {
    char comando = (char)Serial.read();
    if (comando == '\n' || comando == '\r' || comando == ' ') continue;
    if (comando == 'r' || comando == 'R') {
      adcSeco = -1;
      adcMolhado = -1;
      preferencias.remove("seco");
      preferencias.remove("molhado");
      Serial.println("[Solo] Calibracao apagada.");
      mostrarCalibracao();
    } else if (comando == 's' || comando == 'S' || comando == 'm' || comando == 'M') {
      int minimo, maximo;
      int adc = medirADCsolo(minimo, maximo);
      if (adc <= 0 || adc >= 4095) {
        Serial.printf("[Solo] ADC=%d proximo do limite; confira AOUT, GND e 3V3. Calibracao nao salva.\n", adc);
        continue;
      }
      if (comando == 's' || comando == 'S') {
        adcSeco = adc;
        preferencias.putInt("seco", adcSeco);
        Serial.printf("[Solo] Ponto SECO salvo: ADC=%d (min=%d; max=%d)\n", adc, minimo, maximo);
      } else {
        adcMolhado = adc;
        preferencias.putInt("molhado", adcMolhado);
        Serial.printf("[Solo] Ponto MOLHADO salvo: ADC=%d (min=%d; max=%d)\n", adc, minimo, maximo);
      }
      mostrarCalibracao();
    }
  }
}

bool carregarLeituras() {
  qtdLeituras = 0;
  oledTempDS = -999; oledTempDHT = -999; oledUmidade = -1; oledSolo = -1;

  // DS18B20: localizar o dispositivo, iniciar conversao, aguardar e ler.
  // A biblioteca retorna codigo 0 quando a leitura foi bem-sucedida.
  if (ds18b20 != nullptr) {
    if (!dsEncontrado) {
      dsEncontrado = (ds18b20->search(&enderecoDS18B20, 1) == 1);
      if (dsEncontrado) {
        Serial.println("[DS18B20] Sensor encontrado no barramento 1-Wire.");
      } else {
        Serial.println("[DS18B20] Nenhum sensor encontrado. Verifique GPIO7, 3V3, GND e pull-up 4k7.");
      }
    }

    if (dsEncontrado) {
      ds18b20->request();
      delay(800);  // 12 bits: conversao pode levar ate 750 ms.
      float tDS = NAN;
      const uint8_t erro = ds18b20->getTemp(enderecoDS18B20, tDS);
      if (erro == 0 && isfinite(tDS) && tDS >= -55.0f && tDS <= 125.0f && tDS != 85.0f) {
        leituras[qtdLeituras++] = {"ds18b20", (int)lroundf(tDS), "C", "Temperatura"};
        oledTempDS = (int)lroundf(tDS);
        Serial.printf("[DS18B20] Temperatura: %.2f C\n", tDS);
      } else {
        Serial.printf("[DS18B20] Leitura invalida (codigo %u). Novo escaneamento no proximo ciclo.\n", erro);
        dsEncontrado = false;
      }
    }
  } else {
    Serial.println("[DS18B20] Driver nao inicializado.");
  }

  // DHT11: uma leitura fornece temperatura e umidade.
  float uDHT = dht.readHumidity();
  float tDHT = dht.readTemperature();
  if (isfinite(tDHT) && tDHT >= 0 && tDHT <= 50) {
    oledTempDHT = (int)lroundf(tDHT);
    leituras[qtdLeituras++] = {"dht11", (int)lroundf(tDHT), "C", "Temperatura"};
  } else {
    Serial.println("[DHT11] Temperatura invalida.");
  }
  if (isfinite(uDHT) && uDHT >= 20 && uDHT <= 90) {
    oledUmidade = (int)lroundf(uDHT);
    leituras[qtdLeituras++] = {"dht11", (int)lroundf(uDHT), "%", "Umidade"};
  } else {
    Serial.println("[DHT11] Umidade invalida.");
  }

  // O ADC deve variar com a capacitancia. Nenhum percentual sera enviado
  // antes da calibracao real. Ao mergulhar, molhe somente a area sensora
  // indicada pelo fabricante; nao submerja os componentes eletronicos.
  int minimo, maximo;
  int adc = medirADCsolo(minimo, maximo);
  ultimaLeituraSolo = adc;
  Serial.printf("[Solo] GPIO%d ADC medio=%d | min=%d max=%d | seco=%d molhado=%d\n",
                PIN_SOLO, adc, minimo, maximo, adcSeco, adcMolhado);
  if (adc <= 0 || adc >= 4095) {
    Serial.println("[Solo] ADC saturado: confira alimentacao, GND e fio AOUT.");
  } else if (!soloCalibrado()) {
    Serial.println("[Solo] Sem calibracao valida: pressione 's' em condicao SECA e 'm' em MOLHADA.");
  } else {
    float percentual = 100.0f * (adc - adcSeco) / (float)(adcMolhado - adcSeco);
    int umidadeSolo = constrain((int)lroundf(percentual), 0, 100);
    oledSolo = umidadeSolo;
    Serial.printf("[Solo] Umidade estimada: %d%% (ADC %d)\n", umidadeSolo, adc);
    leituras[qtdLeituras++] = {"solo_cap_v2", umidadeSolo, "%", "UmidadeSolo"};
  }

  if (qtdLeituras == 0) {
    Serial.println("[Sensores] Nenhuma leitura valida neste ciclo.");
    return false;
  }
  Serial.printf("[Sensores] %u medicao(oes) validas neste ciclo.\n", qtdLeituras);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SENDER HELTEC V3 | PACOTE UNICO | SEM Wi-Fi/NTP ===");
  dht.begin();
  Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);
  iniciarOLED();
  atualizarOLED(true);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOLO, ADC_11db);
  if (!preferencias.begin("solo-cal", false)) {
    Serial.println("[Solo] ERRO ao acessar armazenamento de calibracao.");
  }
  adcSeco = preferencias.getInt("seco", -1);
  adcMolhado = preferencias.getInt("molhado", -1);
  mostrarCalibracao();
  ds18b20 = new OneWire32(PIN_DS18B20);
  if (ds18b20 == nullptr) {
    Serial.println("[DS18B20] Falha na alocacao do driver RMT.");
  }
  eventosRadio.TxDone = onTxDone;
  eventosRadio.TxTimeout = onTxTimeout;
  Radio.Init(&eventosRadio);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                    LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                    LORA_PREAMBLE_LENGTH, false, true, 0, 0,
                    LORA_IQ_INVERSION_ON, 3000);
}

// Envia UM unico pacote com todos os quatro campos, inclusive leituras indisponiveis.
// O campo NA evita interpretar a ausencia de uma leitura como valor zero.
void loop() {
  Radio.IrqProcess();
  comandosCalibracao();
  atualizarOLED();
  if (!radioLivre || (int32_t)(millis() - proximaTentativa) < 0) return;

  carregarLeituras();
  atualizarOLED(true);

  char ds[16] = "NA", dt[16] = "NA", ur[16] = "NA", solo[16] = "NA";
  for (uint8_t i = 0; i < qtdLeituras; ++i) {
    const Leitura &l = leituras[i];
    if (strcmp(l.modelo, "ds18b20") == 0) snprintf(ds, sizeof(ds), "%d", l.valor);
    else if (strcmp(l.modelo, "dht11") == 0 && strcmp(l.medida, "Temperatura") == 0)
      snprintf(dt, sizeof(dt), "%d", l.valor);
    else if (strcmp(l.modelo, "dht11") == 0 && strcmp(l.medida, "Umidade") == 0)
      snprintf(ur, sizeof(ur), "%d", l.valor);
    else if (strcmp(l.modelo, "solo_cap_v2") == 0)
      snprintf(solo, sizeof(solo), "%d", l.valor);
  }

  if (qtdLeituras == 0) {
    Serial.println("[TX] Nenhuma medicao valida; nao ha dados para transmitir.");
    snprintf(estadoTX, sizeof(estadoTX), "SEM DADOS");
    proximaTentativa = millis() + 15000;
    atualizarOLED(true);
    return;
  }

  const int n = snprintf(pacote, sizeof(pacote),
                         "%lu;ds18b20=%s;dht_temp=%s;dht_ur=%s;solo=%s",
                         (unsigned long)ID_ESTACAO, ds, dt, ur, solo);
  if (n <= 0 || n >= (int)sizeof(pacote)) {
    Serial.println("[TX] Falha: buffer do pacote insuficiente.");
    snprintf(estadoTX, sizeof(estadoTX), "ERRO");
    proximaTentativa = millis() + 15000;
    return;
  }

  Serial.printf("[TX] Pacote unico (%d bytes): %s\n", n, pacote);
  radioLivre = false;
  snprintf(estadoTX, sizeof(estadoTX), "ENVIANDO");
  atualizarOLED(true);
  Radio.Send((uint8_t*)pacote, (uint16_t)n);
  proximaTentativa = millis() + 15000;
}
