#include "stm32f4xx_hal.h"
#include "main.h"
#include "trx_manager.h"
#include "functions.h"
#include "lcd.h"
#include "fpga.h"
#include "settings.h"
#include "wm8731.h"
#include "fpga.h"
#include "bands.h"
#include "agc.h"
#include "audio_filters.h"
#include "usbd_audio_if.h"
#include "front_unit.h"
#include "rf_unit.h"
#include "system_menu.h"

volatile bool TRX_ptt_hard = false;
volatile bool TRX_ptt_soft = false;
volatile bool TRX_old_ptt_soft = false;
volatile bool TRX_key_serial = false;
volatile bool TRX_old_key_serial = false;
volatile bool TRX_key_dot_hard = false;
volatile bool TRX_key_dash_hard = false;
volatile uint_fast16_t TRX_Key_Timeout_est = 0;
volatile bool TRX_RX_IQ_swap = false;
volatile bool TRX_TX_IQ_swap = false;
volatile bool TRX_Tune = false;
volatile bool TRX_Inited = false;
volatile int_fast16_t TRX_RX_dBm = -100;
volatile bool TRX_ADC_OTR = false;
volatile bool TRX_DAC_OTR = false;
volatile int16_t TRX_ADC_MINAMPLITUDE = 0;
volatile int16_t TRX_ADC_MAXAMPLITUDE = 0;
volatile uint32_t TRX_SNTP_Synced = 0; // time of the last time synchronization
volatile int_fast16_t TRX_SHIFT = 0;
volatile float32_t TRX_MAX_TX_Amplitude = MAX_TX_AMPLITUDE;
volatile float32_t TRX_PWR_Forward = 0;
volatile float32_t TRX_PWR_Backward = 0;
volatile float32_t TRX_SWR = 0;
volatile float32_t TRX_ALC = 0;
static uint_fast8_t autogain_wait_reaction = 0;	  // timer for waiting for a reaction from changing the ATT / PRE modes
volatile uint8_t TRX_AutoGain_Stage = 0;			  // stage of working out the amplification corrector
static uint32_t KEYER_symbol_start_time = 0;	  // start time of the automatic key character
static uint_fast8_t KEYER_symbol_status = 0;		  // status (signal or period) of the automatic key symbol
volatile float32_t TRX_IQ_phase_error = 0.0f;
volatile bool TRX_NeedGoToBootloader = false;
volatile bool TRX_Temporary_Stop_BandMap = false;
volatile bool TRX_Mute = false;
volatile uint32_t TRX_Temporary_Mute_StartTime = 0;
uint32_t TRX_freq_phrase = 0;
uint32_t TRX_freq_phrase_tx = 0;
float32_t TRX_InVoltage = 12.0f;

static uint_fast8_t TRX_TXRXMode = 0; //0 - undef, 1 - rx, 2 - tx, 3 - txrx
static void TRX_Start_RX(void);
static void TRX_Start_TX(void);
static void TRX_Start_TXRX(void);

bool TRX_on_TX(void)
{
	if (TRX_ptt_hard || TRX_ptt_soft || TRX_Tune || CurrentVFO()->Mode == TRX_MODE_LOOPBACK || TRX_Key_Timeout_est > 0)
		return true;
	return false;
}

void TRX_Init()
{
	TRX_Start_RX();
	uint_fast8_t saved_mode = CurrentVFO()->Mode;
	TRX_setFrequency(CurrentVFO()->Freq, CurrentVFO());
	TRX_setMode(saved_mode, CurrentVFO());
	HAL_ADCEx_InjectedStart(&hadc1);
	HAL_ADCEx_InjectedStart(&hadc2);
}

void TRX_Restart_Mode()
{
	uint_fast8_t mode = CurrentVFO()->Mode;
	//CLAR
	if (TRX.CLAR)
	{
		TRX.current_vfo = !TRX.current_vfo;
		TRX_setFrequency(CurrentVFO()->Freq, CurrentVFO());
		TRX_setMode(CurrentVFO()->Mode, CurrentVFO());
		LCD_UpdateQuery.FreqInfo = true;
		LCD_UpdateQuery.TopButtons = true;
		LCD_UpdateQuery.StatusInfoGUIRedraw = true;
	}
	//
	if (TRX_on_TX())
	{
		if (mode == TRX_MODE_LOOPBACK || mode == TRX_MODE_CW_L || mode == TRX_MODE_CW_U)
			TRX_Start_TXRX();
		else
			TRX_Start_TX();
	}
	else
	{
		TRX_Start_RX();
	}
	FFT_Reset();
}

