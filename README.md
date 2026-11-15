# Heltec WiFi LoRa 32 V3/V3.2 — pacote LoRa único + POST único

## Arquivos

- `Sender_Estacao/Sender_Estacao.ino`: preservado integralmente da versão anterior; transmite as quatro leituras em **um pacote LoRa** e apresenta valores no OLED. Não utiliza Wi-Fi nem NTP.
- `Receiver_Jetson/Receiver_Jetson.ino` + `Receiver_Jetson/Amostras.h`: lê o pacote LoRa, cria timestamp UTC e envia **um único POST HTTP** para a Jetson, com todas as medições válidas em `payload` (array JSON). Mantém OLED, fila e retentativas.

## Protocolo LoRa

`1001;ds18b20=24;dht_temp=26;dht_ur=56;solo=32`

Se uma leitura não estiver disponível, seu valor será `NA`.

## Novo contrato REST

```json
{
  "id_estacao": 1001,
  "timestamp": "2026-11-15T14:30:00Z",
  "payload": [
    {"valor": 24, "modelo_sensor": "ds18b20", "unidade_de_medida": "C", "medida": "Temperatura"},
    {"valor": 26, "modelo_sensor": "dht11", "unidade_de_medida": "C", "medida": "Temperatura"},
    {"valor": 56, "modelo_sensor": "dht11", "unidade_de_medida": "%", "medida": "Umidade"},
    {"valor": 32, "modelo_sensor": "solo_cap_v2", "unidade_de_medida": "%", "medida": "UmidadeSolo"}
  ]
}
```

O JSON acima é ilustrativo. As medições são preenchidas com dados reais da estação. Registros `NA` são omitidos do array; caso todos os sensores estejam inválidos, não é feito POST.

O timestamp é atribuído pelo Receiver. Se seu relógio não estiver sincronizado, o pacote fica na fila até ser possível reconstruir a hora UTC da chegada com `millis()`; após reiniciar a placa, os pacotes em RAM são perdidos. A Jetson precisa aceitar `payload` como array (contrato anterior era um objeto individual).

## Configuração

1. No Arduino IDE, selecione **Heltec WiFi LoRa 32 (V3)** e Arduino-ESP32 3.0.2.
2. Instale Heltec ESP32 Dev-Boards e, para o Sender, esp32-ds18b20, DHT sensor library e dependências.
3. No Receiver, configure `WIFI_SSID` e `JETSON_URL` conforme a rede. Para rede Wi-Fi protegida, adapte `WiFi.begin` para passar a senha.
4. Grave o Receiver atualizado. O Sender original desta versão pode continuar gravado, já que o protocolo LoRa não mudou.
5. Observe no Serial do Receiver `[JSON]` e `[HTTP] POST OK` após o NTP sincronizar.

## Notas técnicas

- A fila possui 8 pacotes e está na RAM; falhas persistentes de Wi-Fi, NTP ou HTTP podem esgotá-la.
- Uma falha ou timeout HTTP faz o Receiver reenviar o **mesmo lote**. Para evitar duplicidade em situações em que a Jetson processou a primeira tentativa sem retornar resposta, implemente idempotência na API (ex.: chave `id_estacao + timestamp`).
- Valores e estados no OLED foram preservados.
- Os arquivos foram revisados estaticamente; não foram compilados com o ambiente Arduino do usuário nem testados em hardware.

## Correção de compilação Arduino IDE

A estrutura `Amostras` foi movida para `Receiver_Jetson/Amostras.h`, que é
incluído antes das funções `decodificarPacote` e `criarJSON`. Isso evita
o erro de protótipos automáticos do Arduino IDE `Amostras has not been declared`.

**Importante:** mantenha o arquivo `Amostras.h` na mesma pasta do `Receiver_Jetson.ino`.
Abra o projeto pelo `.ino`, não copie apenas o conteúdo do sketch.
Nenhuma alteração foi feita no protocolo LoRa, nas telas OLED ou no Sender.
