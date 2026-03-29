/* DDNet TAS Patch — tas.h
 *
 * Компонент Tool-Assisted Speedrun для DDraceNetwork.
 *
 * Команды:
 *   tas_record          — начать запись
 *   tas_stoprecord      — остановить запись
 *   tas_play            — воспроизвести с начала
 *   tas_stop            — стоп
 *   tas_rewind [N]      — стереть последние N тиков (по умолч. 50)
 *   tas_slowmo <0.05–1> — замедление
 *   tas_pause           — пауза / возобновление
 *   tas_frame_advance   — 1 тик вперёд (в паузе)
 *   tas_clear           — очистить запись
 *
 * Рекомендуемые биндинги (autoexec.cfg):
 *   bind f4  tas_rewind
 *   bind f5  tas_frame_advance
 *   bind f6  tas_pause
 *   bind f7  tas_record
 *   bind f8  tas_stoprecord
 *   bind f9  tas_play
 *   bind f10 tas_stop
 */

#pragma once

#include <engine/console.h>
#include <game/client/component.h>

#include <vector>

// Один тик — все поля CNetObj_PlayerInput без include generated/protocol.h
struct STasInput
{
	int m_Direction;
	int m_TargetX;
	int m_TargetY;
	int m_Jump;
	int m_Fire;
	int m_Hook;
	int m_PlayerFlags;
	int m_WantedWeapon;
	int m_NextWeapon;
	int m_PrevWeapon;
};

struct STasFrame
{
	int       m_Tick;
	STasInput m_Input;
};

enum class ETasMode
{
	IDLE,
	RECORDING,
	PLAYBACK,
};

class CTas : public CComponent
{
public:
	CTas();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnConsoleInit() override;

	// Вызывается из OnSnapInput. pData — массив int размером sizeof(CNetObj_PlayerInput)/4
	bool GetPlaybackInput(int *pData) const;
	bool IsPlayback() const { return m_Mode == ETasMode::PLAYBACK; }

private:
	ETasMode m_Mode          = ETasMode::IDLE;

	std::vector<STasFrame> m_vFrames;
	int   m_PlaybackIndex    = 0;

	float m_Slowmo           = 1.0f;
	bool  m_Paused           = false;
	bool  m_FrameAdvance     = false;
	int   m_LastGameTick     = -1;

	static void ConTasRecord      (IConsole::IResult *pResult, void *pUser);
	static void ConTasStopRecord  (IConsole::IResult *pResult, void *pUser);
	static void ConTasPlay        (IConsole::IResult *pResult, void *pUser);
	static void ConTasStop        (IConsole::IResult *pResult, void *pUser);
	static void ConTasRewind      (IConsole::IResult *pResult, void *pUser);
	static void ConTasSlowmo      (IConsole::IResult *pResult, void *pUser);
	static void ConTasClear       (IConsole::IResult *pResult, void *pUser);
	static void ConTasFrameAdvance(IConsole::IResult *pResult, void *pUser);
	static void ConTasPause       (IConsole::IResult *pResult, void *pUser);

	void StartRecord();
	void StopRecord();
	void StartPlayback();
	void Stop();
	void DoRewind(int Ticks);
	void RenderHud();
	const char *ModeStr() const;
};
