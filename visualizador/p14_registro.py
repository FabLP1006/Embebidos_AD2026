"""P-14: registro con el reloj de la PC para validar la adquisición de MyoGrip EMG+FSR.

Uso (cierra antes el visualizador o cualquier terminal abierta en el puerto):
    python p14_registro.py COM4 600          # 600 s = 10 min; manda 'r' al empezar
    python p14_registro.py --analizar p14_xxx.csv   # repetir el análisis de un registro

Graba cada línea con la hora de la PC en p14_AAAAMMDD_HHMMSS.csv y al final imprime:
  - la frecuencia de muestreo medida con el reloj de la PC (no depende del HSI de la Nucleo),
  - huecos o repeticiones en el contador de muestras,
  - los contadores de error del firmware,
  - las latencias y la memoria libre de las líneas "# p14".
"""
import csv
import sys
import time
from datetime import datetime
from pathlib import Path

NOMINAL_HZ = 2000.0
WINDOW = 100          # muestras por fila
TOLERANCE = 0.01      # criterio de aceptación: ±1 %
ERRORS = {14: 'riel', 15: 'perdidas', 16: 'errores_adc', 17: 'reportes_perdidos',
          18: 'errores_uart', 24: 'errores_fsr', 32: 'errores_i2c'}
ROW_LENGTHS = (19, 25, 33)   # solo EMG; EMG+FSR; EMG+FSR+pulso+temperatura


def key_values(line):
    """'# p14 a=1 b=2' -> {'a': 1, 'b': 2}. Ignora lo que no sea número."""
    out = {}
    for part in line.split()[2:]:
        if '=' in part:
            k, v = part.split('=', 1)
            try:
                out[k] = int(v.split(',')[0])
            except ValueError:
                pass
    return out


def slope(points):
    """Pendiente por mínimos cuadrados de n contra t (muestras por segundo)."""
    m = len(points)
    st = sum(t for t, _ in points); sn = sum(n for _, n in points)
    stt = sum(t * t for t, _ in points); stn = sum(t * n for t, n in points)
    den = m * stt - st * st
    return (m * stn - st * sn) / den if den else float('nan')


