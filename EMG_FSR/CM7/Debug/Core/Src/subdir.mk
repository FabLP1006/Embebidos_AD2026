################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/emg_signal.c \
../Core/Src/freertos.c \
../Core/Src/fsr_signal.c \
../Core/Src/fsr_task.c \
../Core/Src/main.c \
../Core/Src/myogrip.c \
../Core/Src/pulse_signal.c \
../Core/Src/stm32h7xx_hal_msp.c \
../Core/Src/stm32h7xx_hal_timebase_tim.c \
../Core/Src/stm32h7xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/vitals_task.c 

OBJS += \
./Core/Src/emg_signal.o \
./Core/Src/freertos.o \
./Core/Src/fsr_signal.o \
./Core/Src/fsr_task.o \
./Core/Src/main.o \
./Core/Src/myogrip.o \
./Core/Src/pulse_signal.o \
./Core/Src/stm32h7xx_hal_msp.o \
./Core/Src/stm32h7xx_hal_timebase_tim.o \
./Core/Src/stm32h7xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/vitals_task.o 

C_DEPS += \
./Core/Src/emg_signal.d \
./Core/Src/freertos.d \
./Core/Src/fsr_signal.d \
./Core/Src/fsr_task.d \
./Core/Src/main.d \
./Core/Src/myogrip.d \
./Core/Src/pulse_signal.d \
./Core/Src/stm32h7xx_hal_msp.d \
./Core/Src/stm32h7xx_hal_timebase_tim.d \
./Core/Src/stm32h7xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/vitals_task.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H755xx -DUSE_PWR_DIRECT_SMPS_SUPPLY -c -I../Core/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -I../../Middlewares/Third_Party/FreeRTOS/Source/include -I../../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 -I../../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F -I../../Drivers/CMSIS/RTOS2/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -ftls-model=local-exec -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/emg_signal.cyclo ./Core/Src/emg_signal.d ./Core/Src/emg_signal.o ./Core/Src/emg_signal.su ./Core/Src/freertos.cyclo ./Core/Src/freertos.d ./Core/Src/freertos.o ./Core/Src/freertos.su ./Core/Src/fsr_signal.cyclo ./Core/Src/fsr_signal.d ./Core/Src/fsr_signal.o ./Core/Src/fsr_signal.su ./Core/Src/fsr_task.cyclo ./Core/Src/fsr_task.d ./Core/Src/fsr_task.o ./Core/Src/fsr_task.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/myogrip.cyclo ./Core/Src/myogrip.d ./Core/Src/myogrip.o ./Core/Src/myogrip.su ./Core/Src/pulse_signal.cyclo ./Core/Src/pulse_signal.d ./Core/Src/pulse_signal.o ./Core/Src/pulse_signal.su ./Core/Src/stm32h7xx_hal_msp.cyclo ./Core/Src/stm32h7xx_hal_msp.d ./Core/Src/stm32h7xx_hal_msp.o ./Core/Src/stm32h7xx_hal_msp.su ./Core/Src/stm32h7xx_hal_timebase_tim.cyclo ./Core/Src/stm32h7xx_hal_timebase_tim.d ./Core/Src/stm32h7xx_hal_timebase_tim.o ./Core/Src/stm32h7xx_hal_timebase_tim.su ./Core/Src/stm32h7xx_it.cyclo ./Core/Src/stm32h7xx_it.d ./Core/Src/stm32h7xx_it.o ./Core/Src/stm32h7xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/vitals_task.cyclo ./Core/Src/vitals_task.d ./Core/Src/vitals_task.o ./Core/Src/vitals_task.su

.PHONY: clean-Core-2f-Src

