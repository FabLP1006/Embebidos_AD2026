# EMG_FSR: MyoGrip EMG + 4 FSR + pulso + temperatura (NUCLEO-H755ZI-Q, CM7, FreeRTOS)

Estado al 27/nov/2026: compila y enlaza sin advertencias en el código propio.

**27/nov/2026: integración de los sensores I2C del repo `Embebidos_AD2026`.**
- MAX30102 (pulso y SpO₂) en I2C2 y MLX90614 (temperatura) en I2C1, como dos tareas de FreeRTOS.
- **IOC configurado y regenerado el 27/nov/2026** según [`INTEGRACION_FINAL_IOC.md`](INTEGRACION_FINAL_IOC.md): I2C1 en PB8/PB9 e I2C2 en PB10/PB11, a 100 kHz (Timing `0x00707CBB`), en drenaje abierto, con interrupciones de evento y error a prioridad 5. Además, DMA de ADC2 sin FIFO.
- Revisado el código generado: se conservaron todos los USER CODE y compila sin advertencias. Los `hi2c1`/`hi2c2` de `main.c` sustituyen a los débiles de `vitals_task.c`.
- **Primera prueba en tarjeta (27/nov/2026, modo SIM, FSR y EMG sin alimentar):**
  - `# vit i2c1=interrupcion reposo=11 i2c2=interrupcion reposo=11 max30102_id=0x15`, con `errores_pec=0` y `errores_i2c=0`.
  - Pila libre: Pulso 412 de 512 palabras y Temp 322 de 384.
  - Termómetro: `temp_estado=1`, Ta ≈ 22.8 °C y Tobj ≈ 22.1–22.7 °C (apuntando al cuarto). Se actualiza cada 500 ms.
  - Oxímetro: `pulso_estado=1` (sin dedo), como se esperaba. Las dos primeras filas dan 0 mientras se configura el MAX30102 (~100 ms).
  - EMG (SIM) y FSR (en 0, sin alimentar) se comportan igual que antes, todos los errores en 0.
  - La primera línea dio `fs_pulso=11.76`. Era un error del cálculo, no del sensor: dividía ~2.4 s de muestras entre 5 s. Ya está corregido: la primera línea muestra `---`, y desde la segunda debe salir ~25.00.
- **Segunda prueba (27/nov/2026, con el dedo, poniéndolo y quitándolo):**
  - `fs_pulso` dio 24.75–24.95 Hz, `errores_i2c=0` y la primera línea mostró `---`, como debía.
  - EMG/FSR: `retraso=0,0,0,0` y todos los errores en 0. `lat_muestra_max_us` = 83–84 µs, igual que antes de los sensores I2C (85).
  - Heap libre mínimo: 26,184 B.
  - **El pulso no funcionó:**
    - `pulso_estado` se quedó en 2 (midiendo), con `bpm=0` y PI en 0, durante hasta 10 s seguidos con el dedo puesto.
    - El PI solo subió justo antes de quitar el dedo (por ejemplo 2014 → 1246, decayendo) y luego pasó a 1.
    - En la PC, con el mismo algoritmo, una señal sana (DC de 110k con 1 % de pulso) da PI ≈ 0.8 % desde los 4 s y 71 BPM a los 8 s. Una IR **saturada** (plana en 262143) da exactamente lo que se vio: estado 2, PI en 0 y nunca BPM.
    - Hipótesis de entonces: saturación, por luz ambiental o por LEDs demasiado fuertes. Se descartó la lectura de la FIFO: es igual a la del repo y `fs_pulso` sale bien. **La tercera prueba descartó la saturación** (ver abajo).
  - Se agregaron a `# vit` los crudos `ir=min-max rojo=min-max` para confirmarlo antes de cambiar la configuración del sensor.
