# Integración final: EMG + 4 FSR + pulso (MAX30102) + temperatura (MLX90614)

Propuesta del 27/nov/2026, sobre el proyecto `EMG_FSR` (NUCLEO-H755ZI-Q, CM7, FreeRTOS).

- **Fuente:** repo `FabLP1006/Embebidos_AD2026`, commit `940bb39`, archivos `CodigoparaDanna.ioc` y `main.c`. Los `.md` del repo no se usaron.
- La estructura del EMG con FreeRTOS sigue mandando. Los sensores I2C se agregan como dos tareas nuevas y **no pueden detener ni frenar** al EMG ni a los FSR.

El código ya está listo y compila **antes** de cambiar el IOC: mientras no existan I2C1/I2C2, los sensores aparecen como "sin configurar". Este documento es la lista para el IOC.

**Estado al 27/nov/2026:**
- El IOC ya se configuró y regeneró así.
- Se revisó lo generado: `.ioc`, `main.c`, `stm32h7xx_hal_msp.c`, `stm32h7xx_it.c`, `hal_conf` y los enlaces de `.project`. Coincide con las secciones 2 y 3, y compila sin advertencias.
- Falta la prueba en tarjeta (sección 6).

## 1. Qué se agrega

| Sensor | Bus | Pines (SCL / SDA) | Dirección | Qué mide | Cada cuánto se lee |
|---|---|---|---|---|---|
| MAX30102 (oxímetro) | **I2C2** | PB10 / PB11 | 0x57 | Rojo/IR a 100 muestras/s con promedio de 4 → 25 Hz: pulso, SpO₂ e índice de perfusión | 100 ms; la FIFO del sensor guarda 1.28 s |
| MLX90614 (GY-906, IR) | **I2C1** | PB8 / PB9 | 0x5A | Temperatura ambiente y de objeto, validando el PEC (CRC-8) | 500 ms |

- PB8 y PB9 son los pines I2C estándar del conector Zio: D15 y D14, en CN7. PB10 y PB11 van donde ya los tenían conectados.
- Revisen la serigrafía contra el manual UM2408.
- Usar dos buses permite que cada sensor tenga su propia tarea, sin compartir bus ni usar mutex. Además, si un sensor traba su bus, el otro sigue funcionando.

### Mapa completo de pines del proyecto final (contexto CM7)

| Función | Pin(es) | Periférico |
|---|---|---|
| EMG (NODO) | PA3 (A0) | ADC1 IN15 |
| FSR1, FSR2, FSR4 | PC0 (A1), PC3_C (A2), PC2_C (A4) | ADC3 IN10, IN1, IN0 + BDMA |
| FSR3 | PB1 (A3) | ADC2 IN5 + DMA1 Stream0 |
| Monitor de 5 V (opcional) | PF10 (A5) | ADC3 IN6 |
| Disparo común de los ADC | — | TIM6 (TRGO, 2 kHz) |
| Base de tiempo de HAL | — | TIM7 |
| UART al ST-LINK | PD8 / PD9 | USART3 |
| LEDs y botón | PB0, PE1, PB14 y PC13 | GPIO |
| Pruebas P-14 (osciloscopio) | PA5 (D13), PA6 (D12) | GPIO, configurados en código |
| **Temperatura** | **PB8 / PB9** | **I2C1** |
| **Pulso** | **PB10 / PB11** | **I2C2** |

No hay choques: ninguno de los cuatro pines I2C estaba en uso.

## 2. Cambios en `EMG_FSR.ioc`

Todo va en el contexto **Cortex-M7**. En cada periférico, marca la casilla de **Runtime context: Cortex-M7**.

