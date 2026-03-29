/* DDNet TAS Patch — tas.cpp */

#include "tas.h"

#include <base/color.h>
#include <base/math.h>
#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>
#include <game/client/gameclient.h>

#include <generated/protocol.h>   // CNetObj_PlayerInput — только в .cpp

// Кол-во int-полей в CNetObj_PlayerInput
static constexpr int TAS_INPUT_INTS = sizeof(CNetObj_PlayerInput) / sizeof(int);
static_assert(TAS_INPUT_INTS == sizeof(STasInput) / sizeof(int),
	"STasInput size mismatch with CNetObj_PlayerInput");

// ════════════════════════════════════════════════════════════
// Конструктор / OnReset
// ════════════════════════════════════════════════════════════

CTas::CTas() = default;

void CTas::OnReset()
{
	if(m_Mode == ETasMode::PLAYBACK)
		m_Mode = ETasMode::IDLE;
	m_PlaybackIndex = 0;
	m_LastGameTick  = -1;
	m_FrameAdvance  = false;
}

// ════════════════════════════════════════════════════════════
// OnConsoleInit
// ════════════════════════════════════════════════════════════

void CTas::OnConsoleInit()
{
	Console()->Register("tas_record",        "",   CFGFLAG_CLIENT, ConTasRecord,       this, "Start TAS recording");
	Console()->Register("tas_stoprecord",    "",   CFGFLAG_CLIENT, ConTasStopRecord,   this, "Stop TAS recording");
	Console()->Register("tas_play",          "",   CFGFLAG_CLIENT, ConTasPlay,         this, "Play back TAS recording from start");
	Console()->Register("tas_stop",          "",   CFGFLAG_CLIENT, ConTasStop,         this, "Stop playback or recording");
	Console()->Register("tas_rewind",        "?i", CFGFLAG_CLIENT, ConTasRewind,       this, "Rewind last N ticks (default 50)");
	Console()->Register("tas_slowmo",        "f",  CFGFLAG_CLIENT, ConTasSlowmo,       this, "Slowmo factor 0.05-1.0");
	Console()->Register("tas_pause",         "",   CFGFLAG_CLIENT, ConTasPause,        this, "Toggle pause");
	Console()->Register("tas_frame_advance", "",   CFGFLAG_CLIENT, ConTasFrameAdvance, this, "Advance 1 tick (while paused)");
	Console()->Register("tas_clear",         "",   CFGFLAG_CLIENT, ConTasClear,        this, "Clear recording");
}

// ════════════════════════════════════════════════════════════
// OnRender — вызывается каждый рендер-фрейм
// ════════════════════════════════════════════════════════════

void CTas::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	const int CurTick = Client()->GameTick(g_Config.m_ClDummy);
	const bool NewTick = (CurTick != m_LastGameTick);
	m_LastGameTick = CurTick;

	if(NewTick)
	{
		if(m_Mode == ETasMode::RECORDING)
		{
			// Копируем текущий инпут в запись
			const CNetObj_PlayerInput &Inp =
				GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy];

			STasFrame Frame;
			Frame.m_Tick              = CurTick;
			Frame.m_Input.m_Direction   = Inp.m_Direction;
			Frame.m_Input.m_TargetX     = Inp.m_TargetX;
			Frame.m_Input.m_TargetY     = Inp.m_TargetY;
			Frame.m_Input.m_Jump        = Inp.m_Jump;
			Frame.m_Input.m_Fire        = Inp.m_Fire;
			Frame.m_Input.m_Hook        = Inp.m_Hook;
			Frame.m_Input.m_PlayerFlags = Inp.m_PlayerFlags;
			Frame.m_Input.m_WantedWeapon= Inp.m_WantedWeapon;
			Frame.m_Input.m_NextWeapon  = Inp.m_NextWeapon;
			Frame.m_Input.m_PrevWeapon  = Inp.m_PrevWeapon;
			m_vFrames.push_back(Frame);
		}
		else if(m_Mode == ETasMode::PLAYBACK)
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
	}

	m_FrameAdvance = false;
	RenderHud();
}

// ════════════════════════════════════════════════════════════
// GetPlaybackInput — вызывается из OnSnapInput
// ════════════════════════════════════════════════════════════