- **Tercera prueba (27/nov/2026, dedo quieto; primero descubierto ~106 s y luego tapado con la mano ~43 s):**
  - **El pulso funciona y no hay saturación.** Con el dedo, `ir` quedó en 98–132k y `rojo` en 94–109k, cerca de la mitad de la escala (262143).
  - Primer BPM a los ~8.5 s de poner el dedo, como en la simulación (8 s): 2 s de asentamiento más 5 intervalos entre latidos.
  - **Tapado:** BPM 54–88, SpO₂ 95–97 %, PI 0.3–1.3 % y nunca estado 3. `ir` estable en 125–132k, con 2–4k cuentas entre mínimo y máximo de cada ventana de 5 s.
  - **Descubierto:**
    - BPM casi siempre 58–88 y SpO₂ 94–96 %.
    - Hubo valores sueltos raros: 110–118 BPM con SpO₂ 88 % y algunos de 49–56 BPM.
    - Hubo 5 ratos en estado 3 (2 s sin detectar latido).
    - En esas ventanas `ir` varió hasta 11–17k cuentas, lo que apunta a movimiento o cambios de luz.
  - Conclusión: la configuración del MAX30102 (LEDs a 7.2 mA, rango 4096 nA) **se queda como está**. Lo que más mejora la lectura es tapar la luz del cuarto y no mover el dedo.
  - La falla de la segunda prueba no tiene una causa confirmada. Lo más probable es que el dedo estuviera muy apretado: un dedo apretado se queda sin pulso visible (PI en 0), y esta vez no hubo saturación.
  - SpO₂ usa la curva típica de Maxim (`1.5958·R² − 34.6597·R + 112.69`) sin calibrar con este módulo. Sirve para ver tendencias, no como medición clínica.
  - Termómetro: al tapar con la mano, `t_obj` subió a 33.5 °C (la mano quedó frente al GY-906; es temperatura normal de piel) y volvió a ~22 °C al retirarla.
  - EMG/FSR sin cambios: `lat_muestra_max_us` 77–85 µs, `retraso=0,0,0,0`, errores en 0 y heap libre mínimo de 26,184 B.
- **P-14 con todos los sensores (27/nov/2026, 10 min, `p14_20261127_172342.csv`): aprobada.** Detalle en la fila P-14 de `REGISTRO_PRUEBAS.md`.
  - fs = 2005.95 Hz con el reloj de la PC (+0.30 %, del HSI). 0 huecos y todos los errores en 0, incluidos `errores_i2c` y `errores_pec`. `retraso` igual en los 120 `# diag`.
  - `lat_fila_max_us` ≤ 33.7 ms (< 50 ms). Pila y heap estables; heap libre mínimo 26,184 B.
  - `lat_muestra_max_us` subió de 85 a 115 µs; pasó de 99 µs en 55 de 120 ventanas. No hubo pérdidas: el muestreo lo dispara TIM6 y la cola aguanta 512 muestras.
  - **Hallazgo: artefacto de 10 Hz en el EMG ligado al MAX30102.**
    - Una ventana de cada dos (cada 100 ms) trae más ruido: `rms_ac` mediana 276 contra 94, y 1,931 de las 2,017 filas con `riel` (5–6 muestras por ventana, unos 2.5–3 ms; hasta 19).
    - Cada 100 ms es cuando la tarea de pulso lee la FIFO por I2C2 (`PULSE_PERIOD_MS`).
    - En las tres capturas del 26/nov, sin sensores I2C, ventanas pares e impares daban igual.
    - Con el dedo en el sensor es mucho más fuerte (284/96) que sin dedo (113/85). De 125 a 200 s desapareció aun con el dedo puesto.
    - Hipótesis: la actividad del módulo pasa al cuerpo por el dedo y los electrodos la recogen. Falta confirmarlo.
    - No es la fuerza que se usó (brazo y un FSR). Las 86 filas con `riel` en ventanas impares sí coinciden con contracción (`rms_ac` 500–1,200). Pero de 341 a 515 s, con FSR3 a ~1 N, las impares quedaron en reposo (~95) mientras las pares tocaban el riel una tras otra.
  - FSR: según el equipo, no se usaron los cuatro a la vez; por eso FSR2 quedó en 0.
  - El visualizador ahora abre capturas guardadas (`--abrir` o **Abrir captura…**) con EMG, `riel` y cada FSR, y arma la tabla de pulso, SpO₂ y temperatura por minuto. Ver `LEEME_GRAFICAS.md`.
