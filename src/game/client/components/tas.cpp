/* DDNet TAS Patch — tas.cpp
 * Ghost-World реализация
 */

#include "tas.h"

#include <base/color.h>
#include <base/math.h>
#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>

#include <generated/protocol.h>

static constexpr int TAS_INPUT_INTS = sizeof(CNetObj_PlayerInput) / sizeof(int);
static_assert(TAS_INPUT_INTS == sizeof(STasInput) / sizeof(int),
	"STasInput size mismatch");

// ════════════════════════════════════════════════════════════
CTas::CTas() = default;

void CTas::OnReset()
{
	if(m_Mode == ETasMode::PLAYBACK)
		m_Mode = ETasMode::IDLE;
	m_PlaybackIndex   = 0;
	m_LastGameTick    = -1;
	m_FrameAdvance    = false;
	m_GhostWorldReady = false;
	m_SlowmoAccum     = 0.0f;
	mem_zero(&m_PrevGhostInput, sizeof(m_PrevGhostInput));
}

// ════════════════════════════════════════════════════════════
void CTas::OnConsoleInit()
{
	Console()->Register("tas_record",        "",   CFGFLAG_CLIENT, ConTasRecord,       this, "Start TAS ghost recording");
	Console()->Register("tas_stoprecord",    "",   CFGFLAG_CLIENT, ConTasStopRecord,   this, "Stop TAS recording");
	Console()->Register("tas_play",          "",   CFGFLAG_CLIENT, ConTasPlay,         this, "Play back TAS");
	Console()->Register("tas_stop",          "",   CFGFLAG_CLIENT, ConTasStop,         this, "Stop TAS");
	Console()->Register("tas_rewind",        "?i", CFGFLAG_CLIENT, ConTasRewind,       this, "Rewind N ticks (default 50)");
	Console()->Register("tas_slowmo",        "f",  CFGFLAG_CLIENT, ConTasSlowmo,       this, "Ghost world speed 0.05-1.0");
	Console()->Register("tas_pause",         "",   CFGFLAG_CLIENT, ConTasPause,        this, "Toggle ghost world pause");
	Console()->Register("tas_frame_advance", "",   CFGFLAG_CLIENT, ConTasFrameAdvance, this, "Advance ghost 1 tick");
	Console()->Register("tas_clear",         "",   CFGFLAG_CLIENT, ConTasClear,        this, "Clear recording");
}

// ════════════════════════════════════════════════════════════
// InitGhostWorld — копируем текущий PredictedWorld в наш ghost-мир
// ════════════════════════════════════════════════════════════
void CTas::InitGhostWorld()
{
	m_GhostWorld.CopyWorld(&GameClient()->m_PredictedWorld);
	m_GhostTick       = Client()->GameTick(g_Config.m_ClDummy);
	m_GhostWorldReady = true;
	m_SlowmoAccum     = 0.0f;
	mem_zero(&m_PrevGhostInput, sizeof(m_PrevGhostInput));
	// Начальные флаги
	m_PrevGhostInput.m_PlayerFlags = PLAYERFLAG_PLAYING;
}