1. **Connectivity → I2C1 → Mode: I2C**
   - **Parameter Settings:**
     - *I2C Speed Mode* = **Standard Mode**, *Frequency* = **100 kHz**. CubeMX calcula *Timing* = `0x00707CBB` con PCLK1 = 32 MHz, igual que en el repo.
     - *Analog Filter* = Enabled y *Digital Filter* = 0.
     - El MLX90614 es SMBus y **no admite Fast Mode**: déjenlo en 100 kHz.
   - **Pines:** confirmen que quedó **PB8 = I2C1_SCL** y **PB9 = I2C1_SDA**. CubeMX a veces propone PB6/PB7; en ese caso den clic en PB8 y PB9 y asígnenlos.
   - **NVIC Settings:** activen **I2C1 event interrupt** e **I2C1 error interrupt**.
   - **DMA Settings:** nada. No hace falta.
   - **GPIO Settings:**
     - Alternate Function **Open Drain** y velocidad Low.
     - Pull: *No pull-up and no pull-down* si los módulos traen resistencias. El GY-906 y la mayoría de las placas MAX30102 las traen.
     - Si `# vit` da `reposo` distinto de 11 (sección 5), pongan *Pull-up*. Es débil, de ~40 kΩ; lo ideal es una de 4.7 kΩ externa a 3.3 V.
2. **Connectivity → I2C2:** lo mismo, con **PB10 = I2C2_SCL**, **PB11 = I2C2_SDA**, más **I2C2 event** e **I2C2 error interrupt**. El MAX30102 aguanta 400 kHz, pero 100 kHz es lo que ya probaron.
3. **System Core → NVIC:** los cuatro vectores I2C deben quedar con **prioridad de preempción 5 o mayor en número** y *Uses FreeRTOS functions* marcado. Es lo que CubeMX pone por defecto con FreeRTOS.

   | Vector | Prioridad (IOC regenerado el 27/nov/2026) |
   |---|---|
   | TIM6_DAC, BDMA_Channel0, ADC3, DMA1_Stream0 | 5 |
   | ADC (ADC1/ADC2) | 6 |
   | I2C1_EV/ER, I2C2_EV/ER | **5** (sirve de 5 a 15) |
   | TIM7 (tick de HAL), PendSV, SysTick | 15 |

   - Al regenerar, DMA1_Stream0 quedó en 5; antes estaba en 6. Es válido: también llama a FreeRTOS desde la ISR.
   - Con el I2C en 5, sus interrupciones pueden adelantarse a la del ADC1 (prioridad 6), que es la que encola cada muestra EMG. Duran pocos µs y el disparo del ADC sigue siendo por hardware (TIM6), así que el muestreo no se mueve.
   - Si al repetir P-14 `lat_muestra_max_us` sube mucho respecto a los 85 µs anteriores, pongan los cuatro vectores I2C en 7: así el ADC1 los interrumpe a ellos y no al revés.
   - Medido el 27/nov/2026 (P-14 de 10 min): máximo 115 µs, sin pérdidas ni errores. No hace falta cambiarlo; si quieren bajarlo, aprovechen la próxima regeneración del IOC.

   - Si los vectores I2C quedan apagados o con prioridad menor a 5 en número, el firmware **igual funciona**: pasa solo a modo bloqueante y lo avisa en `# vit` como `bloqueante`.
   - Con interrupciones es mejor: la tarea duerme mientras viaja el dato.
4. **RCC / Clock Configuration:** nada que cambiar. El reloj del I2C (I2C123) es PCLK1 = 32 MHz. Si algún día cambian el APB1, regeneren: CubeMX recalcula el *Timing*.
5. **FreeRTOS:** no agreguen tareas en el IOC, porque se crean en el código.
   - El heap sigue fijado en 48 KB desde `FreeRTOSConfig.h`, en USER CODE Defines.
   - P-14 midió 30,136 B libres. Las dos tareas nuevas usan ~4 KB, así que quedan ~26 KB.
6. **Aprovechen la misma regeneración** para los pendientes del LEEME:
   - DMA de ADC2 sin FIFO.
   - Opcional: borrar `defaultTask`.
7. **Generate Code.**

## 3. Después de regenerar

- **`main.c`:**
  - Deben aparecer `I2C_HandleTypeDef hi2c1;` y `hi2c2;`, y las llamadas a `MX_I2C1_Init()` y `MX_I2C2_Init()`.
  - Esas llamadas van antes de USER CODE 2, donde está `MyoGrip_Init()`. CubeMX las pone en ese orden.
  - **No hay que escribir nada en main.c.**
