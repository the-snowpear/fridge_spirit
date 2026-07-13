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
#include "ano/bsp_ano_usart.h"

/* mpu6050 DMP库  begin */
#include "eMPL_outputs.h"
#include "inv_mpu.h"
#include "inv_mpu_dmp_motion_driver.h"
#include "invensense.h"
#include "invensense_adv.h"
#include "log.h"
#include "mltypes.h"
#include "mpu.h"
#include "packet.h"
#include "mpu6050_SL.h"


#ifdef   soft_IIC
#include "iic/bsp_soft_i2c.h"
#else
#include "iic/bsp_hard_i2c.h"
#endif

/* MPU6050数据 */
short Accel[3];
short Gyro[3];
float Temp;

extern struct inv_sensor_cal_t sensors;

/**
  * @brief  主函数
  * @param  无  
  * @retval 无
  */
int main(void)
{
    unsigned char new_temp = 0;
    unsigned long timestamp;
    
    /* 配置SysTick定时器和中断 */
    SysTick_Init(); //配置 SysTick 为 1ms 中断一次，在中断里读取传感器数据
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk; //启动定时器
    
    /* LED 端口初始化 */
    LED_GPIO_Config();
    /* 串口1通信初始化 */
    USART_Config();
	printf("\r\n 欢迎使用野火  STM32 开发板 \r\n");
    printf("\r\n 这是一个MPU6050使用上位机测试例程\r\n");
    printf("\r\n 在串口助手显示乱码，请复位后打开匿名上位机进行姿态显示\r\n");
    
    //I2C初始化
    #ifdef soft_IIC
        i2c_GPIO_Config();
    #else
        MPU_I2C_Config();
    #endif
    
    //初始化INT引脚
    MPU_INT_Init();
    
    //初始化MPU6050
    MPU6050_mpu_init();
    MPU6050_mpl_init();
    MPU6050_config();

    //检测MPU6050
    if(MPU6050ReadID() == 0)
    {
        printf("\r\n没有检测到MPU6050传感器！\r\n");
        LED1_ON; 
        while(1);	
    }
    
    
    
    mdelay(500);
    while (1)
    {
        unsigned long sensor_timestamp;
        int new_data = 0;

        /* 接收到INT传来的中断信息后继续往下 */
        if (!hal.sensors || !hal.new_gyro)
        {
            continue;
        }

        /* 每过500ms读取一次温度 */
        get_tick_count(&timestamp);
        if (timestamp > hal.next_temp_ms)
        {
            hal.next_temp_ms = timestamp + TEMP_READ_MS;
            new_temp = 1;
        }

        /* 接收到新数据 并且 开启DMP */
        if (hal.new_gyro && hal.dmp_on)
        {
            short gyro[3], accel_short[3], sensors;
            unsigned char more;
            long accel[3], quat[4], temperature;
            /* 当DMP正在使用时，该函数从FIFO获取新数据 */
            dmp_read_fifo(gyro, accel_short, quat, &sensor_timestamp, &sensors, &more);
            /* 如果more=0，则数据被获取完毕 */
            if (!more)
                hal.new_gyro = 0;

            /* 读取相对应的新数据，推入MPL */
            //四元数数据
            if (sensors & INV_WXYZ_QUAT)
            {
                inv_build_quat(quat, 0, sensor_timestamp);
                new_data = 1;
            }

            //陀螺仪数据
            if (sensors & INV_XYZ_GYRO)
            {
                inv_build_gyro(gyro, sensor_timestamp);
                new_data = 1;
                if (new_temp)
                {
                    new_temp = 0;
                    /* 获取温度，仅用于陀螺温度比较 */
                    mpu_get_temperature(&temperature, &sensor_timestamp);
                    inv_build_temp(temperature, sensor_timestamp);
                }
            }
            
            //加速度计数据
            if (sensors & INV_XYZ_ACCEL)
            {
                accel[0] = (long)accel_short[0];
                accel[1] = (long)accel_short[1];
                accel[2] = (long)accel_short[2];
                inv_build_accel(accel, 0, sensor_timestamp);
                new_data = 1;
            }
        }

        if (new_data)
        {
            long data[9];
            float Pitch,Roll,Yaw;
            int8_t accuracy;
            /* 处理接收到的数据 */
            if (inv_execute_on_data())
            {
                printf("数据错误\n");
            }
            
			/*获取欧拉角*/
            if (inv_get_sensor_type_euler(data, &accuracy,(inv_time_t*)&timestamp))
            {

                Pitch =data[0]*1.0/(1<<16) ;
                Roll = data[1]*1.0/(1<<16);
                Yaw = data[2]*1.0/(1<<16);
                
                           
                /*向匿名上位机发送欧拉角*/
                Data_Send_Status(Pitch,Roll,Yaw);
                /*向匿名上位机发送陀螺仪数据以及加速度计数据*/
                Send_Data((int16_t *)&sensors.gyro.raw,(int16_t *)&sensors.accel.raw);
            } 
            mdelay(10);
        }

    }

}

	


/*********************************************END OF FILE**********************/
