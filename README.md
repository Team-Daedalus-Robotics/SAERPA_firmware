# Heltec LoRa — pacote único por ciclo (Sender + Receiver + OLED)

## Placas e dependências
- Em **ambas**: `Heltec WiFi LoRa 32 (V3)` (não `WiFi Kit 32 V3`), ESP32 Arduino Core **3.0.2**, Heltec ESP32 Dev-Boards **2.1.6**.
- Sender: biblioteca `esp32-ds18b20` **2.0.3**, DHT sensor library, Preferences.
- Receiver: WiFi, HTTPClient, time.h (core ESP32).
- O OLED SSD1306 integrado continua funcionando nos dois sketches.

## Novo protocolo LoRa
A cada ~15 s, **um único pacote** com 5 campos separados por ponto-e-vírgula:

`1001;ds18b20=24;dht_temp=26;dht_ur=62;solo=37`

O valor `NA` representa uma medição indisponível (por exemplo, solo sem calibração).

`1001;ds18b20=24;dht_temp=26;dht_ur=62;solo=NA`

- O Sender NÃO conecta ao Wi-Fi e NÃO gera timestamp.
- O Receiver captura a hora UTC da **chegada** (sincronização NTP). Se receber um pacote antes do NTP, guarda `millis()` da chegada e, após sincronizar, reconstrói o horário; NÃO envia `UNSYNCED` e NÃO posta na Jetson enquanto o relógio estiver inválido.
- Se a rede aberta exigir portal cativo ou não permitir acesso aos servidores NTP, **o Receiver permanecerá aguardando NTP**. É necessário garantir internet/DNS/NTP na rede, ou configurar outra fonte confiável de hora.
- Mesmo com um pacote LoRa, o Receiver gera até **quatro POSTs HTTP separados**, um por medição válida. Isso preserva a estrutura JSON **anterior** da API Jetson:

```json
{"id_estacao":1001,"timestamp":"2026-11-15T14:30:00Z","payload":{"valor":24,"modelo_sensor":"ds18b20","unidade_de_medida":"C","medida":"Temperatura"}}
```

Se sua API preferir **um único POST com todas as medições**, ela precisará de um contrato de JSON diferente; esta versão preserva o anterior. A URL `http://192.168.1.100:8000/api/medicoes` é a que constava no sketch fornecido e precisa apontar para sua Jetson real.

## OLED
- Sender: DS18B20, DHT11 temperatura/umidade, solo, ADC bruto, estado `TX` e contagem de pacotes.
- Receiver: contagem `RX` (pacotes LoRa, não medições HTTP), fila, atividade RX, último resumo, RSSI/SNR, Wi-Fi, NTP e HTTP.
- `TX OK` não prova recebimento remoto; `RX` no Receiver confirma recepção local.

## Calibração do solo
- DS18B20 GPIO4 (+ pull-up 4,7 kΩ); DHT11 GPIO5; AOUT solo GPIO6; 3,3 V e GND comum.
- Monitor Serial Sender a 115200: `s` salva ADC seco, `m` salva ADC molhado, `r` apaga a calibração.
- Umidade de solo fica `NA` até ter calibração válida.

## Primeiro teste
1. Grave **Receiver e Sender** com esta versão; protocolos antigos são incompatíveis com o novo formato.
2. No Receiver, confirme a rede Wi-Fi, o NTP com `[NTP] UTC sincronizado` e o URL HTTP.
3. No Sender, procure uma linha `[TX] Pacote unico ...` com todas as medições.
4. No Receiver, verifique `[RX] Pacote unico recebido`, os JSONs com timestamp UTC e o `HTTP` retornando 2xx.

> Os arquivos passaram por revisão estática, mas não foram compilados com o toolchain Windows/Heltec nem testados nas placas físicas neste ambiente.
