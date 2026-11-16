#pragma once
#include <stdint.h>

// Estrutura declarada em arquivo .h para que o gerador de prototipos
// do Arduino IDE a conheca antes das funcoes que recebem Amostras.
struct Amostras {
  uint32_t id;
  bool valido[4];
  long valor[4];
};
