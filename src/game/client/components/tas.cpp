/* DDNet TAS — Ghost World реализация (исправленная)
 *
 * Исправления:
 *   1. Ghost рендерится через RenderPlayer/RenderHook (как настоящий тee)
 *   2. Jump/Fire — правильные счётчики (не просто 0/1)
 *   3. Основной персонаж заморожен во время записи
 *   4. Ghost-мир строится из CNetObj_Character снапа (без телепортации)
 */

#include "tas.h"

#include <base/color.h>
#include <base/math.h>
#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>
#include <game/client/components/controls.h>
#include <game/client/components/players.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/render.h>

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
	mem_zero(&m_GhostCharCur, sizeof(m_GhostCharCur));
	mem_zero(&m_GhostCharPrev, sizeof(m_GhostCharPrev));
}

// ════════════════════════════════════════════════════════════
void CTas::OnConsoleInit()
{
	Console()->Register("tas_record",        "",   CFGFLAG_CLIENT, ConTasRecord,       this, "Start TAS ghost recording");
	Console()->Register("tas_stoprecord",    "",   CFGFLAG_CLIENT, ConTasStopRecord,   this, "Stop TAS recording");
	Console()->Register("tas_play",          "",   CFGFLAG_CLIENT, ConTasPlay,         this, "Play back TAS");
	Console()->Register("tas_stop",          "",   CFGFLAG_CLIENT, ConTasStop,         this, "Stop TAS");
	Console()->Register("tas_rewind",        "?i", CFGFLAG_CLIENT, ConTasRewind,       this, "Rewind N ticks (default 50)");
	Console()->Register("tas_slowmo",        "f",  CFGFLAG_CLIENT, ConTasSlowmo,       this, "Ghost world speed 0.05-1.0 (default 0.25)");
	Console()->Register("tas_pause",         "",   CFGFLAG_CLIENT, ConTasPause,        this, "Toggle ghost world pause");
	Console()->Register("tas_frame_advance", "",   CFGFLAG_CLIENT, ConTasFrameAdvance, this, "Advance ghost 1 tick");
	Console()->Register("tas_clear",         "",   CFGFLAG_CLIENT, ConTasClear,        this, "Clear recording");
}

// ════════════════════════════════════════════════════════════
// Конвертируем состояние CCharacter из ghost-мира в CNetObj_Character
// для рендера через стандартный RenderPlayer
// ════════════════════════════════════════════════════════════
static void CharToNetObj(const CCharacter *pChar, CNetObj_Character *pOut)
{
	const CCharacterCore *pCore = pChar->Core();
	mem_zero(pOut, sizeof(*pOut));
	pOut->m_X          = (int)pCore->m_Pos.x;
	pOut->m_Y          = (int)pCore->m_Pos.y;
	pOut->m_VelX       = (int)(pCore->m_Vel.x * 256.0f);
	pOut->m_VelY       = (int)(pCore->m_Vel.y * 256.0f);
	pOut->m_Angle      = (int)(pCore->m_Angle * 256.0f);
	pOut->m_Direction  = pCore->m_Direction;
	pOut->m_Jumped     = pCore->m_Jumped;
	pOut->m_HookedPlayer = pCore->HookedPlayer();
	pOut->m_HookState  = pCore->m_HookState;
	pOut->m_HookX      = (int)pCore->m_HookPos.x;
	pOut->m_HookY      = (int)pCore->m_HookPos.y;
	pOut->m_HookDx     = (int)(pCore->m_HookDir.x * 256.0f);
	pOut->m_HookDy     = (int)(pCore->m_HookDir.y * 256.0f);
	pOut->m_Weapon     = pChar->GetActiveWeapon();
	pOut->m_PlayerFlags= PLAYERFLAG_PLAYING;
}

// ════════════════════════════════════════════════════════════
// InitGhostWorld — копируем snapshot в ghost-мир
// ════════════════════════════════════════════════════════════
void CTas::InitGhostWorld()
{
	// Копируем предсказательный мир
	m_GhostWorld.CopyWorld(&GameClient()->m_PredictedWorld);
	m_GhostTick       = Client()->GameTick(g_Config.m_ClDummy);
	m_GhostWorldReady = true;
	m_SlowmoAccum     = 0.0f;
	mem_zero(&m_PrevGhostInput, sizeof(m_PrevGhostInput));
	m_PrevGhostInput.m_PlayerFlags = PLAYERFLAG_PLAYING;

	// Инициализируем отображаемые данные ghost
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(const CCharacter *pChar = m_GhostWorld.GetCharacterById(LocalId))
	{
		CharToNetObj(pChar, &m_GhostCharCur);
		m_GhostCharPrev = m_GhostCharCur;
	}
}