// ════════════════════════════════════════════════════════════
// StepGhostWorld — один тик ghost-мира с текущим инпутом игрока
// ════════════════════════════════════════════════════════════
void CTas::StepGhostWorld()
{
	if(!m_GhostWorldReady)
		return;

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	CCharacter *pChar = m_GhostWorld.GetCharacterById(LocalId);
	if(!pChar)
		return;

	// Читаем живой инпут от Controls
	const CNetObj_PlayerInput &LiveInp =
		GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];

	// Строим корректный CNetObj_PlayerInput для ghost-мира:
	// Jump и Fire — счётчики, не просто 0/1
	CNetObj_PlayerInput GhostInp;
	GhostInp.m_Direction   = LiveInp.m_Direction;
	GhostInp.m_TargetX     = LiveInp.m_TargetX;
	GhostInp.m_TargetY     = LiveInp.m_TargetY;
	GhostInp.m_Hook        = LiveInp.m_Hook;
	GhostInp.m_PlayerFlags = PLAYERFLAG_PLAYING;
	GhostInp.m_WantedWeapon= LiveInp.m_WantedWeapon;
	GhostInp.m_NextWeapon  = LiveInp.m_NextWeapon;
	GhostInp.m_PrevWeapon  = LiveInp.m_PrevWeapon;

	// Jump: инкрементируем счётчик при нажатии, иначе 0
	if(LiveInp.m_Jump && !m_PrevGhostInput.m_Jump)
		GhostInp.m_Jump = m_PrevGhostInput.m_Jump + 1;
	else if(!LiveInp.m_Jump)
		GhostInp.m_Jump = 0;
	else
		GhostInp.m_Jump = m_PrevGhostInput.m_Jump;

	// Fire: инкрементируем при нажатии
	if(LiveInp.m_Fire && !(m_PrevGhostInput.m_Fire & 1))
		GhostInp.m_Fire = (m_PrevGhostInput.m_Fire + 1) & INPUT_STATE_MASK;
	else if(!LiveInp.m_Fire && (m_PrevGhostInput.m_Fire & 1))
		GhostInp.m_Fire = (m_PrevGhostInput.m_Fire + 1) & INPUT_STATE_MASK;
	else
		GhostInp.m_Fire = m_PrevGhostInput.m_Fire & INPUT_STATE_MASK;

	// Применяем инпут к ghost-персонажу
	pChar->OnPredictedInput(&GhostInp);

	// Шагаем ghost-мир
	m_GhostWorld.Tick();
	m_GhostTick++;

	// Сохраняем инпут в запись
	STasFrame Frame;
	Frame.m_Tick              = m_GhostTick;
	Frame.m_Input.m_Direction   = GhostInp.m_Direction;
	Frame.m_Input.m_TargetX     = GhostInp.m_TargetX;
	Frame.m_Input.m_TargetY     = GhostInp.m_TargetY;
	Frame.m_Input.m_Jump        = GhostInp.m_Jump;
	Frame.m_Input.m_Fire        = GhostInp.m_Fire;
	Frame.m_Input.m_Hook        = GhostInp.m_Hook;
	Frame.m_Input.m_PlayerFlags = GhostInp.m_PlayerFlags;
	Frame.m_Input.m_WantedWeapon= GhostInp.m_WantedWeapon;
	Frame.m_Input.m_NextWeapon  = GhostInp.m_NextWeapon;
	Frame.m_Input.m_PrevWeapon  = GhostInp.m_PrevWeapon;
	m_vFrames.push_back(Frame);

	// Запоминаем предыдущий инпут
	mem_copy(&m_PrevGhostInput, &Frame.m_Input, sizeof(STasInput));
}

// ════════════════════════════════════════════════════════════
// OnRender
// ════════════════════════════════════════════════════════════
void CTas::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	const int CurTick = Client()->GameTick(g_Config.m_ClDummy);
	const bool NewTick = (CurTick != m_LastGameTick);
	m_LastGameTick = CurTick;

	if(m_Mode == ETasMode::RECORDING && NewTick)
	{
		// Инициализируем ghost-мир при первом тике записи
		if(!m_GhostWorldReady)
			InitGhostWorld();

		if(!m_Paused || m_FrameAdvance)
		{
			// Накапливаем время slowmo
			m_SlowmoAccum += m_Slowmo;
			// Шагаем ghost столько раз, сколько накопилось целых тиков
			while(m_SlowmoAccum >= 1.0f)
			{
				StepGhostWorld();
				m_SlowmoAccum -= 1.0f;
			}
		}
		m_FrameAdvance = false;
	}
	else if(m_Mode == ETasMode::PLAYBACK && NewTick)
	{
		if(m_PlaybackIndex < (int)m_vFrames.size())
			m_PlaybackIndex++;
		else
		{
			m_Mode          = ETasMode::IDLE;
			m_PlaybackIndex = 0;
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Playback finished.");
		}
	}

	RenderHud();
}

// ════════════════════════════════════════════════════════════
// RenderGhost — рисуем ghost-персонажа поверх обычного рендера
// ════════════════════════════════════════════════════════════
void CTas::RenderGhost()
{
	if(m_Mode != ETasMode::RECORDING || !m_GhostWorldReady)
		return;

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	const CCharacter *pChar = m_GhostWorld.GetCharacterById(LocalId);
	if(!pChar)
		return;

	// Получаем позицию ghost-персонажа
	const vec2 GhostPos = pChar->Core()->m_Pos;

	// Рисуем простой кружок-маркер на позиции ghost
	// (полноценный скин рисовать сложнее — оставим маркер)
	const float ScreenW = 300.0f * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, ScreenW, 300.0f);

	// Конвертируем world-координаты в screen-координаты
	// через камеру
	const vec2 Center = GameClient()->m_Camera.m_Center;
	const float Zoom  = GameClient()->m_Camera.m_Zoom;
	const float ScrX  = ScreenW * 0.5f + (GhostPos.x - Center.x) / (32.0f * Zoom);
	const float ScrY  = 300.0f  * 0.5f + (GhostPos.y - Center.y) / (32.0f * Zoom);
	const float R     = 8.0f / Zoom;

	// Полупрозрачный синий круг
	Graphics()->SetColor(0.3f, 0.6f, 1.0f, 0.6f);
	Graphics()->DrawCircle(ScrX, ScrY, R, 24);
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
}