- **`stm32h7xx_it.c`:** deben aparecer `I2C1_EV_IRQHandler`, `I2C1_ER_IRQHandler`, `I2C2_EV_IRQHandler` e `I2C2_ER_IRQHandler`.
- **El código nuevo está en archivos que CubeMX no toca:** `vitals_task.c/.h` y `pulse_signal.c/.h`.
  - En el firmware actual, `hi2c1` y `hi2c2` están declarados como débiles. Cuando CubeMX genere los de verdad en main.c, esos tienen prioridad. Se probó el enlazado en los dos casos.
- **Si CubeIDE vuelve a "perder" los enlaces de Drivers/Middlewares** (errores `undefined reference` de HAL o FreeRTOS): Close Project → Open Project, y luego Clean.

## 4. Lo que hace el firmware

| Tarea | Prioridad | Periodo | Pila | Qué hace |
|---|---|---|---|---|
| EMG | 40 | cada muestra (2 kHz) | 768 | Sin cambios |
| FSR | 32 | cada bloque (50 ms) | 512 | Sin cambios |
| Serial | 24 | cada ventana (50 ms) | 1024 | Escribe la fila CSV con las 8 columnas nuevas. **Es la única que escribe en la UART** |
| **Pulso** | 20 | 100 ms (`vTaskDelayUntil`) | 512 | Lee la FIFO del MAX30102 y aplica el algoritmo del repo **sin cambios** (`pulse_signal.c`) |
| **Temp** | 20 | 500 ms (`vTaskDelayUntil`) | 384 | Lee Ta y Tobj del MLX90614 y valida el PEC |
| UI | 16 | 10 ms | 384 | Sin cambios |

- **Transferencias I2C por interrupción:**
  - `HAL_I2C_Mem_Read_IT` arranca la transferencia y la tarea espera en `xTaskNotifyWait`. El callback de HAL la despierta.
  - Si se agota el tiempo, el periférico se reinicia con `HAL_I2C_DeInit` + `HAL_I2C_Init`.
  - Un NACK de un sensor ausente no reinicia nada.
- **Sensor ausente o desconectado:**
  - Su estado pasa a 0 y la tarea lo reintenta cada 2 s, así que se puede conectar en caliente.
  - El MAX30102 se da por perdido tras 3 lecturas fallidas seguidas.
- **Se tomó del repo sin cambios:**
  - los registros y la configuración del MAX30102: FIFO `0x50`, SpO₂ `0x27`, LEDs `0x24`, modo `0x03`;
  - `hr_process` y `spo2_update`;
  - la fórmula de la temperatura.
- **Equivalencia del algoritmo:** se probó en la PC contra el `main.c` original con una señal de 635 muestras (sin dedo, pulso de 72 BPM con R = 0.5, señal débil, dedo retirado). Dio **0 diferencias** en BPM y SpO₂.
  - Con 25 Hz, la resolución del pulso es de ±1–2 BPM: 72 BPM se lee como 71. Es del algoritmo original.
- **Lo que cambió respecto al repo:**
  - El lazo con `HAL_GetTick` se volvió dos tareas.
  - Los `uart_print` se volvieron columnas del CSV.
  - `HAL_Delay(200)` y los mensajes de arranque se quitaron. El PART_ID y el estado de los buses salen en `# vit`.
  - La "fs medida" cada 10 s ahora es `fs_pulso` en `# vit`.
  - Se agregó el PEC del MLX90614, verificado con el ejemplo de la hoja de datos: lectura `0x3AD2` → PEC `0x30`.

## 5. Salida serial: 33 columnas

Las 25 columnas de siempre, y al final:

| Columna | Significado |
|---|---|
| `pulso_estado` | 0 sin sensor · 1 sin dedo · 2 midiendo · 3 señal débil · 4 ok |
| `bpm` | Pulso. Vale 0 si el estado no es 4 |
| `spo2` | SpO₂ en %. Vale 0 mientras mide o si R sale fuera de 0.3–1.3 |
| `pi_x100` | Índice de perfusión en centésimas de %: 182 = 1.82 % |
| `temp_estado` | 0 sin sensor · 1 ok · 2 error de lectura (PEC o bandera del sensor) |
| `t_amb_cC`, `t_obj_cC` | Temperaturas en centésimas de °C: 3315 = 33.15 °C. Pueden ser negativas |
| `errores_i2c` | Errores acumulados de los dos buses |

