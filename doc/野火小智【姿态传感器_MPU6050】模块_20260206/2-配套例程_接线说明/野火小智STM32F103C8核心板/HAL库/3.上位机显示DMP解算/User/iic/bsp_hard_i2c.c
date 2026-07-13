/**
  ******************************************************************************
  * @file    bsp_iic.c
  * @author  fire
  * @version V1.0
  * @date    2024-xx-xx
  * @brief   IIC应用函数接口
  ******************************************************************************
  * @attention
  *
  * 实验平台:野火 STM32 F103 开发板  
  * 论坛    :http://www.firebbs.cn
  * 淘宝    :http://firestm32.taobao.com
  *
  ******************************************************************************
  */
  
#include "./iic/bsp_hard_i2c.h"
#include "./mpu6050/mpu6050.h"
#include "./usart/bsp_debug_usart.h"

I2C_HandleTypeDef mpu_i2c;

 /**
  * @brief  初始化I2C的IO
  * @param  无
  * @retval 无
  */
void HAL_I2C_MspInit(I2C_HandleTypeDef* i2cHandle)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if(i2cHandle->Instance == MPU_I2C)
    {
        //开启I2C时钟
        I2C_CLK_ENABLE();
        //开启I2C的GPIO时钟
        I2C_GPIO_CLK_ENABLE();
        
        //初始化GPIO
        GPIO_InitStruct.Pin   = I2C_SCL_PIN | I2C_SDA_PIN;
        GPIO_InitStruct.Mode  = GPIO_MODE_AF_OD;//复用开漏输出
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
        GPIO_InitStruct.Pull  = GPIO_NOPULL;
        HAL_GPIO_Init(I2C_GPIO_PORT,&GPIO_InitStruct);
        
        
    }   
}

 /**
  * @brief  失能I2C的IO
  * @param  无
  * @retval 无
  */
void HAL_I2C_MspDeInit(I2C_HandleTypeDef* i2cHandle)
{

  if(i2cHandle->Instance == MPU_I2C)
  {
    //失能I2C时钟
    I2C_CLK_DISABLE();

    //失能SCL，SDA
    HAL_GPIO_DeInit(I2C_GPIO_PORT, I2C_SCL_PIN);
    HAL_GPIO_DeInit(I2C_GPIO_PORT, I2C_SDA_PIN);

  }
}

 /**
  * @brief  配置MPU6050的I2C
  * @param  无
  * @retval 无
  */
void MPU_I2C_Config(void)
{
    mpu_i2c.Instance             = MPU_I2C;                    //选择I2C2
    mpu_i2c.Init.ClockSpeed      = 400000;                     //时钟频率为400000
    mpu_i2c.Init.DutyCycle       = I2C_DUTYCYCLE_2;            //时钟占空比为low/high = 2:1
    mpu_i2c.Init.OwnAddress1     = 0;                          //设备自身地址1只要和其它地址不一样即可
    mpu_i2c.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;    //寻址模式为7位
    mpu_i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;    //不开启双地址模式
    mpu_i2c.Init.OwnAddress2     = 0;                          //设备自身地址2为0
    mpu_i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;    //不开启广播模式
    mpu_i2c.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;      //禁止时钟延长，主模式下必须禁止
    
    //初始化I2C
    HAL_I2C_Init(&mpu_i2c);
    
    //I2C使能
    __HAL_I2C_ENABLE(&mpu_i2c);
}

/**
  * @brief   利用I2C写一个字节到 MPU6050中
  * @param   
  *		@arg pBuffer:缓冲区指针
  *		@arg WriteAddr:写地址 
  * @retval  无
  */
uint32_t I2C_ByteWrite(uint8_t* pBuffer, uint8_t WriteAddr)
{
	HAL_StatusTypeDef status = HAL_OK;

	status = HAL_I2C_Mem_Write(&mpu_i2c, MPU6050_SLAVE_ADDRESS, (uint16_t)WriteAddr, I2C_MEMADD_SIZE_8BIT, pBuffer, 1, 1000); 

	/* Check the communication status */
	if(status != HAL_OK)
	{
        printf("写入失败");
	/* Execute user timeout callback */
	//I2Cx_Error(Addr);
	}
	while (HAL_I2C_GetState(&mpu_i2c) != HAL_I2C_STATE_READY)
	{
		
	}


//	while (HAL_I2C_IsDeviceReady(&mpu_i2c, MPU6050_SLAVE_ADDRESS, 300, 300) == HAL_TIMEOUT);

	/* Wait for the end of the transfer */
	while (HAL_I2C_GetState(&mpu_i2c) != HAL_I2C_STATE_READY)
	{
		
	}
	return status;
}


/**
  * @brief   从MPU6050里面读取一块数据 
  * @param   
  *		@arg pBuffer:存放从EEPROM读取的数据的缓冲区指针
  *		@arg WriteAddr:接收数据的EEPROM的地址
  *     @arg NumByteToWrite:要从EEPROM读取的字节数
  * @retval  无
  */
uint32_t I2C_BufferRead(uint8_t* pBuffer, uint8_t ReadAddr, uint16_t NumByteToRead)
{
	HAL_StatusTypeDef status = HAL_OK;
	
	status=HAL_I2C_Mem_Read(&mpu_i2c,MPU6050_SLAVE_ADDRESS,ReadAddr, I2C_MEMADD_SIZE_8BIT, (uint8_t *)pBuffer, NumByteToRead,0xffff);

	return status;
}