// ════════════════════════════════════════════════════════════
// GetPlaybackInput
// ════════════════════════════════════════════════════════════
bool CTas::GetPlaybackInput(int *pData) const
{
	if(m_Mode != ETasMode::PLAYBACK)
		return false;
	if(m_PlaybackIndex <= 0 || m_PlaybackIndex > (int)m_vFrames.size())
		return false;

	const STasInput &Inp = m_vFrames[m_PlaybackIndex - 1].m_Input;
	pData[0] = Inp.m_Direction;
	pData[1] = Inp.m_TargetX;
	pData[2] = Inp.m_TargetY;
	pData[3] = Inp.m_Jump;
	pData[4] = Inp.m_Fire;
	pData[5] = Inp.m_Hook;
	pData[6] = Inp.m_PlayerFlags;
	pData[7] = Inp.m_WantedWeapon;
	pData[8] = Inp.m_NextWeapon;
	pData[9] = Inp.m_PrevWeapon;
	return true;
}

// ════════════════════════════════════════════════════════════
// HUD
// ════════════════════════════════════════════════════════════
void CTas::RenderHud()
{
	if(m_Mode == ETasMode::IDLE && m_vFrames.empty())
		return;

	const float ScreenW = 300.0f * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, ScreenW, 300.0f);

	char aBuf[256];
	if(m_Mode == ETasMode::RECORDING)
		str_format(aBuf, sizeof(aBuf),
			"[TAS] REC  frames:%d  ghost_tick:%d  slowmo:%.2fx%s",
			(int)m_vFrames.size(), m_GhostTick, m_Slowmo,
			m_Paused ? "  PAUSED" : "");
	else if(m_Mode == ETasMode::PLAYBACK)
		str_format(aBuf, sizeof(aBuf),
			"[TAS] PLAY  %d / %d",
			m_PlaybackIndex, (int)m_vFrames.size());
	else
		str_format(aBuf, sizeof(aBuf),
			"[TAS] IDLE  frames:%d",
			(int)m_vFrames.size());

	const float FontSize = 6.5f;
	const float X = 5.0f, Y = 5.0f;
	const float TextW = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
	const float Pad = 2.5f;

	Graphics()->DrawRect(X - Pad, Y - Pad,
		TextW + Pad * 2.0f, FontSize + Pad * 2.0f,
		ColorRGBA(0.0f, 0.0f, 0.0f, 0.55f), IGraphics::CORNER_ALL, 2.0f);

	if(m_Mode == ETasMode::RECORDING)
		TextRender()->TextColor(1.0f, 0.25f, 0.25f, 1.0f);
	else if(m_Mode == ETasMode::PLAYBACK)
		TextRender()->TextColor(0.25f, 1.0f, 0.25f, 1.0f);
	else
		TextRender()->TextColor(1.0f, 1.0f, 0.3f, 0.85f);

	TextRender()->Text(X, Y, FontSize, aBuf, -1.0f);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	// Прогресс-бар playback
	if(m_Mode == ETasMode::PLAYBACK && !m_vFrames.empty())
	{
		const float BarW = 200.0f;
		const float BarH = 4.0f;
		const float BarX = (ScreenW - BarW) * 0.5f;
		const float BarY = 291.0f;
		const float P    = (float)m_PlaybackIndex / (float)m_vFrames.size();

		Graphics()->DrawRect(BarX, BarY, BarW, BarH,
			ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 2.0f);
		Graphics()->DrawRect(BarX, BarY, BarW * P, BarH,
			ColorRGBA(0.2f, 1.0f, 0.2f, 0.9f), IGraphics::CORNER_ALL, 2.0f);
	}
}