- Falta: confirmar el artefacto (EMG en reposo con y sin el dedo en el MAX30102), comparar el BPM con un conteo manual o un oxímetro comercial y una funda o carcasa oscura para el sensor.

**Primera prueba en tarjeta (26/nov/2026, modo SIM):**
- Sale el encabezado de 25 columnas y todos los contadores de error están en 0.
- Respondieron f4 (hasta 21.5 N) y f3 (hasta 8.3 N).
- Cada valor de fuerza salía repetido en dos filas. Era la FIFO del DMA1; ya se corrigió en código.

**Segunda prueba (26/nov/2026, modo SIM, con DMA1 en modo directo):**
- La fuerza cambia en cada fila y ya no sale en pares.
- `# reloj`: 64 MHz, 32 MHz, PSC 63, ARR 499.
- `# diag` a los 5 s: 10018 muestras (2 kHz), 100 bloques (20 Hz), 50 de cada evento DMA y `retraso=0,0,0,0`.
- Confirmado A2 → f2: con presión sostenida da unos 18.5–19.7 N, nivel 3.

**Mapeo completo (26/nov/2026, modo SIM, un FSR a la vez):**

| Pin | Columna | Pico | Diafonía |
|---|---|---|---|
| A1 | f1 | ~30 N | ninguna |
| A2 | f2 | ~20 N | ninguna |
| A3 | f3 | ~10 N | ninguna |
| A4 | f4 | ~22 N | ninguna |

- En reposo los cuatro dan 0 exacto. Un toque ligero en A4 dio 10–40 mN, nivel 0.
- Ojo: el NODO y los electrodos estaban puestos, pero las filas eran `SIM`. En SIM el ADC1 no se lee, y las columnas EMG son la señal sintética (100 ↔ 902 cada 3 s). El EMG real empieza con `r`.
- `# diag` entre 150 s y 320 s:
  - 340,000 muestras en 170,000 ms = 2000.0 Hz.
  - 3400 bloques = 20.0 por segundo.
  - `retraso=0,0,0,0` y `errores_fsr=0` durante más de 5 min.
  - Ojo: TIM6 y el tick de HAL salen del mismo HSI, así que esto comprueba que van parejos, no la frecuencia absoluta. Esa la mide P-14 con el osciloscopio.
- Arriba de ~20 N el FSR 402 sale de su rango nominal y el modelo solo extrapola. Para valores en newtons hay que calibrar con pesos.
- Pendiente: la captura P-13 con un FSR. El modo `r` con el NODO y P-14 ya se hicieron (ver la sección P-14).

## Arquitectura

| Tarea | Prioridad | Qué hace | Archivo |
|---|---|---|---|
| EMG | 40 | Recibe cada muestra del ADC1 (PA3) por cola y arma ventanas de 100 muestras (50 ms) | `myogrip.c`, `emg_signal.c` |
| FSR | 32 | La despiertan los DMA (mitad o buffer completo); empareja ADC2 y ADC3 y calcula la fuerza | `fsr_task.c`, `fsr_signal.c` |
| Serial | 24 | Una fila CSV por ventana EMG con la fuerza del último bloque FSR y los últimos valores de pulso y temperatura; lee los comandos s/r/c/h. Es la única que escribe en la UART | `myogrip.c` |
| Pulso | 20 | Cada 100 ms vacía la FIFO del MAX30102 (I2C2) y calcula pulso, SpO₂ e índice de perfusión | `vitals_task.c`, `pulse_signal.c` |
| Temp | 20 | Cada 500 ms lee Ta y Tobj del MLX90614 (I2C1) y valida el PEC | `vitals_task.c` |
| UI | 16 | Botón B1 (recalibrar) y LEDs del EMG | `myogrip.c` |

