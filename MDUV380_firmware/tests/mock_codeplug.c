/* mock_codeplug.c — див. mock_codeplug.h. */
#include "mock_codeplug.h"
#include <string.h>

static CodeplugCustomDataType_t s_type = CODEPLUG_CUSTOM_DATA_TYPE_EMPTY;
static uint8_t                  s_buf[256];
static int                      s_len = 0;
static int                      s_writeCount = 0;

void mock_codeplug_set_block(CodeplugCustomDataType_t type, const uint8_t *data, int len)
{
	s_type = type;
	if (len > (int)sizeof s_buf) { len = (int)sizeof s_buf; }
	if (len > 0) { memcpy(s_buf, data, (size_t)len); }
	s_len = len;
}

void mock_codeplug_clear(void)
{
	s_type = CODEPLUG_CUSTOM_DATA_TYPE_EMPTY;
	s_len = 0;
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
	if (len < 0 || len > (int)sizeof s_buf) { return false; }
	s_type = dataType;
	if (len > 0) { memcpy(s_buf, dataBuf, (size_t)len); }
	s_len = len;
	s_writeCount++;
	return true;
}

/* Той самий підпис, що й справжній codeplugGetOpenGD77CustomDataBounded() в codeplug.c
 * (application/include/functions/codeplug.h) — лінкер бере цю реалізацію замість
 * реальної, бо ми свідомо не компілюємо величезний STM32-залежний codeplug.c в тестах. */
bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t dataType, uint8_t *dataBuf, int maxLen)
{
	if ((s_len == 0) || (dataType != s_type) || (maxLen <= 0)) { return false; }
	int n = s_len;
	if (n > maxLen) { n = maxLen; }
	memcpy(dataBuf, s_buf, (size_t)n);
	return true;
}