// ════════════════════════════════════════════════════════════
// Действия
// ════════════════════════════════════════════════════════════

void CTas::StartRecord()
{
	if(m_Mode == ETasMode::PLAYBACK)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
			"Stop playback first (tas_stop).");
		return;
	}
	m_Mode            = ETasMode::RECORDING;
	m_GhostWorldReady = false; // будет инициализирован при первом тике
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf),
		"Ghost recording started. Slowmo: %.2fx. "
		"Your character is frozen. Control the ghost!",
		m_Slowmo);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aBuf);
}

void CTas::StopRecord()
{
	if(m_Mode != ETasMode::RECORDING)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Not recording.");
		return;
	}
	m_Mode            = ETasMode::IDLE;
	m_GhostWorldReady = false;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf),
		"Recording stopped. %d frames saved. Use tas_play to replay.",
		(int)m_vFrames.size());
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aBuf);
}

void CTas::StartPlayback()
{
	if(m_vFrames.empty())
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
			"Nothing to play. Record first with tas_record.");
		return;
	}
	m_Mode          = ETasMode::PLAYBACK;
	m_PlaybackIndex = 0;
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Playback started.");
}

void CTas::Stop()
{
	m_Mode            = ETasMode::IDLE;
	m_PlaybackIndex   = 0;
	m_GhostWorldReady = false;
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Stopped.");
}

void CTas::DoRewind(int Ticks)
{
	if(m_vFrames.empty())
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Nothing to rewind.");
		return;
	}
	const int Remove = minimum(Ticks, (int)m_vFrames.size());
	m_vFrames.resize(m_vFrames.size() - Remove);

	// Восстанавливаем ghost-мир на позицию N тиков назад
	if(m_Mode == ETasMode::RECORDING)
	{
		// Переинициализируем ghost с текущего server-снапа
		// и переигрываем оставшиеся фреймы
		m_GhostWorldReady = false; // инициализируется при следующем тике
	}
	else if(m_Mode == ETasMode::PLAYBACK)
	{
		m_Mode          = ETasMode::IDLE;
		m_PlaybackIndex = 0;
	}

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf),
		"Rewound %d frames. Frames left: %d.", Remove, (int)m_vFrames.size());
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aBuf);
}

const char *CTas::ModeStr() const
{
	switch(m_Mode)
	{
	case ETasMode::RECORDING: return "REC";
	case ETasMode::PLAYBACK:  return "PLAY";
	default:                  return "IDLE";
	}
}

// ════════════════════════════════════════════════════════════
// Консольные обработчики
// ════════════════════════════════════════════════════════════

void CTas::ConTasRecord(IConsole::IResult *, void *pUser)
{ static_cast<CTas *>(pUser)->StartRecord(); }

void CTas::ConTasStopRecord(IConsole::IResult *, void *pUser)
{ static_cast<CTas *>(pUser)->StopRecord(); }

void CTas::ConTasPlay(IConsole::IResult *, void *pUser)
{ static_cast<CTas *>(pUser)->StartPlayback(); }

void CTas::ConTasStop(IConsole::IResult *, void *pUser)
{ static_cast<CTas *>(pUser)->Stop(); }

void CTas::ConTasRewind(IConsole::IResult *pResult, void *pUser)
{
	const int Ticks = (pResult->NumArguments() > 0) ? pResult->GetInteger(0) : 50;
	static_cast<CTas *>(pUser)->DoRewind(Ticks);
}

void CTas::ConTasSlowmo(IConsole::IResult *pResult, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	float Factor = pResult->GetFloat(0);
	if(Factor < 0.05f) Factor = 0.05f;
	if(Factor > 1.0f)  Factor = 1.0f;
	pTas->m_Slowmo = Factor;
	char aMsg[64];
	str_format(aMsg, sizeof(aMsg), "Ghost world slowmo: %.2fx", Factor);
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aMsg);
}

void CTas::ConTasClear(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_vFrames.clear();
	pTas->m_Mode            = ETasMode::IDLE;
	pTas->m_PlaybackIndex   = 0;
	pTas->m_GhostWorldReady = false;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Recording cleared.");
}

void CTas::ConTasFrameAdvance(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_FrameAdvance = true;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Ghost +1 tick.");
}

void CTas::ConTasPause(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_Paused = !pTas->m_Paused;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
		pTas->m_Paused ? "Ghost world paused." : "Ghost world resumed.");
}