static void TRX_Start_RX()
{
	if (TRX_TXRXMode == 1)
		return;
	//sendToDebug_str("RX MODE\r\n");
	WM8731_CleanBuffer();
	Processor_NeedRXBuffer = false;
	WM8731_Buffer_underrun = false;
	WM8731_DMA_state = true;
	WM8731_RX_mode();
	WM8731_start_i2s_and_dma();
	TRX_TXRXMode = 1;
	//clean TX buffer
	memset((void *)&FPGA_Audio_SendBuffer_Q[0], 0x00, sizeof(FPGA_Audio_SendBuffer_Q));
	memset((void *)&FPGA_Audio_SendBuffer_I[0], 0x00, sizeof(FPGA_Audio_SendBuffer_I));
}

static void TRX_Start_TX()
{
	if (TRX_TXRXMode == 2)
		return;
	//sendToDebug_str("TX MODE\r\n");
	WM8731_CleanBuffer();
	HAL_Delay(10); // delay before the RF signal is applied, so that the relay has time to trigger
	WM8731_TX_mode();
	WM8731_start_i2s_and_dma();
	TRX_TXRXMode = 2;
}

static void TRX_Start_TXRX()
{
	if (TRX_TXRXMode == 3)
		return;
	//sendToDebug_str("TXRX MODE\r\n");
	WM8731_CleanBuffer();
	WM8731_TXRX_mode();
	WM8731_start_i2s_and_dma();
	TRX_TXRXMode = 3;
}

void TRX_ptt_change(void)
{
	if (TRX_Tune)
		return;
	bool TRX_new_ptt_hard = !HAL_GPIO_ReadPin(PTT_IN_GPIO_Port, PTT_IN_Pin);
	if (TRX_ptt_hard != TRX_new_ptt_hard)
	{
		TRX_ptt_hard = TRX_new_ptt_hard;
		TRX_ptt_soft = false;
		LCD_UpdateQuery.StatusInfoGUIRedraw = true;
		FPGA_NeedSendParams = true;
		TRX_Restart_Mode();
	}
	if (TRX_ptt_soft != TRX_old_ptt_soft)
	{
		TRX_old_ptt_soft = TRX_ptt_soft;
		LCD_UpdateQuery.StatusInfoGUIRedraw = true;
		FPGA_NeedSendParams = true;
		TRX_Restart_Mode();
	}
}

void TRX_key_change(void)
{
	if (TRX_Tune)
		return;
	if (CurrentVFO()->Mode != TRX_MODE_CW_L && CurrentVFO()->Mode != TRX_MODE_CW_U)
		return;
	bool TRX_new_key_dot_hard = !HAL_GPIO_ReadPin(KEY_IN_DOT_GPIO_Port, KEY_IN_DOT_Pin);
	if (TRX_key_dot_hard != TRX_new_key_dot_hard)
	{
		TRX_key_dot_hard = TRX_new_key_dot_hard;
		if (TRX_key_dot_hard == true && (KEYER_symbol_status == 0 || !TRX.CW_KEYER))
		{
			TRX_Key_Timeout_est = TRX.CW_Key_timeout;
			LCD_UpdateQuery.StatusInfoGUIRedraw = true;
			FPGA_NeedSendParams = true;
			TRX_Restart_Mode();
		}
	}
	bool TRX_new_key_dash_hard = !HAL_GPIO_ReadPin(KEY_IN_DASH_GPIO_Port, KEY_IN_DASH_Pin);
	if (TRX_key_dash_hard != TRX_new_key_dash_hard)
	{
		TRX_key_dash_hard = TRX_new_key_dash_hard;
		if (TRX_key_dash_hard == true && (KEYER_symbol_status == 0 || !TRX.CW_KEYER))
		{
			TRX_Key_Timeout_est = TRX.CW_Key_timeout;
			LCD_UpdateQuery.StatusInfoGUIRedraw = true;
			FPGA_NeedSendParams = true;
			TRX_Restart_Mode();
		}
	}
	if (TRX_key_serial != TRX_old_key_serial)
	{
		TRX_old_key_serial = TRX_key_serial;
		if (TRX_key_serial == true)
			TRX_Key_Timeout_est = TRX.CW_Key_timeout;
		LCD_UpdateQuery.StatusInfoGUIRedraw = true;
		FPGA_NeedSendParams = true;
		TRX_Restart_Mode();
	}
}