- **El visualizador y `p14_registro.py` aceptan 19, 25 y 33 columnas.** El visualizador muestra pulso, SpO₂ y temperaturas en la línea de texto.
- **Línea `# vit`** cada 5 s, desfasada 2.5 s de `# diag` para no juntar texto en la UART:

  `# vit i2c1=interrupcion reposo=11 i2c2=interrupcion reposo=11 max30102_id=0x15 fs_pulso=25.00 lecturas_temp=... errores_pec=0 errores_i2c=0 pila_libre_pulso=... temp=...`

  - `i2c1`/`i2c2`: `sin_cfg` (no está en el IOC), `bloqueante` o `interrupcion`.
  - `reposo`: SCL y SDA al arrancar. **11** = bien, hay pull-up; 00, 01 o 10 = falta pull-up o algo jala la línea a GND; 99 = no medido.
  - `max30102_id`: **0x15**.
  - `fs_pulso`: frecuencia real del MAX30102. Usa su propio oscilador; nominal 25 Hz.
    - La primera `# vit` (a los 2.5 s) muestra `fs_pulso=---`: solo toma la referencia.
    - Antes del arreglo del 27/nov/2026 esa primera línea daba 11.76, porque dividía unos 2.4 s de muestras entre 5 s.
  - `ir=min-max rojo=min-max`: valores crudos del MAX30102 (18 bits, de 0 a 262143) en los últimos 5 s. Se agregó el 27/nov/2026 para diagnosticar.
    - Sin dedo: el repo midió unos 40–51k. Con dedo: unos 110k, y `max - min` de cientos a miles de cuentas (eso es el pulso).
    - Medido con este módulo el 27/nov/2026: con el dedo, `ir` de 98–132k. Quieto y tapado da 2–4k cuentas entre mínimo y máximo; si sube a más de 10k, el dedo se está moviendo o le cambia la luz.
    - Si `ir` llega a 262143, el sensor está **saturado** y no puede ver el pulso.
    - Si con el dedo puesto `ir` anda cerca de 70000 (el umbral de dedo), el algoritmo se reinicia a cada rato y tampoco llega a calcular BPM.
- **Presupuesto de la UART:** la fila pasa a ~120 caracteres, ~10.4 ms a 115200. Eso es ~21 % del canal, y la latencia típica de fila queda en ~12.5 ms, todavía muy debajo de los 50 ms de una ventana.

## 6. Prueba en banco

1. **Antes de tocar el IOC**, carga el firmware actual.
   - Esperado: filas de 33 columnas con `pulso_estado=0` y `temp_estado=0`, y `# vit i2c1=sin_cfg ... i2c2=sin_cfg`.
   - El EMG y los FSR deben seguir igual.
2. **Con el IOC actualizado**, regenera, compila y carga.
   - Esperado en `# vit`: `interrupcion`, `reposo=11` en los dos buses y `max30102_id=0x15`.
3. **Dedo en el MAX30102.** `pulso_estado` pasa de 1 a 2, y a 4 a los ~8.5 s (medido el 27/nov/2026: 2 s de asentamiento + 5 intervalos entre latidos).
   - Compara el pulso con un oxímetro comercial, o cuenta 30 s y multiplica por 2.
   - Mantén el dedo quieto y sin apretar.
   - Tapa el sensor de la luz del cuarto (mano, tela oscura o funda). Descubierto salieron BPM sueltos de 110–118 y ratos en estado 3; tapado, no.
4. **MLX90614 apuntando a la piel**, a 1–2 cm de la frente o la muñeca.
   - Esperado: `t_obj` de ~32–35 °C y `t_amb` cerca de la temperatura del cuarto.
