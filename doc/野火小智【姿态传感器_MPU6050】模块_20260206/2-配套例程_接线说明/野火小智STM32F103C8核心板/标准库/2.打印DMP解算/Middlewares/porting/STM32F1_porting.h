
#ifndef MPU6050_MOTION_DRIVER_PORTING_STM32F1_PORTING_H_
#define MPU6050_MOTION_DRIVER_PORTING_STM32F1_PORTING_H_


#include "main.h"
#include "./mpu6050/mpu6050.h"

#ifdef i2c_soft
    #include "./iic/bsp_soft_i2c.h"
#else
    #include "./iic/bsp_hard_i2c.h"
#endif



#endif /* MPU6050_MOTION_DRIVER_PORTING_STM32F1_PORTING_H_ */