void TRX_setFrequency(uint32_t _freq, VFO *vfo)
{
	if (_freq < 1)
		return;
	if (_freq >= MAX_RX_FREQ_HZ)
		_freq = MAX_RX_FREQ_HZ;

	vfo->Freq = _freq;

	//get band
	int_fast8_t bandFromFreq = getBandFromFreq(_freq, false);
	if (bandFromFreq >= 0)
	{
		TRX.BANDS_SAVED_SETTINGS[bandFromFreq].Freq = _freq;
	}
	if (TRX.BandMapEnabled && !TRX_Temporary_Stop_BandMap)
	{
		uint_fast8_t mode_from_bandmap = getModeFromFreq(vfo->Freq);
		if (vfo->Mode != mode_from_bandmap)
		{
			TRX_setMode(mode_from_bandmap, vfo);
			TRX.BANDS_SAVED_SETTINGS[bandFromFreq].Mode = mode_from_bandmap;
			LCD_UpdateQuery.TopButtons = true;
		}
	}

	//get fpga freq phrase
	VFO *current_vfo = CurrentVFO();
	VFO *secondary_vfo = SecondaryVFO();
	TRX_freq_phrase = getRXPhraseFromFrequency((int32_t)current_vfo->Freq + TRX_SHIFT);
	TRX_freq_phrase_tx = getTXPhraseFromFrequency((int32_t)current_vfo->Freq + TRX_SHIFT);
	if (!TRX_on_TX())
	{
		switch (current_vfo->Mode)
		{
		case TRX_MODE_CW_L:
			TRX_freq_phrase_tx = getTXPhraseFromFrequency((int32_t)current_vfo->Freq - TRX.CW_GENERATOR_SHIFT_HZ);
			break;
		case TRX_MODE_CW_U:
			TRX_freq_phrase_tx = getTXPhraseFromFrequency((int32_t)current_vfo->Freq + TRX.CW_GENERATOR_SHIFT_HZ);
			break;
		default:
			TRX_freq_phrase_tx = getTXPhraseFromFrequency((int32_t)current_vfo->Freq);
			break;
		}
	}

	//
	TRX_MAX_TX_Amplitude = getMaxTXAmplitudeOnFreq(vfo->Freq);
	FPGA_NeedSendParams = true;
}

void TRX_setMode(uint_fast8_t _mode, VFO *vfo)
{
	if (vfo->Mode == TRX_MODE_LOOPBACK || _mode == TRX_MODE_LOOPBACK)
		LCD_UpdateQuery.StatusInfoGUI = true;
	vfo->Mode = _mode;
	if (vfo->Mode == TRX_MODE_LOOPBACK)
		TRX_Start_TXRX();

	switch (_mode)
	{
	case TRX_MODE_AM:
		vfo->LPF_Filter_Width = TRX.AM_LPF_Filter;
		vfo->HPF_Filter_Width = 0;
		break;
	case TRX_MODE_LSB:
	case TRX_MODE_USB:
	case TRX_MODE_DIGI_L:
	case TRX_MODE_DIGI_U:
		vfo->LPF_Filter_Width = TRX.SSB_LPF_Filter;
		vfo->HPF_Filter_Width = TRX.SSB_HPF_Filter;
		break;
	case TRX_MODE_CW_L:
	case TRX_MODE_CW_U:
		vfo->LPF_Filter_Width = TRX.CW_LPF_Filter;
		vfo->HPF_Filter_Width = TRX.CW_HPF_Filter;
		LCD_UpdateQuery.StatusInfoGUI = true;
		break;
	case TRX_MODE_NFM:
		vfo->LPF_Filter_Width = TRX.FM_LPF_Filter;
		vfo->HPF_Filter_Width = 0;
		break;
	case TRX_MODE_WFM:
		vfo->LPF_Filter_Width = 0;
		vfo->HPF_Filter_Width = 0;
		break;
	}
	NeedReinitAudioFilters = true;
	NeedSaveSettings = true;
	LCD_UpdateQuery.StatusInfoBar = true;
	LCD_UpdateQuery.StatusInfoGUI = true;
}