// ════════════════════════════════════════════════════════════
// StepGhostWorld — один тик ghost-мира
// ════════════════════════════════════════════════════════════
void CTas::StepGhostWorld()
{
	if(!m_GhostWorldReady)
		return;

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	CCharacter *pChar = m_GhostWorld.GetCharacterById(LocalId);
	if(!pChar)
		return;

	// Читаем живой инпут
	const CNetObj_PlayerInput &LiveInp =
		GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];

	// Собираем инпут с правильными счётчиками
	CNetObj_PlayerInput GhostInp;
	mem_zero(&GhostInp, sizeof(GhostInp));
	GhostInp.m_Direction    = LiveInp.m_Direction;
	GhostInp.m_TargetX      = LiveInp.m_TargetX;
	GhostInp.m_TargetY      = LiveInp.m_TargetY;
	GhostInp.m_Hook         = LiveInp.m_Hook;
	GhostInp.m_PlayerFlags  = PLAYERFLAG_PLAYING;
	GhostInp.m_WantedWeapon = LiveInp.m_WantedWeapon;
	GhostInp.m_NextWeapon   = LiveInp.m_NextWeapon;
	GhostInp.m_PrevWeapon   = LiveInp.m_PrevWeapon;

	// Jump — инкрементируем счётчик при каждом новом нажатии
	bool JumpPressed  = LiveInp.m_Jump != 0;
	bool WasJumping   = m_PrevGhostInput.m_Jump != 0;
	if(JumpPressed && !WasJumping)
		GhostInp.m_Jump = m_PrevGhostInput.m_Jump + 1;
	else if(!JumpPressed)
		GhostInp.m_Jump = 0;
	else
		GhostInp.m_Jump = m_PrevGhostInput.m_Jump;

	// Fire — инкрементируем при смене состояния (0→1 и 1→0)
	bool FirePressed  = (LiveInp.m_Fire & 1) != 0;
	bool WasFiring    = (m_PrevGhostInput.m_Fire & 1) != 0;
	if(FirePressed != WasFiring)
		GhostInp.m_Fire = (m_PrevGhostInput.m_Fire + 1) & INPUT_STATE_MASK;
	else
		GhostInp.m_Fire = m_PrevGhostInput.m_Fire & INPUT_STATE_MASK;

	// Сохраняем предыдущий кадр для рендера (интерполяция)
	m_GhostCharPrev = m_GhostCharCur;

	// Применяем инпут и шагаем мир
	pChar->OnPredictedInput(&GhostInp);
	m_GhostWorld.Tick();
	m_GhostTick++;

	// Обновляем текущее состояние ghost для рендера
	if(const CCharacter *pCharNew = m_GhostWorld.GetCharacterById(LocalId))
		CharToNetObj(pCharNew, &m_GhostCharCur);

	// Записываем тик
	STasFrame Frame;
	Frame.m_Tick                = m_GhostTick;
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

	// Запоминаем инпут для следующего тика
	Frame.m_Input.m_Jump = GhostInp.m_Jump;
	Frame.m_Input.m_Fire = GhostInp.m_Fire;
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
		if(!m_GhostWorldReady)
			InitGhostWorld();

		if(!m_Paused || m_FrameAdvance)
		{
			m_SlowmoAccum += m_Slowmo;
			while(m_SlowmoAccum >= 1.0f)
			{
				StepGhostWorld();
				m_SlowmoAccum -= 1.0f;
			}
		}
		m_FrameAdvance = false;

		// Рендерим ghost поверх игры
		RenderGhost();
	}
	else if(m_Mode == ETasMode::PLAYBACK && NewTick)
	{
		if(m_PlaybackIndex < (int)m_vFrames.size())
			m_PlaybackIndex++;
		else
		{
			m_Mode          = ETasMode::IDLE;
			m_PlaybackIndex = 0;
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
				"Playback finished.");
		}
	}

	RenderHud();
}