bool CTas::GetPlaybackInput(int *pData) const
{
	if(m_Mode != ETasMode::PLAYBACK)
		return false;
	if(m_PlaybackIndex <= 0 || m_PlaybackIndex > (int)m_vFrames.size())
		return false;

	const STasInput &Inp = m_vFrames[m_PlaybackIndex - 1].m_Input;
	// Накладываем поля напрямую в int-массив (порядок совпадает с CNetObj_PlayerInput)
	pData[0]  = Inp.m_Direction;
	pData[1]  = Inp.m_TargetX;
	pData[2]  = Inp.m_TargetY;
	pData[3]  = Inp.m_Jump;
	pData[4]  = Inp.m_Fire;
	pData[5]  = Inp.m_Hook;
	pData[6]  = Inp.m_PlayerFlags;
	pData[7]  = Inp.m_WantedWeapon;
	pData[8]  = Inp.m_NextWeapon;
	pData[9]  = Inp.m_PrevWeapon;
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
		str_format(aBuf, sizeof(aBuf), "[TAS] REC  frame:%d  slowmo:%.2fx%s",
			(int)m_vFrames.size(), m_Slowmo, m_Paused ? "  PAUSED" : "");
	else if(m_Mode == ETasMode::PLAYBACK)
		str_format(aBuf, sizeof(aBuf), "[TAS] PLAY  %d / %d",
			m_PlaybackIndex, (int)m_vFrames.size());
	else
		str_format(aBuf, sizeof(aBuf), "[TAS] IDLE  frames:%d  (tas_play / tas_record)",
			(int)m_vFrames.size());

	const float FontSize = 6.5f;
	const float X = 5.0f, Y = 5.0f;
	const float TextW = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
	const float Pad = 2.5f;

	// Фон
	ColorRGBA BgColor(0.0f, 0.0f, 0.0f, 0.55f);
	Graphics()->DrawRect(X - Pad, Y - Pad, TextW + Pad * 2.0f, FontSize + Pad * 2.0f,
		BgColor, IGraphics::CORNER_ALL, 2.0f);

	// Текст
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
// Внутренние действия
// ════════════════════════════════════════════════════════════

void CTas::StartRecord()
{
	if(m_Mode == ETasMode::PLAYBACK)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
			"Stop playback first (tas_stop).");
		return;
	}
	m_Mode = ETasMode::RECORDING;
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
		"Recording started. Use tas_stoprecord to finish.");
}

void CTas::StopRecord()
{
	if(m_Mode != ETasMode::RECORDING)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Not recording.");
		return;
	}
	m_Mode = ETasMode::IDLE;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "Recording stopped. %d frames saved.", (int)m_vFrames.size());
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
	m_Mode          = ETasMode::IDLE;
	m_PlaybackIndex = 0;
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

	if(m_Mode == ETasMode::PLAYBACK)
	{
		m_Mode          = ETasMode::IDLE;
		m_PlaybackIndex = 0;
	}

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf),
		"Rewound %d ticks. Frames left: %d. Resume with tas_record.",
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
// Статические обработчики консольных команд
// ════════════════════════════════════════════════════════════

void CTas::ConTasRecord(IConsole::IResult *, void *pUser)
{
	static_cast<CTas *>(pUser)->StartRecord();
}

void CTas::ConTasStopRecord(IConsole::IResult *, void *pUser)
{
	static_cast<CTas *>(pUser)->StopRecord();
}

void CTas::ConTasPlay(IConsole::IResult *, void *pUser)
{
	static_cast<CTas *>(pUser)->StartPlayback();
}

void CTas::ConTasStop(IConsole::IResult *, void *pUser)
{
	static_cast<CTas *>(pUser)->Stop();
}

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

	// Замедление через встроенную переменную демо-плеера.
	// В режиме live-игры эффект достигается через cl_demo_slowmo —
	// она замедляет рендер/тик-аккумулятор клиента.
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "cl_demo_slowmo %.2f", Factor);
	pTas->Console()->ExecuteLine(aBuf, -1);

	char aMsg[64];
	str_format(aMsg, sizeof(aMsg), "Slowmo: %.2fx", Factor);
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", aMsg);
}

void CTas::ConTasClear(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_vFrames.clear();
	pTas->m_Mode          = ETasMode::IDLE;
	pTas->m_PlaybackIndex = 0;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "Recording cleared.");
}

void CTas::ConTasFrameAdvance(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_FrameAdvance = true;
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS", "+1 tick.");
}

void CTas::ConTasPause(IConsole::IResult *, void *pUser)
{
	CTas *pTas = static_cast<CTas *>(pUser);
	pTas->m_Paused = !pTas->m_Paused;
	pTas->Console()->ExecuteLine(pTas->m_Paused ? "cl_demo_slowmo 0.01" : "cl_demo_slowmo 1.0", -1);
	pTas->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "TAS",
		pTas->m_Paused ? "Paused." : "Resumed.");
}