void TRX_DoAutoGain(void)
{
	#define SKIP_CYCLES_DOWNSTAGE 10 //skip cycles on stage downgrade
	static uint8_t skip_cycles = 0;
	
	//Process AutoGain feature
	if (TRX.AutoGain && !TRX_on_TX())
	{
		int32_t max_amplitude = abs(TRX_ADC_MAXAMPLITUDE);
		if(abs(TRX_ADC_MINAMPLITUDE) > max_amplitude)
			max_amplitude = abs(TRX_ADC_MINAMPLITUDE);
		
		switch (TRX_AutoGain_Stage)
		{
		case 0: // stage 1 - ATT
			TRX.ADC_Driver = false;
			TRX.ATT = true;
			FPGA_NeedSendParams = true;
			LCD_UpdateQuery.TopButtons = true;
			autogain_wait_reaction = 0;
			if(skip_cycles == 0)
			{
				sendToDebug_strln("AUTOGAIN BPF + ATT");
				TRX_AutoGain_Stage++;
			}
			else
				skip_cycles--;
			break;
		case 1:																					// changed the state, process the results
			if ((max_amplitude * db2rateV(TRX.ATT_DB)) <= AUTOGAIN_TARGET_AMPLITUDE) // if we can turn off ATT - go to the next stage (+ 12dB)
				autogain_wait_reaction++;
			else
				autogain_wait_reaction = 0;
			if (autogain_wait_reaction >= AUTOGAIN_CORRECTOR_WAITSTEP)
			{
				TRX_AutoGain_Stage++;
				autogain_wait_reaction = 0;
			}
			break;
		case 2: // stage 2 - NONE
			TRX.ATT = false;
			TRX.ADC_Driver = false;
			FPGA_NeedSendParams = true;
			LCD_UpdateQuery.TopButtons = true;
			autogain_wait_reaction = 0;
			if(skip_cycles == 0)
			{
				sendToDebug_strln("AUTOGAIN BPF");
				TRX_AutoGain_Stage++;
			}
			else
				skip_cycles--;
			break;
		case 3: // changed the state, process the results
			if (max_amplitude > AUTOGAIN_MAX_AMPLITUDE || TRX_ADC_OTR)
			{
				TRX_AutoGain_Stage -= 3; // too much gain, go back one step
				skip_cycles = SKIP_CYCLES_DOWNSTAGE;
			}
			if ((max_amplitude * db2rateV(ADC_DRIVER_GAIN_DB) * db2rateV(-TRX.ATT_DB)) <= AUTOGAIN_TARGET_AMPLITUDE) // if we can turn off ATT - go to the next stage (+ 12dB)
				autogain_wait_reaction++;
			else
			{
				autogain_wait_reaction = 0;
			}
			if (autogain_wait_reaction >= AUTOGAIN_CORRECTOR_WAITSTEP)
			{
				TRX_AutoGain_Stage++;
				autogain_wait_reaction = 0;
			}
			break;
		case 5: // stage 4 - BPF + DRIVER + ATT
			TRX.ATT = false;
			TRX.ADC_Driver = true;
			FPGA_NeedSendParams = true;
			LCD_UpdateQuery.TopButtons = true;
			autogain_wait_reaction = 0;
			if(skip_cycles == 0)
			{
				sendToDebug_strln("AUTOGAIN BPF + DRIVER + ATT");
				TRX_AutoGain_Stage++;
			}
			else
				skip_cycles--;
			break;
		case 6: // changed the state, process the results
			if (max_amplitude > AUTOGAIN_MAX_AMPLITUDE || TRX_ADC_OTR)
			{
				TRX_AutoGain_Stage -= 3;															// too much gain, go back one step
				skip_cycles = SKIP_CYCLES_DOWNSTAGE;
			}
			if ((max_amplitude * db2rateV(TRX.ATT_DB)) <= AUTOGAIN_TARGET_AMPLITUDE) // if we can turn off ATT - go to the next stage (+ 12dB)
				autogain_wait_reaction++;
			else
				autogain_wait_reaction = 0;
			if (autogain_wait_reaction >= AUTOGAIN_CORRECTOR_WAITSTEP)
			{
				TRX_AutoGain_Stage++;
				autogain_wait_reaction = 0;
			}
			break;
		case 7: // stage 5 - BPF + DRIVER
			TRX.ATT = false;
			TRX.ADC_Driver = true;
			FPGA_NeedSendParams = true;
			LCD_UpdateQuery.TopButtons = true;
			autogain_wait_reaction = 0;
			if(skip_cycles == 0)
			{
				sendToDebug_strln("AUTOGAIN BPF + DRIVER");
				TRX_AutoGain_Stage++;
			}
			else
				skip_cycles--;
			break;
		case 9: // changed the state, process the results
			if (max_amplitude > AUTOGAIN_MAX_AMPLITUDE || TRX_ADC_OTR)
			{
				TRX_AutoGain_Stage -= 3; // too much gain, go back one step
				skip_cycles = SKIP_CYCLES_DOWNSTAGE;
			}
			break;
			
		default:
			TRX_AutoGain_Stage = 0;
			break;
		}
		
		int8_t band = getBandFromFreq(CurrentVFO()->Freq, true);
		if (band > 0)
		{
			TRX.BANDS_SAVED_SETTINGS[band].ATT = TRX.ATT;
			TRX.BANDS_SAVED_SETTINGS[band].ADC_Driver = TRX.ADC_Driver;
			TRX.BANDS_SAVED_SETTINGS[band].AutoGain_Stage = TRX_AutoGain_Stage;
		}
	}
}

