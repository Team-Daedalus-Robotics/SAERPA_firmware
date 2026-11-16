#pragma once
#include <stdint.h>

// CSV de teste gravado junto ao firmware (flash). Uma linha = um pacote LoRa.
// Colunas: id_estacao,ds18b20,dht_temp,dht_ur,solo
// NA indica medicao indisponivel. Respeitar intervalos fisicos esperados.
static const char* const REGISTROS_CSV[] = {
  "1001,24,26,62,37",
  "1001,25,27,60,41",
  "1001,23,25,66,44",
  "1001,26,28,58,35",
  "1001,24,26,63,39",
  "1001,NA,27,61,38",
  "1001,25,NA,60,40",
  "1001,24,26,NA,42",
  "1001,24,26,62,NA",
  "1001,26,28,57,34"
};
static constexpr uint8_t QUANTIDADE_REGISTROS = sizeof(REGISTROS_CSV) / sizeof(REGISTROS_CSV[0]);