- Las ISR solo encolan (EMG) o notifican (FSR). El cálculo corre en las tareas.
- TIM6 (2 kHz, TRGO) dispara los tres ADC. Cada bloque FSR de 100 disparos cae en la misma ventana de 50 ms que el EMG.
- La fuerza va en el último bloque terminado, con un desfase de 50 ms como máximo.
- **Inicialización (antes del planificador):** `MyoGrip_Init()` en `main.c` (USER CODE 2) hace cuatro cosas:
  1. Revisa el IOC.
  2. Fija TIM6 a partir de `EMG_SAMPLE_HZ`.
  3. Calibra ADC1, ADC2 y ADC3.
  4. Arma los DMA del FSR.
- **Indicación de errores con el LED rojo:**
  - Parpadeando: el IOC no coincide con lo esperado. El motivo también sale por serial como `# ERROR CONFIG`.
  - Fijo: `Error_Handler`, o falta de memoria o desbordamiento de pila en FreeRTOS.

| Señal | Pin | Zio | ADC |
|---|---|---|---|
| EMG | PA3 | A0 | ADC1 IN15, 12 bits, interrupción |
| FSR1 | PC0 | A1 | ADC3 Rank 1 (IN10), 16 bits, BDMA |
| FSR2 | PC3_C | A2 | ADC3 Rank 2 (IN1) |
| FSR3 | PB1 | A3 | ADC2 Rank 1 (IN5), 16 bits, DMA1 |
| FSR4 | PC2_C | A4 | ADC3 Rank 3 (IN0) |
| 5V (opcional) | PF10 | A5 | ADC3 Rank 4 (IN6); activar `FSR_USE_5V_MONITOR` y poner 4 conversiones en el IOC |
| MLX90614 SCL / SDA | PB8 / PB9 | D15 / D14 | I2C1, 100 kHz |
| MAX30102 SCL / SDA | PB10 / PB11 | — | I2C2, 100 kHz |

## Cambios pendientes en el IOC (y regenerar)

1. ~~USART3 en PD8 (TX) / PD9 (RX)~~. **Hecho el 26/nov/2026.**
2. ~~SYS > Timebase Source = TIM7~~. **Hecho el 26/nov/2026.** Es el aviso de CubeMX: con FreeRTOS no conviene que HAL comparta SysTick con el planificador. TIM7 no se usa en nada más. TIM6 ya no depende de `HAL_TIM_PeriodElapsedCallback` (se atiende en `TIM6_DAC_IRQHandler`, USER CODE), así que el callback que CubeMX genere en `main.c` para TIM7 no choca con nada.
3. ~~TIM6: Period = 499 y Trigger Event Selection = Update Event~~. **Hecho el 26/nov/2026.** El código ya lo fuerza, pero así el IOC queda coherente.
4. ~~DMA de ADC2 en modo directo (DMA Settings → ADC2 → quitar *Use Fifo*)~~. **Hecho el 27/nov/2026.** El código lo sigue forzando en `Fsr_Init()` por si alguien lo vuelve a activar. Con FIFO (umbral lleno = 8 muestras) la mitad 0 llegaba 2 ms tarde y cada valor de fuerza salía repetido en dos filas (primera prueba en tarjeta, 26/nov/2026).
5. **Opcional:** borrar `defaultTask` en FreeRTOS > Tasks; hoy se borra sola al arrancar.
6. **No activar** en el IOC los ganchos de FreeRTOS (stack overflow, malloc failed). Ya están en `myogrip.c`, y el heap de 48 KB se fija en `FreeRTOSConfig.h` (USER CODE Defines).
7. ~~I2C1 (PB8/PB9) e I2C2 (PB10/PB11) con sus interrupciones de evento y de error~~. **Hecho el 27/nov/2026** ([`INTEGRACION_FINAL_IOC.md`](INTEGRACION_FINAL_IOC.md)).

