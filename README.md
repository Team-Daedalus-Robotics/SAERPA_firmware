# Heltec WiFi LoRa 32 V3 — Sender e Receiver revisados

## Arquitetura definitiva

- **Sender:** mede DS18B20, DHT11 (temperatura e umidade) e solo capacitivo; calibração de solo via Serial `s`, `m`, `r`; exibe as leituras no OLED; transmite **um único pacote LoRa por ciclo**. **Não usa Wi-Fi, NTP ou timestamp.**
- **Receiver:** recebe pelo SX1262, verifica o formato, apresenta as medições e RSSI no OLED **sem depender da conexão Wi-Fi**; sincroniza hora UTC por NTP, registra a hora de chegada de cada pacote e envia **um único POST HTTP** com array de medições para a Jetson.

## Instalação

Abra os sketches em diretórios separados, como fornecidos. **Mantenha `Amostras.h` na mesma pasta do `Receiver_Jetson.ino`**. Selecione **Heltec WiFi LoRa 32 (V3)** em ambas as placas, Arduino-ESP32 **3.0.2** e Heltec ESP32 Dev-Boards **2.1.6**. No Sender, instale `esp32-ds18b20` 2.0.3, `DHT sensor library` e dependências.

**Grave NOVAMENTE O SENDER**, mesmo que o código-fonte aberto seja o correto: o log `1001;UNSYNCED;...` prova que ele ainda estava com firmware antigo. Após reset, o Sender deve mostrar `=== SENDER HELTEC V3 | PACOTE UNICO | SEM Wi-Fi/NTP ===`.

Para o Receiver, configure `WIFI_SSID` e `JETSON_URL` no topo do código. A rede de exemplo é aberta, mas se precisar de senha use `WiFi.begin(WIFI_SSID, WIFI_PASSWORD)` e forneça uma variável de senha. O endereço HTTP de exemplo deve ser substituído pelo **IP/porta/endpoint real** da Jetson.

## Protocolo SX1262 — exatamente igual nas duas placas

| Campo | Valor |
|---|---|
| Frequência | 915 MHz (confirme legalidade e banda da placa) |
| Largura de banda | 125 kHz |
| Spreading factor | SF7 |
| Coding rate | 4/5 |
| Preamble | 8 símbolos |
| CRC | ativado |
| IQ inversion | desativada |
| Header | explícito, tamanho variável |

Payload com as quatro medições:

```text
1001;ds18b20=25;dht_temp=27;dht_ur=60;solo=41
```

**NA** representa medição inválida ou indisponível, sem fabricar números. O Sender envia o pacote como bytes, sem o terminador `\0`; o Receiver acrescenta o `\0` ao copiar o pacote e interpreta exatamente 5 campos separados por `;`.

## Timestamp e NTP

O Receiver registra `millis()` no instante de chegada. Se **já tiver sincronizado** com um servidor NTP nesta inicialização, também registra imediatamente UTC. Se ainda não sincronizou, guarda o pacote em RAM e, ao receber resposta SNTP real, reconstrói UTC como `hora_atual - tempo_decorrido_desde_recepcao`, com resolução de um segundo. Isso evita `UNSYNCED` e timestamp inventado, mas pressupõe que a placa permaneça ligada e sem reset durante a espera.

O callback SNTP é um critério de sincronização mais rigoroso do que apenas `WiFi.status() == WL_CONNECTED`. Se a rede bloquear DNS, UDP/123 ou exigir portal cativo, o OLED mostrará `N:--` e os dados permanecerão na fila. O Serial mostrará mensagens de diagnóstico.

## HTTP POST único

```json
{
  "id_estacao": 1001,
  "timestamp": "2026-11-15T14:30:00Z",
  "payload": [
    {"valor":25,"modelo_sensor":"ds18b20","unidade_de_medida":"C","medida":"Temperatura"},
    {"valor":27,"modelo_sensor":"dht11","unidade_de_medida":"C","medida":"Temperatura"},
    {"valor":60,"modelo_sensor":"dht11","unidade_de_medida":"%","medida":"Umidade"},
    {"valor":41,"modelo_sensor":"solo_cap_v2","unidade_de_medida":"%","medida":"UmidadeSolo"}
  ]
}
```

JSON ilustrativo. Leituras `NA` são omitidas. Cada pacote recebido gera **um POST**, salvo tentativas de reenvio em caso de erro HTTP. Para eliminar duplicações em falha de rede, implemente idempotência na Jetson (idealmente número sequencial do pacote em uma evolução futura do protocolo).

## Saída OLED

- Sender: DS18B20, DHT11/UR, Solo %, ADC bruto e status do TX.
- Receiver: contador RX, fila, RX recente, últimas temperaturas/umidade, Solo %, RSSI, Wi-Fi, NTP e status HTTP. Uma leitura validada **aparece mesmo se NTP/HTTP estiver pendente**.
- Se o Receiver receber um pacote `;UNSYNCED;` do firmware antigo, o Serial avisará e o OLED indicará `SENDER ANTIGO!`.

## Limitações importantes

- `TX OK` significa **transmissão física concluída**, não confirmação de recepção no destino. Não há ACK LoRa nesta versão. O contador `RX` é a confirmação local de que o Receiver detectou o pacote.
- O HTTPClient é síncrono e pode manter o processamento do rádio parado por até alguns segundos durante um POST. Com o ciclo atual de ~15 segundos do Sender isso reduz o risco de perda, mas **não garante ausência de perda**; para alta confiabilidade, seriam necessários uma tarefa de rede independente, ACK/retransmissão LoRa e identificação sequencial de pacotes.
- A fila de 8 pacotes fica na RAM. Se NTP/Jetson indisponível por tempo suficiente, a fila lota e os próximos pacotes são descartados.
- O RTC pode continuar contando após obter NTP, mesmo se a conexão cair, mas não é um relógio de precisão garantida em desconexões longas.
- Código revisado estaticamente; não houve compilação local com o toolchain Heltec nem teste físico das duas placas.