def analyze(records):
    """records: lista de (segundos_pc, línea). Devuelve el informe como texto."""
    segments, current, prev = [], [], None
    gaps = repeats = bad = late = 0
    worst = {name: 0 for name in ERRORS.values()}
    modes = set()
    diag, p14, vit = [], [], []
    for t, line in records:
        if line.startswith('# diag'):
            diag.append((t, key_values(line)))
            delays = line.split('retraso=')[-1].split(',') if 'retraso=' in line else []
            if len(set(delays)) > 1:
                late += 1
            continue
        if line.startswith('# p14'):
            p14.append(key_values(line))
            continue
        if line.startswith('# vit'):
            vit.append(line)
            continue
        parts = line.split(',')
        if parts[0] not in ('SIM', 'ADC'):
            continue
        if len(parts) not in ROW_LENGTHS:
            bad += 1
            continue
        try:
            n = int(parts[1])
            values = [int(x) for x in parts[1:]]
        except ValueError:
            bad += 1
            continue
        modes.add(parts[0])
        for idx, name in ERRORS.items():
            if idx < len(parts):
                worst[name] = max(worst[name], values[idx - 1])
        if prev is not None and (parts[0] != prev[0] or n < prev[1]):
            segments.append(current); current = []        # reinicio (s/r/c) o cambio de modo
        elif prev is not None:
            step = n - prev[1]
            if step == 0:
                repeats += 1
            elif step != WINDOW:
                gaps += max(1, step // WINDOW - 1)
        current.append((t, n))
        prev = (parts[0], n)
    if current:
        segments.append(current)

    lines = []
    total_rows = sum(len(s) for s in segments)
    span = records[-1][0] - records[0][0] if records else 0
    lines.append(f'Duración del registro: {span:.1f} s · filas de datos: {total_rows} · modos: {", ".join(sorted(modes)) or "-"}')
    if bad:
        lines.append(f'Líneas mal formadas: {bad}')
    best = max(segments, key=len) if segments else []
    if len(best) >= 2 and best[-1][0] - best[0][0] >= 30:
        fs = slope(best)
        dt = best[-1][0] - best[0][0]
        dev = (fs - NOMINAL_HZ) / NOMINAL_HZ
        ok = abs(dev) <= TOLERANCE
        lines.append(f'Frecuencia con reloj de la PC (tramo continuo de {dt:.0f} s): {fs:.2f} Hz '
                     f'({dev * 100:+.3f} %, {dev * 1e6:+.0f} ppm) -> {"DENTRO" if ok else "FUERA"} de ±{TOLERANCE * 100:.0f} %')
        rate = len(best) / dt
        lines.append(f'Filas por segundo: {rate:.3f} (nominal {NOMINAL_HZ / WINDOW:.0f})')
    else:
        lines.append('Frecuencia: hace falta un tramo continuo de al menos 30 s.')
    if len(segments) > 1:
        lines.append(f'Tramos separados por reinicio o cambio de modo: {len(segments)}')
    lines.append(f'Contador de muestras: huecos {gaps}, repeticiones {repeats} (ambos deben ser 0)')
    lines.append('Errores máximos del firmware: ' + ', '.join(f'{k}={v}' for k, v in worst.items()))
    if len(diag) >= 2:
        (t0, a), (t1, b) = diag[0], diag[-1]
        if 'ms' in a and 'ms' in b and b['ms'] > a['ms']:
            board_hz = (b['muestras'] - a['muestras']) / ((b['ms'] - a['ms']) / 1000)
            pc_hz = (b['muestras'] - a['muestras']) / (t1 - t0)
            blocks = (b.get('bloques_fsr', 0) - a.get('bloques_fsr', 0)) / ((b['ms'] - a['ms']) / 1000)
            clock = ((b['ms'] - a['ms']) / 1000) / (t1 - t0)
            lines.append(f'# diag: {board_hz:.2f} Hz con el reloj de la Nucleo, {pc_hz:.2f} Hz con el de la PC, '
                         f'{blocks:.2f} bloques FSR/s; reloj Nucleo/PC = {clock:.6f}')
        lines.append(f'# diag con retraso desigual entre eventos DMA: {late} de {len(diag)} (debe ser 0)')
    if p14:
        def worst_of(key, fn):
            vals = [d[key] for d in p14 if key in d]
            return fn(vals) if vals else None
        lines.append(f'# p14 (peor caso en {len(p14)} líneas): '
                     f'lat_muestra_max={worst_of("lat_muestra_max_us", max)} us, '
                     f'lat_fila_max={worst_of("lat_fila_max_us", max)} us, '
                     f'pila libre mín EMG/FSR/Serial/UI = {worst_of("pila_libre_emg", min)}/{worst_of("fsr", min)}/'
                     f'{worst_of("serial", min)}/{worst_of("ui", min)} palabras, '
                     f'heap libre mín = {worst_of("heap_libre_min", min)} B')
    if vit:
        lines.append(f'# vit (última de {len(vit)}): {vit[-1][6:]}')
    return '\n'.join(lines)


def record(port, seconds):
    import serial  # pyserial
    name = Path(__file__).with_name(datetime.now().strftime('p14_%Y%m%d_%H%M%S.csv'))
    records = []
    with serial.Serial(port, 115200, timeout=1) as ser, open(name, 'w', newline='', encoding='utf-8') as f:
        out = csv.writer(f)
        out.writerow(['pc_s', 'linea'])
        # El VCP del ST-LINK puede perder lo que se manda justo al abrir el puerto
        # (26/nov/2026 se perdió la 'r'). Se espera, se manda y se confirma el reinicio.
        time.sleep(1.0)
        ser.reset_input_buffer()
        ser.write(b'r'); ser.flush()   # modo ADC real; el firmware recalibra 2 s (no moverse)
        start = time.perf_counter()
        sends, confirmed = 1, False
        print(f"Mandé 'r' (ADC real). Grabando {seconds:.0f} s de {port} en {name.name} (Ctrl+C para terminar antes)")
        try:
            while time.perf_counter() - start < seconds:
                raw = ser.readline()
                t = time.perf_counter() - start
                if not confirmed and t > 3 * sends:
                    if sends < 3:
                        ser.write(b'r'); ser.flush(); sends += 1
                        print("No se vio el reinicio; vuelvo a mandar 'r'.")
                    else:
                        confirmed = True   # se deja de insistir; el análisis lo dirá
                        print("AVISO: la Nucleo no reinició con 'r'. Revisa que nada más tenga abierto el puerto.")
                if not raw:
                    continue
                line = raw.decode('ascii', 'replace').strip()
                if not line:
                    continue
                if not confirmed and line.startswith('ADC,') and line.split(',')[1].isdigit() \
                        and int(line.split(',')[1]) <= 10 * WINDOW:
                    confirmed = True
                    print('Reinicio en modo ADC confirmado; calibrando 2 s.')
                records.append((t, line))
                out.writerow([f'{t:.6f}', line])
                if line.startswith('#'):
                    print(line)
        except KeyboardInterrupt:
            print('Detenido.')
    return records


def load(path):
    with open(path, newline='', encoding='utf-8') as f:
        return [(float(r['pc_s']), r['linea']) for r in csv.DictReader(f)]


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--analizar':
        print(analyze(load(sys.argv[2])))
    else:
        port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
        seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 600
        print(analyze(record(port, seconds)))
