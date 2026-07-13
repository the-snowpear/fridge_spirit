#ifndef __BSP_ANO_USART_H
#define	__BSP_ANO_USART_H

#include "stm32f10x.h"

void Send_Data(int16_t *Gyro,int16_t *Accel);
void Data_Send_Status(float Pitch,float Roll,float Yaw);


#endif /* __BSP_ANO_USART_H */
