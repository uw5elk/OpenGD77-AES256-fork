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

/* ===================== СТАН anti-replay у ФЛЕШІ ==========================
 *
 * НАВІЩО (знайдено аудитом 2026-09-04). Кеш anti-replay жив лише в ОЗП і обнулявся при
 * КОЖНОМУ ввімкненні рації. Тому той, хто просто ЗАПИСАВ з ефіру зашифрований кадр
 * CHECK_REQ (ключ для цього не потрібен), міг відтворити запис після перезавантаження
 * цілі: кеш порожній -> кадр проходить як від "нового видавця" -> рація АВТОМАТИЧНО
 * виходить в ефір з відповіддю. Для тактичної мережі це примусове демаскування,
 * придатне для пеленгації.
 *
 * ЧОМУ ЗБЕРІГАЄТЬСЯ Й ЛІЧИЛЬНИК ВІДПРАВКИ. Зберегти лише приймальні лічильники було б
 * гірше за хворобу. Відправник сіяв свій seq із ticksGetMillis(), тобто від МОМЕНТУ
 * ВВІМКНЕННЯ. Якби ціль пам'ятала високий lastSeq через перезавантаження, а запитувач
 * після СВОГО перезавантаження починав з малого числа -- його команди відхилялися б
 * назавжди. Дірка в безпеці перетворилась би на повну відмову зв'язку. Монотонним через
 * перезавантаження має бути КОЖЕН бік.
 *
 * ЗНОС ФЛЕША. Номери відправки видаються ПАЧКАМИ: у флеш пишеться верхня межа діапазону,
 * а всередині діапазону номери роздаються з ОЗП без записів. Один запис на
 * DMR_RCTL_TXSEQ_RESERVE команд замість запису на кожну. Після перезавантаження
 * продовжуємо з записаної межі, пропускаючи невикористані номери -- монотонність
 * гарантована навіть при раптовому знеструмленні.
 *
 * ОКРЕМИЙ БЛОК від "RCTL": той пише CHIRP/ПК, а цей веде сама прошивка. В одному блоці
 * запис із ПК затирав би лічильники захисту. */
#define DMR_RCTL_TXSEQ_RESERVE  64u

typedef struct
{
	char     magic[4];      /* "RCTS" */
	uint8_t  version;       /* 1 */
	uint8_t  used;          /* скільки слотів кеша зайнято */
	uint8_t  nextEvict;     /* курсор витіснення по колу */
	uint8_t  reserved;      /* має бути 0 */
	uint32_t txSeqReserved; /* усе <= цього числа вважаємо ВЖЕ використаним для відправки */
	uint32_t issuerId[DMR_RCTL_REPLAY_CACHE];
	uint32_t lastSeq[DMR_RCTL_REPLAY_CACHE];
} dmrRctlOnFlashState_t;

static dmrRctlOnFlashState_t s_state;
static uint8_t               s_stateLoaded;
static uint32_t              s_txSeq;        /* останній ВИДАНИЙ номер */
static uint32_t              s_txSeqLimit;   /* до цього включно можна видавати без запису */

static void state_blank(void)
{
	memset(&s_state, 0, sizeof s_state);
	memcpy(s_state.magic, "RCTS", 4);
	s_state.version = 1;
}

static void state_load(void)
{
	s_stateLoaded = 1;
	memset(&s_state, 0, sizeof s_state);

	if (!(codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_STATE,
			(uint8_t *)&s_state, (int)sizeof s_state) &&
			(memcmp(s_state.magic, "RCTS", 4) == 0) && (s_state.version == 1) &&
			(s_state.used <= DMR_RCTL_REPLAY_CACHE) &&
			(s_state.nextEvict < DMR_RCTL_REPLAY_CACHE)))
	{
		state_blank();   /* відсутній/побитий блок -> порожній кеш, лічильник з нуля */
	}

	s_txSeq = s_state.txSeqReserved;
	s_txSeqLimit = s_state.txSeqReserved;
}

static void state_ensure(void) { if (!s_stateLoaded) { state_load(); } }

static int state_save(void)
{
	return codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_RCTL_STATE,
			(uint8_t *)&s_state, (int)sizeof s_state) ? 1 : 0;
}

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
	state_ensure();
	dmr_rctl_gate_init(&s_gate, s_cfg.enabled);

	/* І ОДРАЗУ відновлюємо збережені лічильники. Саме цей рядок закриває replay після
	 * перезавантаження: до 2026-09-04 кеш лишався порожнім, і перший же відтворений
	 * кадр проходив як від "нового видавця". */
	s_gate.used = s_state.used;
	s_gate.nextEvict = s_state.nextEvict;
	for (uint8_t i = 0; i < DMR_RCTL_REPLAY_CACHE; i++)
	{
		s_gate.issuerId[i] = s_state.issuerId[i];
		s_gate.lastSeq[i] = s_state.lastSeq[i];
	}

	s_gateLoaded = 1;
}

int dmrRctlGatePersist(void)
{
	if (!s_gateLoaded) { return 0; }
	state_ensure();

	s_state.used = s_gate.used;
	s_state.nextEvict = s_gate.nextEvict;
	for (uint8_t i = 0; i < DMR_RCTL_REPLAY_CACHE; i++)
	{
		s_state.issuerId[i] = s_gate.issuerId[i];
		s_state.lastSeq[i] = s_gate.lastSeq[i];
	}
	return state_save();
}

uint32_t dmrRctlNextTxSeq(void)
{
	state_ensure();

	if (s_txSeq >= s_txSeqLimit)
	{
		/* Резервуємо наступну пачку. Межу піднімаємо ЛИШЕ після успішного запису:
		 * інакше після перезавантаження ми б видали ті самі номери вдруге, і власні
		 * команди виглядали б для цілі як повтор -- тобто відхилялись. */
		uint32_t want = s_txSeq + DMR_RCTL_TXSEQ_RESERVE;

		s_state.txSeqReserved = want;
		if (!state_save())
		{
			s_state.txSeqReserved = s_txSeqLimit;   /* відкотити -- у флеші лишилось старе */
			return 0;                               /* 0 = номер не видано, відправку скасувати */
		}
		s_txSeqLimit = want;
	}

	return ++s_txSeq;
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
	/* Стан теж перечитуємо: саме так тест (і реальне ввімкнення рації) відтворює
	 * "перезавантаження" -- ОЗП чисте, флеш лишився. */
	s_stateLoaded = 0;
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