Lo que **no** regenera CubeMX, y por eso se conserva:
- `myogrip.*`, `emg_signal.*`, `fsr_signal.*`, `fsr_task.*`, `vitals_task.*`, `pulse_signal.*`.
- Las secciones USER CODE.
- La sección `.ram_d3` agregada en `CM7/STM32H755ZITX_FLASH.ld`. Ahí vive el buffer del BDMA, porque el BDMA solo ve la SRAM4.

## Salida serial (115200, 33 columnas)

Las 19 columnas del EMG, iguales que antes, y después:

`f1_mN,f2_mN,f3_mN,f4_mN,fsr_nivel,errores_fsr,pulso_estado,bpm,spo2,pi_x100,temp_estado,t_amb_cC,t_obj_cC,errores_i2c`

El significado de las 8 columnas de pulso y temperatura, y de la línea `# vit`, está en la sección 5 de `INTEGRACION_FINAL_IOC.md`.

- La fuerza está en mN, con el modelo aproximado del FSR 402 (±25 % entre piezas; hay que calibrar con pesos).
- `fsr_nivel`: 0 = nada, 1 = ligero, 2 = medio, 3 = fuerte, según el sensor con más fuerza.
- `errores_fsr`: bloques perdidos, ADC2 y ADC3 desincronizados y errores de ADC o DMA. Debe quedarse en 0.

El visualizador (`RETO/RETO/MyoGrip_EMG/Visualizador`) acepta los formatos de 19, 25 y 33 columnas, y los guarda completos en el CSV. Grafica la fuerza y muestra el pulso y la temperatura en la línea de texto.

**Líneas de diagnóstico (temporales).** Las ignora el visualizador porque no empiezan con SIM/ADC.
- Al arrancar sale `# reloj sysclk=... pclk1=... tim6 psc=... arr=...`. Lo esperado es 64000000, 32000000, 63 y 499.
- Cada 5 s sale `# diag ...` con contadores acumulados. Entre dos líneas deben subir 10000 muestras, 100 bloques y 50 de cada evento DMA.
- `retraso=a,b,c,d`: en qué muestra (módulo 100) llegó el último evento de cada tipo, en el orden ADC2 mitad, ADC2 completo, ADC3 mitad, ADC3 completo. Los cuatro deben ser iguales. Si uno queda 4 arriba, la FIFO sigue activa.

## P-14: validar adquisición y RTOS

Según `REGISTRO_PRUEBAS.md`, hay que medir la frecuencia de muestreo real, las pérdidas, la latencia y la estabilidad, y compararlas con lo nominal. Se hace en modo **ADC** (`r`), no en SIM.

**Recursos en el firmware:**

| Recurso | Dónde | Qué da |
|---|---|---|
| D13 = PA5 (CN7 pin 10) | Cambia en cada muestra | Onda cuadrada de fs/2. Debe dar 1.000 kHz, o sea fs = 2.000 kHz |
| D12 = PA6 (CN7 pin 12) | Sube con la última muestra de cada ventana y baja al enviar su fila | Pulso de 20 Hz; el ancho es la latencia de la ventana hasta la PC |
| `# p14` cada 5 s | Serial | `lat_muestra_max_us` (ISR → tarea EMG) y `lat_fila_max_us` (última muestra → fila enviada), ambos el máximo de los últimos 5 s; también la pila libre mínima por tarea (palabras) y el heap libre mínimo (bytes) |
| `# diag` cada 5 s | Serial | Muestras, bloques FSR, eventos DMA y retrasos |
| `p14_registro.py` | `RETO/RETO/MyoGrip_EMG/Visualizador` | Graba cada línea con el reloj de la PC, que no depende del HSI de la Nucleo, y saca el resumen |

GND para la punta del osciloscopio: el pin rotulado GND en CN7, junto a D13 (pin 8).

