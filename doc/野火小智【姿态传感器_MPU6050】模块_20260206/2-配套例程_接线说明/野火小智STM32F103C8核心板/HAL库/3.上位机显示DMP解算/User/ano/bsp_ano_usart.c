/**
  ******************************************************************************
  * @file    bsp_ano_usart.c
  * @author  fire
  * @version V1.0
  * @date    2024-xx-xx
  * @brief   使用串口1，重定向c库printf函数到usart端口，中断接收模式
  ******************************************************************************
  * @attention
  *
  * 实验平台:野火 STM32 F103 开发板  
  * 论坛    :http://www.firebbs.cn
  * 淘宝    :http://firestm32.taobao.com
  *
  ******************************************************************************
  */ 
#include "./ano/bsp_ano_usart.h"
#include "./usart/bsp_debug_usart.h"


#define BYTE0(dwTemp)       (*(char *)(&dwTemp))
#define BYTE1(dwTemp)       (*((char *)(&dwTemp) + 1))
#define BYTE2(dwTemp)       (*((char *)(&dwTemp) + 2))
#define BYTE3(dwTemp)       (*((char *)(&dwTemp) + 3))
    
extern UART_HandleTypeDef UartHandle;

/**
  * @brief  控制串口发送1个字符
  * @param  c:要发送的字符
  * @retval none
  */
void usart_send_char(uint8_t c)
{
	while(__HAL_UART_GET_FLAG(&UartHandle,USART_FLAG_TXE)==RESET); //循环发送,直到发送完毕
	HAL_UART_Transmit(&UartHandle,&c,1,1000);
}



/*函数功能：根据匿名最新上位机协议写的显示欧拉角的程序（上位机V7版本）
 *具体协议说明请查看上位机软件的帮助说明。
 */
void Data_Send_Status(float Pitch,float Roll,float Yaw)
{
	unsigned char i=0;
	unsigned char _cnt=0,sum = 0,add=0;
	unsigned int _temp;
	uint8_t data_to_send[50];

	data_to_send[_cnt++]=0xAA;//帧头
	data_to_send[_cnt++]=0xFF;//目标地址，0xFF为广播
	data_to_send[_cnt++]=0x03;//欧拉角格式
	data_to_send[_cnt++]=7;   //数据长度
	
	_temp = (int)(Roll*100);
	data_to_send[_cnt++]=BYTE0(_temp);
	data_to_send[_cnt++]=BYTE1(_temp);
	_temp = (int)(Pitch*100);
	data_to_send[_cnt++]=BYTE0(_temp);
	data_to_send[_cnt++]=BYTE1(_temp);
	_temp = (int)(Yaw*100);
	data_to_send[_cnt++]=BYTE0(_temp);
	data_to_send[_cnt++]=BYTE1(_temp);
	
	data_to_send[_cnt++]=0x00;
	
	//和校验
	for(i=0;i<_cnt;i++)
    {
		sum+= data_to_send[i];
        add+= sum;
    }
	data_to_send[_cnt++]=sum;
	data_to_send[_cnt++]=add;
    
	//串口发送数据
	for(i=0;i<_cnt;i++)
		usart_send_char(data_to_send[i]);
}

/*函数功能：根据匿名最新上位机协议写的显示传感器数据（上位机V7版本）
 *具体协议说明请查看上位机软件的帮助说明。
 */
void Send_Data(int16_t *Gyro,int16_t *Accel)
{
	unsigned char i=0;
	unsigned char _cnt=0,sum = 0,add = 0;

	uint8_t data_to_send[50];

	data_to_send[_cnt++]=0xAA;
	data_to_send[_cnt++]=0xFF;
	data_to_send[_cnt++]=0x01;
	data_to_send[_cnt++]=13;
	

	data_to_send[_cnt++]=BYTE0(Accel[0]);
	data_to_send[_cnt++]=BYTE1(Accel[0]);
	data_to_send[_cnt++]=BYTE0(Accel[1]);
	data_to_send[_cnt++]=BYTE1(Accel[1]);
	data_to_send[_cnt++]=BYTE0(Accel[2]);
	data_to_send[_cnt++]=BYTE1(Accel[2]);
	
	data_to_send[_cnt++]=BYTE0(Gyro[0]);
	data_to_send[_cnt++]=BYTE1(Gyro[0]);
	data_to_send[_cnt++]=BYTE0(Gyro[1]);
	data_to_send[_cnt++]=BYTE1(Gyro[1]);
	data_to_send[_cnt++]=BYTE0(Gyro[2]);
	data_to_send[_cnt++]=BYTE1(Gyro[2]);

    data_to_send[_cnt++]=0;

	//和校验
	for(i=0;i<_cnt;i++)
    {
		sum+= data_to_send[i];
        add+= sum;
    }
	data_to_send[_cnt++]=sum;
    data_to_send[_cnt++]=add;
	
	//串口发送数据
	for(i=0;i<_cnt;i++)
		usart_send_char(data_to_send[i]);
}

