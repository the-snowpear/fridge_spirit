#ifndef __BSP_IIC_H
#define	__BSP_IIC_H

#include "stm32f1xx.h"

#define  MPU_I2C    I2C2

#define  I2C_CLK_ENABLE()        __HAL_RCC_I2C2_CLK_ENABLE()
#define  I2C_CLK_DISABLE()       __HAL_RCC_I2C2_CLK_DISABLE()

    /**I2C2 GPIO Configuration
    PB10     ------> I2C2_SCL
    PB11     ------> I2C2_SDA
    */
//宏定义IIC的GPIO
#define  I2C_GPIO_CLK_ENABLE()   __HAL_RCC_GPIOB_CLK_ENABLE()
#define  I2C_SCL_PIN             GPIO_PIN_10
#define  I2C_SDA_PIN             GPIO_PIN_11
#define  I2C_GPIO_PORT           GPIOB

/* 这个地址只要与STM32外挂的I2C器件地址不一样即可 */
#define I2Cx_OWN_ADDRESS7      0X0A 




void MPU_I2C_Config(void);
uint32_t I2C_ByteWrite(uint8_t* pBuffer, uint8_t WriteAddr);                                          
uint32_t I2C_BufferRead(uint8_t* pBuffer, uint8_t ReadAddr, uint16_t NumByteToRead);
#endif /* __BSP_IIC_H */
