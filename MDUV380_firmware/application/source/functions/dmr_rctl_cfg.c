/*
 * dmr_rctl_cfg.c — див. dmr_rctl_cfg.h. Компілюється в ніщо без -DENABLE_AES
 * -DENABLE_DMR_DATA (звичайні/лише-SMS збірки лишаються byte-identical) — дзеркалить
 * умову компіляції dmr_sms.c/.h.
 */
#include "functions/dmr_rctl_cfg.h"

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

#include "functions/codeplug.h"
#include <string.h>

typedef struct
{
	char     magic[4];      /* "RCTL" */
	uint8_t  version;       /* 2 -- 2026-09-03: без allowlist (див. dmr_rctl_pdu.h) */
	uint8_t  enabled;       /* 0 = вимкнено (default/не налаштовано) = ніхто, 1 = увімкнено = будь-хто з канальним ключем */
	uint8_t  reserved[2];   /* про запас/майбутні прапорці, мають бути 0 */
} dmrRctlOnFlashCfg_t;

/* На відміну від MSGC-структури в dmr_sms.c цей блок навмисно НЕ кладемо в CCM RAM
 * (DMR_AES_CCM з dmr_aes.h) — він у рази менший за MSGC (з його 10 текстовими
 * пресетами по 48 байт), тож ризикувати бюджетом CCM заради економії, яку без ПК
 * навіть виміряти нема на чому, сенсу немає. Звичайна статична пам'ять. */
static dmrRctlOnFlashCfg_t s_cfg;
static uint8_t             s_cfgLoaded;

static dmr_rctl_gate_t     s_gate;
static uint8_t             s_gateLoaded;

static void cfg_load(void)
{
	s_cfgLoaded = 1;
	memset(&s_cfg, 0, sizeof s_cfg);
	/* Bounded-читання: dataLength береться з флешу — пошкоджений/чужий блок не
	 * повинен переповнити s_cfg (той самий захист, що й cfg_load() у dmr_sms.c). */
	if (codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, (uint8_t *)&s_cfg, (int)sizeof s_cfg) &&
			(memcmp(s_cfg.magic, "RCTL", 4) == 0))
	{
		return;
	}
	memset(&s_cfg, 0, sizeof s_cfg);   /* відсутній/побитий блок -> fail closed: enabled=0 (ніхто) */
}

static void cfg_ensure(void) { if (!s_cfgLoaded) { cfg_load(); } }

static void gate_load(void)
{
	cfg_ensure();
	/* dmr_rctl_gate_init() скидає кеш anti-replay -- саме те, що треба і при
	 * першому завантаженні, і при примусовому reload(). */
	dmr_rctl_gate_init(&s_gate, s_cfg.enabled);
	s_gateLoaded = 1;
}

dmr_rctl_gate_t *dmrRctlGate(void)
{
	if (!s_gateLoaded) { gate_load(); }
	return &s_gate;
}

void dmrRctlConfigReload(void)
{
	s_cfgLoaded = 0;
	s_gateLoaded = 0;
}

int dmrRctlConfigEnabled(void)
{
	cfg_ensure();
	return (s_cfg.enabled != 0);
}

int dmrRctlConfigSetEnabled(int enabled)
{
	cfg_ensure();

	/* Якщо блоку ще не було (або він побитий) -- cfg_load() вже обнулив s_cfg,
	 * тож magic/version підставляємо тут, так само як dmrAesSetTxKeyId() робить
	 * для блоку "AESK" у crypto/dmr_aes_hook.c. */
	if (memcmp(s_cfg.magic, "RCTL", 4) != 0)
	{
		memset(&s_cfg, 0, sizeof s_cfg);
		memcpy(s_cfg.magic, "RCTL", 4);
		s_cfg.version = 2;
	}
	s_cfg.enabled = enabled ? 1 : 0;

	int ok = codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_CONFIG, (uint8_t *)&s_cfg, (int)sizeof s_cfg) ? 1 : 0;
	dmrRctlConfigReload();   /* негайний ефект: наступний dmrRctlGate()/dmrRctlConfigEnabled() перечитає з флешу */
	return ok;
}

#endif /* ENABLE_DMR_DATA && ENABLE_AES */
