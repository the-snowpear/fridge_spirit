/**
  ******************************************************************************
  * @file    bsp_mpu6050.c
  * @author  STMicroelectronics
  * @version V1.0
  * @date    2024-xx-xx
  * @brief    mpu6050驱动
  ******************************************************************************
  * @attention
  *
  * 实验平台:野火 F103 STM32 开发板 
  * 论坛    :http://www.firebbs.cn
  * 淘宝    :https://fire-stm32.taobao.com
  *
  ******************************************************************************
  */ 

#include "./mpu6050/mpu6050.h"
#include "./usart/bsp_debug_usart.h"

#ifdef soft_IIC
    #include "./iic/bsp_soft_i2c.h"
#else
    #include "./iic/bsp_hard_i2c.h"
#endif


/**
 * @brief  Manages error callback by re-initializing I2C.
 * @param  Addr: I2C Address
 * @retval None
 */
static void I2Cx_Error(uint8_t Addr)
{

    /* 重新初始化I2C外设 */
    #ifdef soft_IIC
        i2c_GPIO_Config();
        i2c_Stop();
    #else
        MPU_I2C_Config();
        /* 恢复I2C寄存器为默认值 */
        HAL_I2C_DeInit(&MPU6050_I2C_Handle);
    #endif
    
    MPU6050_Init();
}


#ifdef soft_IIC
/**
 * @brief  读寄存器，这是提供给上层的接口
 * @param  slave_addr: 从机地址
 * @param 	reg_addr:寄存器地址
 * @param len：要读取的长度
 *	@param data_ptr:指向要存储数据的指针
 * @retval 正常为0，不正常为非0
 */
int MPU6050_ReadData(unsigned char slave_addr, unsigned char reg_addr, unsigned short len,
                             unsigned char *data_ptr)
{
	unsigned char i;

    slave_addr <<= 1;
	
	i2c_Start();
	i2c_SendByte(slave_addr);
    
	if(i2c_WaitAck())
        I2Cx_Error(slave_addr);
    
	i2c_SendByte(reg_addr);
	
    if(i2c_WaitAck())
        I2Cx_Error(slave_addr);
	
	i2c_Start();
	i2c_SendByte(MPU6050_SLAVE_ADDRESS+1);
	
    if(i2c_WaitAck())
        I2Cx_Error(slave_addr);
	
	for(i=0;i<(len-1);i++){
		*data_ptr=i2c_ReadByte(1);
		data_ptr++;
	}
	*data_ptr=i2c_ReadByte(0);
	i2c_Stop();
    return 0;
}

/**
 * @brief  写寄存器，这是提供给上层的接口
 * @param  slave_addr: 从机地址
 * @param 	reg_addr:寄存器地址
 * @param len：写入的长度
 *	@param data_ptr:指向要写入的数据
 * @retval 正常为0，不正常为非0
 */
int MPU6050_WriteReg(unsigned char slave_addr, unsigned char reg_addr, unsigned short len,
                              unsigned char *data_ptr)
{
    unsigned short i = 0;
    slave_addr <<= 1;
    i2c_Start();
    i2c_SendByte(slave_addr);
    
    if(i2c_WaitAck())
        I2Cx_Error(slave_addr);
    
    i2c_SendByte(reg_addr);
    
    if(i2c_WaitAck())
        I2Cx_Error(slave_addr);
    
    for(i=0;i<len;i++)
    {
        i2c_SendByte(data_ptr[i]);
        if(i2c_WaitAck())
            I2Cx_Error(slave_addr);
    }
    i2c_Stop();
    return 0;
}
#else


/**
 * @brief  读寄存器，这是提供给上层的接口
 * @param  slave_addr: 从机地址
 * @param 	reg_addr:寄存器地址
 * @param len：要读取的长度
 *	@param data_ptr:指向要存储数据的指针
 * @retval 正常为0，不正常为非0
 */
