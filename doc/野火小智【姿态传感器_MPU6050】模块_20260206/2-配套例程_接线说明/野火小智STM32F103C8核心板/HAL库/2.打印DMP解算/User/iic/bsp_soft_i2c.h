#ifndef _BSP_I2C_H
#define _BSP_I2C_H

#include <inttypes.h>

#define I2C_WR	0		/* 写控制bit */
#define I2C_RD	1		/* 读控制bit */

/* 定义I2C总线连接的GPIO端口, 用户只需要修改下面4行代码即可任意改变SCL和SDA的引脚 */
#define GPIO_PORT_SOFT_I2C	GPIOB			/* GPIO端口 */
#define SOFT_I2C_GPIO_CLK_ENABLE()   __HAL_RCC_GPIOB_CLK_ENABLE()
#define SOFT_I2C_SCL_PIN		GPIO_PIN_10			/* 连接到SCL时钟线的GPIO */
#define SOFT_I2C_SDA_PIN		GPIO_PIN_11			/* 连接到SDA数据线的GPIO */

void i2c_Start(void);
void i2c_Stop(void);
void i2c_SendByte(uint8_t _ucByte);
uint8_t i2c_ReadByte(uint8_t ack);
uint8_t i2c_WaitAck(void);
void i2c_Ack(void);
void i2c_NAck(void);
uint8_t i2c_CheckDevice(uint8_t _Address);
void i2c_GPIO_Config(void);

#endif
