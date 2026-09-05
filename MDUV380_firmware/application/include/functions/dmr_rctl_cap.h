/*
 * dmr_rctl_cap.h — тимчасове захоплення сирих DMR data-burst для реверсу стокового
 * протоколу керування (RCTL_COMPAT.md, крок 2.1).
 *
 * НАВІЩО. Стокова TYT MD-UV3x0 має Radio Check / Remote Mon. / Radio Enable / Radio
 * Disable (підтверджено фото меню 2026-09-05). Щоб зробити наш формат сумісним, треба
 * СПОЧАТКУ побачити точні байти, які стокова шле в ефір. Звичайний RX-шлях у HR-C6000.c
 * читає сирі байти (сторінка 0x02) лише для data-типів 6 і 7; ці команди майже напевно
 * йдуть CSBK (тип 3), який той шлях не бачить. Тут — окреме кільце, що ловить БУДЬ-ЯКИЙ
 * тип, але тільки коли явно ввімкнено з ПК (щоб у звичайній роботі не було ані зайвого
 * SPI-читання в ISR, ані впливу на роботу).
 *
 * Це діагностика для розробки, а не бойова функція: вмикається лише USB-командою,
 * після реверсу приберемо. Компілюється в ніщо без -DENABLE_AES -DENABLE_DMR_DATA.
 */
#ifndef DMR_RCTL_CAP_H
#define DMR_RCTL_CAP_H

#include <stdint.h>

#define DMR_RCTL_CAP_BURST_LEN   12   /* інфо-байти одного burst (сторінка 0x02, LC_DATA_LENGTH) */
#define DMR_RCTL_CAP_SLOTS        8   /* команда стокової -- зазвичай 1 CSBK; 8 з запасом.
                                        * НЕ більше: CCM майже повний -- 24 слоти давали
                                        * переповнення регіону CCMRAM на 172 байти (CI #66). */

typedef struct
{
	uint8_t seq;                          /* монотонний № burst — щоб ПК бачив порядок і пропуски */
	uint8_t type;                         /* rxDataType: 3=CSBK, 6=data header, 7=rate-1/2, ... */
	uint8_t data[DMR_RCTL_CAP_BURST_LEN]; /* сирі інфо-байти як їх віддав чип (ще не розшифровані) */
} dmr_rctl_cap_entry_t;

void dmrRctlCapArm(int on);      /* 1 = почати ловити, 0 = зупинити */
int  dmrRctlCapArmed(void);      /* 1, якщо захоплення активне (читається з ISR — має бути дешевим) */
void dmrRctlCapReset(void);      /* очистити кільце й лічильники */

/* Викликається з ISR HR-C6000 для КОЖНОГО CRC-валідного data-burst, коли armed.
 * Кладе burst у кільце. Політика — "стоп, коли повне": зберігаємо ПОЧАТОК послідовності,
 * а не найновіше, бо для реверсу важливий перший цілий кадр команди. */
void dmrRctlCapBurst(int type, const uint8_t *p12);

/* Викласти захоплене для USB. Формат:
 *   [0]=count  [1]=dropped_hi  [2]=dropped_lo  [3]=armed  далі count x (seq, type, 12 байт)
 * Повертає кількість записаних байт (<= maxLen). */
int  dmrRctlCapDump(uint8_t *out, int maxLen);

#endif /* DMR_RCTL_CAP_H */