5. **Desconecta un sensor con el sistema corriendo.** Su estado debe pasar a 0 sin que el EMG ni los FSR se enteren. Al reconectarlo, debe volver en ≤ 2 s.
6. **Repite P-14** (`p14_registro.py COM4 600`) con los tres tipos de sensor.
   - Esperado: 0 huecos, todos los errores en 0, `lat_fila_max` < 50 ms y pila y heap estables. El resumen ahora también incluye la última línea `# vit`.
   - Ojo: la primera `# p14` después de un reset da `lat_fila_max_us` ≈ 60,000, porque las primeras filas esperan a que salga el encabezado. Desde la segunda queda en 12–30 ms. Si la captura empieza desde el reset, ese primer valor no cuenta como falla.
   - Hecha el 27/nov/2026: aprobada. Apareció un artefacto en el EMG cada 100 ms (una ventana de cada dos con más `rms_ac` y con `riel`), sobre todo con el dedo en el MAX30102. Ver LEEME.

## 7. Temporización: ¿hace falta otro timer?

**No hace falta otro timer de hardware.** Cada cosa ya tiene su base de tiempo:

| Base de tiempo | Para qué | Por qué así |
|---|---|---|
| **TIM6** (hardware, 2 kHz) | Dispara los tres ADC (EMG y FSR) | El muestreo no depende del software y no tiene jitter |
| **TIM7** (tick de HAL, 1 kHz) | Tiempos límite de HAL: UART e I2C en modo bloqueante | CubeMX lo pidió para no compartir SysTick con FreeRTOS |
| **SysTick** (tick de FreeRTOS, 1 kHz) | `vTaskDelayUntil` de Pulso (100 ms), Temp (500 ms) y UI (10 ms) | Periodos fijos sin acumular retraso. Una transferencia I2C tarda milisegundos y no cabe en una ISR de timer |
| **Oscilador del MAX30102** | Su propio muestreo a 25 Hz | La tarea solo vacía la FIFO; `fs_pulso` mide la frecuencia real |

**Opcionales, por orden de utilidad para la versión final:**

1. **Watchdog (IWDG1)** para un equipo que corre solo.
   - Una tarea supervisora (puede ser la UI, cada 10 ms) refresca el watchdog **solo** si EMG, FSR, Serial, Pulso y Temp avisaron que avanzaron en el último segundo.
   - En el IOC: IWDG1 con un tiempo de ~1–2 s.
   - En código: congelarlo mientras se depura con `__HAL_DBGMCU_FREEZE_IWDG1()`.
   - No está implementado; pídanlo si lo quieren.
2. **HSE de 8 MHz del ST-LINK (bypass)** en lugar del HSI.
   - P-14 midió fs = 2005.4 Hz (+0.27 %) por el HSI. Con HSE queda con precisión de cristal.
   - Cambian el PLL2 del ADC, la UART y el I2C (CubeMX los recalcula), y TIM6 (el código lo recalcula solo).
   - Háganlo como paso aparte y repitan P-14.
3. **Pin INT del MAX30102 a una EXTI** (flanco de bajada, con pull-up), para que el sensor avise cuando la FIFO esté casi llena en lugar de leerla cada 100 ms.
   - Requiere un pin libre con EXTI en el IOC (prioridad ≥ 5), activar `A_FULL_EN` en el registro 0x02 del sensor, y que el callback notifique a la tarea Pulso.
   - A 25 Hz no hace falta: hoy son ~3 transferencias cortas cada 100 ms.
4. **UART a 460800** si se agregan más columnas. Hoy, con 115200, se usa el ~21 % del canal.

## 8. Archivos

| Archivo | Qué es |
|---|---|
| `CM7/Core/Src/pulse_signal.c`, `CM7/Core/Inc/pulse_signal.h` | Algoritmo de pulso y SpO₂ del repo, sin HAL; se prueba en la PC |
| `CM7/Core/Src/vitals_task.c`, `CM7/Core/Inc/vitals_task.h` | Buses I2C, drivers del MAX30102 y del MLX90614, y las tareas Pulso y Temp |
| `CM7/Core/Src/myogrip.c` | Crea las tareas, las 8 columnas nuevas y la línea `# vit` |
| `RETO/RETO/MyoGrip_EMG/Visualizador/myogrip_monitor.py`, `p14_registro.py` | Aceptan 33 columnas |