int MPU6050_ReadData(unsigned char slave_addr, unsigned char reg_addr, unsigned short len,
                             unsigned char *data_ptr)
{

    HAL_StatusTypeDef status = HAL_OK;
    slave_addr <<= 1;
    status = HAL_I2C_Mem_Read(&MPU6050_I2C_Handle, slave_addr, reg_addr, I2C_MEMADD_SIZE_8BIT, data_ptr, len,
                              I2Cx_FLAG_TIMEOUT);
    /* 检查通讯状态 */
    if (status != HAL_OK)
    {
        /* 总线出错处理 */
        I2Cx_Error(slave_addr);
    }
    while (HAL_I2C_GetState(&MPU6050_I2C_Handle) != HAL_I2C_STATE_READY)
    {
    }
    /* 检查SENSOR是否就绪进行下一次读写操作 */
    while (HAL_I2C_IsDeviceReady(&MPU6050_I2C_Handle, slave_addr, I2Cx_FLAG_TIMEOUT, I2Cx_FLAG_TIMEOUT) == HAL_TIMEOUT)
        ;
    /* 等待传输结束 */
    while (HAL_I2C_GetState(&MPU6050_I2C_Handle) != HAL_I2C_STATE_READY)
    {
    }
    return status;
}

/**
 * @brief  写寄存器，这是提供给上层的接口
 * @param  slave_addr: 从机地址
 * @param 	reg_addr:寄存器地址
 * @param len：写入的长度
 *	@param data_ptr:指向要写入的数据
 * @retval 正常为0，不正常为非0
 */
int MPU6050_WriteReg(unsigned char slave_addr, unsigned char reg_addr, unsigned short len,
                              unsigned char *data_ptr)
{
    HAL_StatusTypeDef status = HAL_OK;
    slave_addr <<= 1;
    status = HAL_I2C_Mem_Write(&MPU6050_I2C_Handle, slave_addr, reg_addr, I2C_MEMADD_SIZE_8BIT, data_ptr, len,
                               I2Cx_FLAG_TIMEOUT);
    /* 检查通讯状态 */
    if (status != HAL_OK)
    {
        /* 总线出错处理 */
        I2Cx_Error(slave_addr);
    }
    while (HAL_I2C_GetState(&MPU6050_I2C_Handle) != HAL_I2C_STATE_READY)
    {
    }
    /* 检查SENSOR是否就绪进行下一次读写操作 */
    while (HAL_I2C_IsDeviceReady(&MPU6050_I2C_Handle, slave_addr, I2Cx_FLAG_TIMEOUT, I2Cx_FLAG_TIMEOUT) == HAL_TIMEOUT)
        ;
    /* 等待传输结束 */
    while (HAL_I2C_GetState(&MPU6050_I2C_Handle) != HAL_I2C_STATE_READY)
    {
    }
    return status;
}

#endif

inline void get_ms_user(unsigned long *count) //换壳函数  inline修饰（内联）减少出入栈开销
{
    *count = HAL_GetTick();
}


/**
  * @brief   初始化MPU6050的INT引脚
  * @param   
  * @retval  
  */
void MPU_INT_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin : PtPin */
  GPIO_InitStruct.Pin = MPU6050_INT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(MPU6050_INT_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

}


/**
  * @brief   初始化MPU6050芯片
  * @param   
  * @retval  
  */
void MPU6050_Init(void)
{
  int i=0,j=0;
  uint8_t reg[5]={0x00,0x07,0x06,0x01,0x18};
  //在初始化之前要延时一段时间，若没有延时，则断电后再上电数据可能会出错
  for(i=0;i<1000;i++)
  {
    for(j=0;j<1000;j++)
    {
      ;
    }
  }
	MPU6050_WriteReg(MPU6050_SLAVE_ADDRESS,MPU6050_RA_PWR_MGMT_1,1, &reg[0]);	     //解除休眠状态
	MPU6050_WriteReg(MPU6050_SLAVE_ADDRESS,MPU6050_RA_SMPLRT_DIV ,1, &reg[1]);	    //陀螺仪采样率
	MPU6050_WriteReg(MPU6050_SLAVE_ADDRESS,MPU6050_RA_CONFIG , 1,&reg[2]);	
	MPU6050_WriteReg(MPU6050_SLAVE_ADDRESS,MPU6050_RA_ACCEL_CONFIG ,1, &reg[3]);	  //配置加速度传感器工作在4G模式
	MPU6050_WriteReg(MPU6050_SLAVE_ADDRESS,MPU6050_RA_GYRO_CONFIG,1, &reg[4]);     //陀螺仪自检及测量范围，典型值：0x18(不自检，2000deg/s)
}


