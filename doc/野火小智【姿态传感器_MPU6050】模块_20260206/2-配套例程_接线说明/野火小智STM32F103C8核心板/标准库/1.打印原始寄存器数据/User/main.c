/**
 ******************************************************************************
 * @file    main.c
 * @author  fire
 * @version V1.0
 * @date    2024-xx-xx
 * @brief   MPU6050
 ******************************************************************************
 * @attention
 *
 * 实验平台:野火小智 STM32F103C8 核心板 
 * 论坛    :http://www.firebbs.cn
 * 淘宝    :https://fire-stm32.taobao.com
 *
 ******************************************************************************
 */

#include "stm32f10x.h"
#include "stm32f10x_it.h"
#include "systick/bsp_SysTick.h"
#include "led/bsp_led.h"
#include "usart/bsp_usart.h"
#include "mpu6050/mpu6050.h"

#ifdef   soft_IIC
#include "iic/bsp_soft_i2c.h"
#else
#include "iic/bsp_hard_i2c.h"
#endif

/* MPU6050数据 */
short Accel[3];
short Gyro[3];
float Temp;


/**
  * @brief  主函数
  * @param  无  
  * @retval 无
  */
int main(void)
{
    /* 配置SysTick定时器和中断 */
    SysTick_Init(); //配置 SysTick 为 1ms 中断一次，在中断里读取传感器数据
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk; //启动定时器
    
    /* LED 端口初始化 */
    LED_GPIO_Config();
    /* 串口1通信初始化 */
    USART_Config();
	printf("\r\n 欢迎使用野火  STM32 开发板 \r\n");
    printf("\r\n 这是一个MPU6050串口显示读取寄存器测试例程\r\n");
    
    //I2C初始化
    #ifdef soft_IIC
        i2c_GPIO_Config();
    #else
        MPU_I2C_Config();
    #endif
    
    //MPU6050初始化
    MPU6050_Init();

    //检测MPU6050
    if(MPU6050ReadID() == 0)
    {
        printf("\r\n没有检测到MPU6050传感器！\r\n");
        LED1_ON; 
        while(1);	
    }
    
    
    
    while(1)
    {
    if( task_readdata_finish ) //task_readdata_finish = 1 表示读取MPU6050数据完成
    {
    
        printf("\r\n加速度： %8d%8d%8d    ",Accel[0],Accel[1],Accel[2]);
        printf("陀螺仪： %8d%8d%8d    ",Gyro[0],Gyro[1],Gyro[2]);
        printf("温度： %8.2f°C",Temp);
        
        task_readdata_finish = 0; // 清零标志位
    }
    }
	
}

/*********************************************END OF FILE**********************/