**Procedimiento:**
1. Carga el firmware y mándale `r`. El NODO va conectado como en P-09b.
2. **Frecuencia con osciloscopio.** Canal 1 en D13. Mide la frecuencia. También vale ver el periodo con cursores a lo largo de varios ciclos.
   - Esperado: 1.000 kHz, que equivale a fs = 2.000 kHz.
   - Anota la frecuencia y el jitter de los flancos. El jitter es de la ISR, no del muestreo: el ADC lo dispara TIM6 por hardware.
3. **Latencia con osciloscopio.** Canal 2 en D12. Mide el ancho del pulso.
   - Esperado: unos 2 ms de espera para el bloque FSR más el envío de la fila (~90 caracteres a 115200, unos 8 ms), del orden de 10 ms.
   - Compáralo con `lat_fila_max_us`.
4. **Frecuencia sin osciloscopio, y estabilidad.**
   - Cierra el visualizador y corre `python p14_registro.py COM4 600` (10 min). El mismo programa graba los datos.
   - Al final imprime:
     - fs medida con el reloj de la PC, con su desviación en % y ppm;
     - huecos y repeticiones en el contador de muestras;
     - los contadores de error máximos;
     - el reloj Nucleo/PC;
     - el peor caso de latencias, pila y heap.
   - `python p14_registro.py --analizar p14_xxx.csv` repite el análisis de un registro ya guardado.
5. **Criterios propuestos:**
   - fs = 2000 Hz ± 1 %;
   - 0 huecos, 0 repeticiones y todos los contadores de error en 0 durante 10 min;
   - `retraso` igual en los cuatro eventos;
   - pila libre de cada tarea > 0 y estable;
   - heap libre mínimo estable;
   - `lat_fila_max_us` < 50,000, es decir, la fila sale antes de que termine la siguiente ventana.

**Resultado (26/nov/2026), aprobada.** Detalle en la fila P-14 de `REGISTRO_PRUEBAS.md`.
- **Osciloscopio, modo ADC:** D13 ≈ 1 kHz y pulso D12 ≈ 9.5 ms. Casi todo el pulso es la UART: 2 ms + ~7.5 ms.
- **Registro de 10 min con electrodos:**
  - fs = 2005.42 Hz con el reloj de la PC (+0.27 %, del HSI);
  - 0 huecos y todos los errores en 0;
  - latencia de fila: mediana 9.7 ms, peor caso 36 ms (cuando coincide con las líneas de diagnóstico);
  - ISR → tarea: máximo 85 µs;
  - pila y heap con margen amplio.
- **Para la versión final:**
  - Quitar o apagar las líneas `# diag`/`# p14`; con eso desaparece el peor caso de 36 ms.
  - Usar fs = 2005 Hz en el análisis espectral, o pasar a HSE.
  - Considerar que con contracciones fuertes la señal toca el riel del ADC.

**Repetida 27/nov/2026 con pulso y temperatura: aprobada.** fs = 2005.95 Hz, 0 huecos, errores en 0, `lat_fila_max` ≤ 33.7 ms y `lat_muestra_max` 115 µs. Apareció un artefacto de 10 Hz en el EMG ligado al MAX30102; detalle al inicio de este archivo y en `REGISTRO_PRUEBAS.md`.

Por seguridad, con electrodos en el cuerpo la laptop va desconectada del cargador. Por USB, la tierra de la Nucleo queda unida a la de la PC.

## Prueba en banco

1. Corregir el IOC (punto 1 en adelante), regenerar, compilar y cargar el CM7 y el CM4.
2. En la terminal o el visualizador: encabezado de 25 columnas y filas `SIM` a 20 Hz. `f1..f4` = 0 sin presión y `errores_fsr` = 0.
3. Presionar cada FSR: solo cambia su columna, en el orden A1 → f1, A2 → f2, A3 → f3, A4 → f4.
4. `r`: modo ADC real del EMG. Con el NODO conectado, `base` ≈ 2,000–2,050, como en P-09b.
5. Repetir una captura de P-13 con un FSR en la mano y revisar que la fuerza y `rms_ac` suben juntas.
6. P-14: ver la sección "P-14: validar adquisición y RTOS".
