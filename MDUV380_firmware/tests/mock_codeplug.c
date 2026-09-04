/* mock_codeplug.c — див. mock_codeplug.h. */
#include "mock_codeplug.h"
#include <string.h>

/* Кілька блоків одночасно: 2026-09-04 RCTL почав тримати конфіг ("RCTL", пише ПК) і
 * стан anti-replay ("RCTS", веде прошивка) в РІЗНИХ блоках, тож мок на один буфер
 * більше не годився -- запис стану затирав би конфіг і тест перевіряв би не те. */
#define MOCK_MAX_BLOCKS 4

static struct
{
	CodeplugCustomDataType_t type;
	uint8_t                  buf[256];
	int                      len;
} s_blk[MOCK_MAX_BLOCKS];
static int s_writeCount = 0;

static int find_slot(CodeplugCustomDataType_t type)
{
	for (int i = 0; i < MOCK_MAX_BLOCKS; i++)
	{
		if ((s_blk[i].len > 0) && (s_blk[i].type == type)) { return i; }
	}
	return -1;
}

static int alloc_slot(CodeplugCustomDataType_t type)
{
	int i = find_slot(type);
	if (i >= 0) { return i; }
	for (i = 0; i < MOCK_MAX_BLOCKS; i++)
	{
		if (s_blk[i].len == 0) { return i; }
	}
	return -1;
}

void mock_codeplug_set_block(CodeplugCustomDataType_t type, const uint8_t *data, int len)
{
	int i = alloc_slot(type);
	if (i < 0) { return; }
	if (len > (int)sizeof s_blk[i].buf) { len = (int)sizeof s_blk[i].buf; }
	s_blk[i].type = type;
	if (len > 0) { memcpy(s_blk[i].buf, data, (size_t)len); }
	s_blk[i].len = len;
}

void mock_codeplug_clear(void)
{
	memset(s_blk, 0, sizeof s_blk);
	s_writeCount = 0;
}

int mock_codeplug_write_count(void)
{
	return s_writeCount;
}

/* Той самий підпис, що й справжній codeplugSetOpenGD77CustomData() в codeplug.c --
 * реалізація для тестів пише в той самий "флеш"-буфер, що read-функція нижче читає,
 * тож round-trip (set -> reload -> get) із dmrRctlConfigSetEnabled() перевіряється
 * по-справжньому, а не лише мокається на true. */
bool codeplugSetOpenGD77CustomData(CodeplugCustomDataType_t dataType, uint8_t *dataBuf, int len)
{
	int i = alloc_slot(dataType);
	if ((i < 0) || (len < 0) || (len > (int)sizeof s_blk[i].buf)) { return false; }
	s_blk[i].type = dataType;
	if (len > 0) { memcpy(s_blk[i].buf, dataBuf, (size_t)len); }
	s_blk[i].len = len;
	s_writeCount++;
	return true;
}

/* Той самий підпис, що й справжній codeplugGetOpenGD77CustomDataBounded() в codeplug.c:
 * лінкер бере цю реалізацію замість реальної, бо величезний STM32-залежний codeplug.c
 * у тестах свідомо не компілюється. */
bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t dataType, uint8_t *dataBuf, int maxLen)
{
	int i = find_slot(dataType);
	if ((i < 0) || (maxLen <= 0)) { return false; }
	int n = s_blk[i].len;
	if (n > maxLen) { n = maxLen; }
	memcpy(dataBuf, s_blk[i].buf, (size_t)n);
	return true;
}
