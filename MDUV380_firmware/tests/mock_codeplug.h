/*
 * mock_codeplug.h — хостовий стаб codeplug custom-data API для тестів dmr_rctl_cfg.c.
 * Симулює флеш одним буфером; реального codeplug.c/SPI не потребує.
 */
#ifndef _MOCK_CODEPLUG_H_
#define _MOCK_CODEPLUG_H_

#include "functions/codeplug.h"

/* Виставити вміст "флешу" так, ніби там лежить блок типу type довжиною len байт. */
void mock_codeplug_set_block(CodeplugCustomDataType_t type, const uint8_t *data, int len);

/* Симулювати відсутність будь-якого блоку (codeplugGetOpenGD77CustomDataBounded -> false). */
void mock_codeplug_clear(void);

#endif /* _MOCK_CODEPLUG_H_ */