void TRX_DBMCalculate(void)
{
	if(Processor_RX_Power_value == 0)
		return;
	
	float32_t adc_volts = Processor_RX_Power_value * (ADC_RANGE / 2.0f);
	if(adc_volts > 0.0f)
	{
		TRX_RX_dBm = (int16_t)(10.0f * log10f_fast((adc_volts * adc_volts / ADC_INPUT_IMPEDANCE) / 0.001f));
		TRX_RX_dBm += CALIBRATE.smeter_calibration;
	}
	Processor_RX_Power_value = 0;
}

float32_t current_cw_power = 0.0f;
static float32_t TRX_generateRiseSignal(float32_t power)
{
	if(current_cw_power < power)
		current_cw_power += power * 0.01f;
	if(current_cw_power > power)
		current_cw_power = power;
	return current_cw_power;
}
static float32_t TRX_generateFallSignal(float32_t power)
{
	if(current_cw_power > 0.0f)
		current_cw_power -= power * 0.01f;
	if(current_cw_power < 0.0f)
		current_cw_power = 0.0f;
	return current_cw_power;
}

float32_t TRX_GenerateCWSignal(float32_t power)
{
	if (!TRX.CW_KEYER)
	{
		if (!TRX_key_serial && !TRX_ptt_hard && !TRX_key_dot_hard && !TRX_key_dash_hard)
				return TRX_generateFallSignal(power);
		return TRX_generateRiseSignal(power);
	}

	uint32_t dot_length_ms = 1200 / TRX.CW_KEYER_WPM;
	uint32_t dash_length_ms = dot_length_ms * 3;
	uint32_t sim_space_length_ms = dot_length_ms;
	uint32_t curTime = HAL_GetTick();
	//dot
	if (KEYER_symbol_status == 0 && TRX_key_dot_hard)
	{
		KEYER_symbol_start_time = curTime;
		KEYER_symbol_status = 1;
	}
	if (KEYER_symbol_status == 1 && (KEYER_symbol_start_time + dot_length_ms) > curTime)
	{
		TRX_Key_Timeout_est = TRX.CW_Key_timeout;
		return TRX_generateRiseSignal(power);
	}
	if (KEYER_symbol_status == 1 && (KEYER_symbol_start_time + dot_length_ms) < curTime)
	{
		KEYER_symbol_start_time = curTime;
		KEYER_symbol_status = 3;
	}
	
	//dash
	if (KEYER_symbol_status == 0 && TRX_key_dash_hard)
	{
		KEYER_symbol_start_time = curTime;
		KEYER_symbol_status = 2;
	}
	if (KEYER_symbol_status == 2 && (KEYER_symbol_start_time + dash_length_ms) > curTime)
	{
		TRX_Key_Timeout_est = TRX.CW_Key_timeout;
		return TRX_generateRiseSignal(power);
	}
	if (KEYER_symbol_status == 2 && (KEYER_symbol_start_time + dash_length_ms) < curTime)
	{
		KEYER_symbol_start_time = curTime;
		KEYER_symbol_status = 3;
	}
	
	//space
	if (KEYER_symbol_status == 3 && (KEYER_symbol_start_time + sim_space_length_ms) > curTime)
	{
		TRX_Key_Timeout_est = TRX.CW_Key_timeout;
		return TRX_generateFallSignal(power);
	}
	if (KEYER_symbol_status == 3 && (KEYER_symbol_start_time + sim_space_length_ms) < curTime)
	{
		KEYER_symbol_status = 0;
	}
	
	return TRX_generateFallSignal(power);
}

void TRX_TemporaryMute(void)
{
	WM8731_Mute();
	TRX_Temporary_Mute_StartTime = HAL_GetTick();
}
