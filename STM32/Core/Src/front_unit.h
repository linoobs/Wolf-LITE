#ifndef FRONT_UNIT_h
#define FRONT_UNIT_h

#include "stm32f4xx_hal.h"
#include <stdbool.h>

#define MCP3008_THRESHOLD 100
#define BOTTOM_SCROLLBUTTONS_GROUPS_COUNT 4

typedef struct
{
	uint8_t port;
	uint8_t channel;
	bool state;
	bool prev_state;
	uint32_t start_hold_time;
	bool afterhold;
	bool work_in_menu;
	char name[16];
	void (*clickHandler)(void);
	void (*holdHandler)(void);
} PERIPH_FrontPanel_Button;

extern PERIPH_FrontPanel_Button* PERIPH_FrontPanel_BottomScroll_Buttons_Active;

extern void FRONTPANEL_ENCODER_checkRotate(void);
extern void FRONTPANEL_ENCODER2_checkRotate(void);
extern void FRONTPANEL_check_ENC2SW(void);
extern void FRONTPANEL_Init(void);
extern void FRONTPANEL_Process(void);

#endif