// ════════════════════════════════════════════════════════════
// RenderGhost — рисуем ghost-tee через стандартный RenderPlayer
// ════════════════════════════════════════════════════════════
void CTas::RenderGhost()
{
	if(!m_GhostWorldReady)
		return;

	// Берём RenderInfo локального игрока и делаем его полупрозрачным
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return;

	CTeeRenderInfo GhostRenderInfo = GameClient()->m_aClients[LocalId].m_RenderInfo;
	// Синеватый оттенок + полупрозрачность для ghost
	GhostRenderInfo.m_ColorBody  = ColorRGBA(0.4f, 0.6f, 1.0f, 0.7f);
	GhostRenderInfo.m_ColorFeet  = ColorRGBA(0.3f, 0.5f, 0.9f, 0.7f);
	GhostRenderInfo.m_CustomColoredSkin = true;

	// ClientId = -2 означает ghost (как в системе race ghost)
	GameClient()->m_Players.RenderHook(
		&m_GhostCharPrev, &m_GhostCharCur,
		&GhostRenderInfo, -2,
		Client()->IntraGameTick(g_Config.m_ClDummy));

	GameClient()->m_Players.RenderPlayer(
		&m_GhostCharPrev, &m_GhostCharCur,
		&GhostRenderInfo, -2,
		Client()->IntraGameTick(g_Config.m_ClDummy));
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
			"[TAS] REC  frames:%d  slowmo:%.2fx%s",
			(int)m_vFrames.size(), m_Slowmo,
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
		TextRender()->TextColor(1.0f, 0.3f, 0.3f, 1.0f);
	else if(m_Mode == ETasMode::PLAYBACK)
		TextRender()->TextColor(0.3f, 1.0f, 0.3f, 1.0f);
	else
		TextRender()->TextColor(1.0f, 1.0f, 0.3f, 0.9f);

	TextRender()->Text(X, Y, FontSize, aBuf, -1.0f);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

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
	m_GhostWorldReady = false;
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf),
		"Ghost recording started. Slowmo: %.2fx. "
		"Your char is frozen — move the blue ghost!",
		m_Slowmo);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aBuf);
}

void CTas::StopRecord()
{
	if(m_Mode != ETasMode::RECORDING)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
			"Not recording.");
		return;
	}
	m_Mode            = ETasMode::IDLE;
	m_GhostWorldReady = false;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf),
		"Recording stopped. %d frames. Use tas_play.",
		(int)m_vFrames.size());
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aBuf);
}

void CTas::StartPlayback()
{
	if(m_vFrames.empty())
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
			"Nothing to play. Use tas_record first.");
		return;
	}
	m_Mode          = ETasMode::PLAYBACK;
	m_PlaybackIndex = 0;
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
		"Playback started.");
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
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
			"Nothing to rewind.");
		return;
	}
	const int Remove = minimum(Ticks, (int)m_vFrames.size());
	m_vFrames.resize(m_vFrames.size() - Remove);

	if(m_Mode == ETasMode::RECORDING)
		m_GhostWorldReady = false; // переинициализируется при следующем тике
	else if(m_Mode == ETasMode::PLAYBACK)
	{
		m_Mode          = ETasMode::IDLE;
		m_PlaybackIndex = 0;
	}

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf),
		"Rewound %d frames. Left: %d.",
		Remove, (int)m_vFrames.size());
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
	const int Ticks = pResult->NumArguments() > 0
		? pResult->GetInteger(0) : 50;
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
	str_format(aMsg, sizeof(aMsg), "Ghost slowmo: %.2fx", Factor);
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aMsg);
}

void CTas::ConTasClear(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_vFrames.clear();
	pTas->m_Mode            = ETasMode::IDLE;
	pTas->m_PlaybackIndex   = 0;
	pTas->m_GhostWorldReady = false;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
		"Recording cleared.");
}

void CTas::ConTasFrameAdvance(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_FrameAdvance = true;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
		"Ghost +1 tick.");
}

void CTas::ConTasPause(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_Paused = !pTas->m_Paused;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
		pTas->m_Paused ? "Ghost paused." : "Ghost resumed.");
}
