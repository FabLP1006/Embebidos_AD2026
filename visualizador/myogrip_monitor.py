"""Monitor local de los campos CSV del firmware MyoGrip (19 EMG, 25 con FSR, 33 con pulso y temperatura). Python 3 + tkinter.

También abre capturas guardadas (las de este monitor y las p14_*.csv) para revisarlas con las mismas gráficas,
y arma una tabla con el último valor de pulso, SpO2 y temperatura de cada minuto.
"""
import argparse
import bisect
import csv
import math
import sys
import time
from collections import deque
from pathlib import Path
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

FIELDS = 'modo,muestras,tiempo_ms,adc,mV,media,rms,rms_ac,delta_rms,suavizado,base,umbral,calibrado,activo,riel,perdidas,errores_adc,reportes_perdidos,errores_uart'.split(',')
# Firmware EMG_FSR: 6 columnas más al final (fuerza de los 4 FSR en mN, nivel y errores).
FSR_FIELDS = 'f1_mN,f2_mN,f3_mN,f4_mN,fsr_nivel,errores_fsr'.split(',')
# Firmware con sensores I2C: 8 columnas más (MAX30102 y MLX90614). Temperaturas en centésimas de °C.
VIT_FIELDS = 'pulso_estado,bpm,spo2,pi_x100,temp_estado,t_amb_cC,t_obj_cC,errores_i2c'.split(',')
SIGNED_FIELDS = {'t_amb_cC', 't_obj_cC'}
PULSE_TEXT = {0: 'sin sensor', 1: 'sin dedo', 2: 'midiendo', 3: 'señal débil', 4: 'ok'}
TEMP_TEXT = {0: 'sin sensor', 1: 'ok', 2: 'error de lectura'}
BG, PANEL, TEXT, MUTED = '#101b2b', '#19283c', '#e6eef8', '#a8b7cb'
LIVE_SPAN = 15                      # segundos visibles en vivo
SPANS = {'15 s': 15, '1 min': 60, '5 min': 300, 'Todo': None}   # ventanas de la revisión
ROW_S = 0.05                        # 20 reportes/s: tiempo por fila si el CSV no trae reloj de la PC


def parse_line(line):
    parts = line.strip().split(',')
    if not parts or parts[0] not in ('SIM', 'ADC'):
        return None
    formats = {len(FIELDS): FIELDS, len(FIELDS) + len(FSR_FIELDS): FIELDS + FSR_FIELDS,
               len(FIELDS) + len(FSR_FIELDS) + len(VIT_FIELDS): FIELDS + FSR_FIELDS + VIT_FIELDS}
    names = formats.get(len(parts))
    if names is None:
        raise ValueError('Cantidad de columnas incorrecta')
    row = dict(zip(names, [parts[0]] + [int(p) for p in parts[1:]]))
    if any(row[k] < 0 for k in names[1:] if k not in SIGNED_FIELDS):
        raise ValueError('Valor negativo')
    if row['adc'] > 4095 or any(row[k] not in (0, 1) for k in ('calibrado', 'activo')):
        raise ValueError('Valor fuera de rango')
    # Para el texto de estado: el FSR con más fuerza (0 con el firmware de solo EMG).
    row['fuerza_max'] = max((row[k] for k in FSR_FIELDS[:4] if k in row), default=0)
    return row


def load_capture(path):
    """Lee una captura guardada y devuelve (filas, líneas inválidas, origen del tiempo).

    Cada fila lleva 't' en segundos desde el inicio de la captura:
      - p14_*.csv (columnas pc_s, linea): el reloj de la PC de cada línea;
      - CSV de este monitor (origen_pc, modo, ...): 0.05 s por fila, porque no traen reloj de la PC.
    """
    rows, bad = [], 0
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames or []
        if 'pc_s' in fields and 'linea' in fields:
            clock = 'reloj de la PC'
            for rec in reader:
                line = rec['linea'] or ''
                if line.startswith('#'):
                    continue
                try:
                    row = parse_line(line)
                    t = float(rec['pc_s'])
                except (ValueError, TypeError):
                    bad += 1
                    continue
                if row:
                    row['t'] = t
                    rows.append(row)
        elif 'modo' in fields:
            clock = '0.05 s por fila'
            names = FIELDS + FSR_FIELDS + VIT_FIELDS
            for rec in reader:
                try:
                    row = parse_line(','.join(rec[k] for k in names if rec.get(k) not in (None, '')))
                except (ValueError, TypeError):
                    bad += 1
                    continue
                if row:
                    row['t'] = len(rows) * ROW_S
                    rows.append(row)
        else:
            raise ValueError('No parece una captura de MyoGrip: faltan las columnas "modo" o "pc_s,linea".')
    return rows, bad, clock


class MinuteLog:
    """Se queda solo con la última fila de cada minuto (según 't') e ignora las demás."""

    def __init__(self):
        self.minute = None
        self.last = None

    def push(self, row):
        """Devuelve (minuto, su última fila) cuando llega la primera fila del minuto siguiente."""
        minute = int(row['t'] // 60)
        done = (self.minute, self.last) if self.last is not None and minute != self.minute else None
        self.minute, self.last = minute, row
        return done


def fmt_time(s):
    s = max(0.0, s)
    return f'{int(s // 60)}:{int(s % 60):02d}' if s >= 60 else f'{s:.0f} s'


def envelope(rows, key, xof, start, span, width):
    """Puntos (x, valor). Si hay más filas que píxeles, mínimo y máximo por píxel para no perder picos."""
    if len(rows) <= 2 * width:
        return [(xof(r), r[key]) for r in rows]
    buckets = {}
    for r in rows:
        i = min(width - 1, max(0, int((xof(r) - start) / span * width)))
        v = r[key]
        b = buckets.get(i)
        if b is None:
            buckets[i] = [v, v]
        elif v < b[0]:
            b[0] = v
        elif v > b[1]:
            b[1] = v
    points = []
    for i in sorted(buckets):
        x = start + (i + 0.5) / width * span
        points += [(x, buckets[i][0]), (x, buckets[i][1])]
    return points


def demo_row(n):
    """Datos ilustrativos de PC; no son resultados del sensor ni del firmware."""
    t = n / 20
    calibrated = t >= 2
    active = calibrated and int(t / 3) % 2 == 1
    adc = int(102 + 2 * math.sin(t * 21) + (550 + 45 * math.sin(t * 16) if active else 0))
    delta = abs(adc - 102)
    values = ['SIM', n * 100, n * 50, adc, round(adc * 3300 / 4095), adc,
              adc, 12 if active else 2, delta, adc, 102, 20,
              int(calibrated), int(active), 0, 0, 0, 0, 0]
    row = dict(zip(FIELDS, values))
    force = int(12000 + 3000 * math.sin(t * 2)) if active else 0
    row.update(f1_mN=force, f2_mN=0, f3_mN=0, f4_mN=0, fuerza_max=force)
    return row


class Monitor:
    def __init__(self, root, demo=False):
        self.root = root
        self.port = None
        self.buffer = bytearray()
        self.points = deque(maxlen=1200)
        self.demo = False
        self.demo_n = 0
        self.next_demo = 0
        self.last_received = None
        self.bad_lines = 0
        self.capture = None
        self.writer = None
        self.last_row = None
        self.t0 = None                  # reloj de la PC del primer reporte en vivo
        self.minutes = MinuteLog()
        self.review_rows = None         # filas de una captura abierta (modo revisión)
        self.review_t = []
        self.dirty = True
        root.title('MyoGrip | Monitor EMG')
        # Que quepa en pantallas de laptop (la barra de tareas ocupa ~50 px abajo).
        width = min(1280, root.winfo_screenwidth() - 40)
        height = min(900, root.winfo_screenheight() - 90)
        root.geometry(f'{width}x{height}+15+10')
        root.minsize(960, 620)
        root.configure(bg=BG)
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('TFrame', background=BG)
        style.configure('TLabel', background=BG, foreground=TEXT, font=('Segoe UI', 10))
        style.configure('TButton', padding=7, font=('Segoe UI', 10))
        style.configure('Treeview', background=PANEL, fieldbackground=PANEL, foreground=TEXT, rowheight=22, font=('Segoe UI', 10))
        style.configure('Treeview.Heading', background='#223650', foreground=TEXT, font=('Segoe UI', 9, 'bold'))
        style.map('Treeview.Heading', background=[('active', '#2c3c52')])
        style.map('Treeview', background=[('selected', '#2c4a6e')])
        main = ttk.Frame(root, padding=(16, 10))
        main.pack(fill='both', expand=True)
        header = ttk.Frame(main)
        header.pack(fill='x', pady=(0, 6))
        ttk.Label(header, text='MYOGRIP  /  Actividad muscular', font=('Segoe UI', 18, 'bold')).pack(side='left')
        ttk.Label(header, text='En vivo: ventana de 15 s · 20 reportes/s · EMG en cuentas (0–4095) · fuerza en mN',
                  foreground=MUTED).pack(side='left', padx=(16, 0), pady=(6, 0))
        bar = ttk.Frame(main)
        bar.pack(fill='x')
        self.port_name = tk.StringVar()
        self.combo = ttk.Combobox(bar, textvariable=self.port_name, width=32, state='readonly')
        self.combo.pack(side='left')
        ttk.Button(bar, text='Buscar COM', command=self.refresh_ports).pack(side='left', padx=4)
        ttk.Button(bar, text='Conectar', command=self.connect).pack(side='left', padx=4)
        ttk.Button(bar, text='Desconectar', command=self.disconnect).pack(side='left', padx=4)
        ttk.Button(bar, text='Demo en PC', command=self.start_demo).pack(side='left', padx=4)
        ttk.Button(bar, text='Abrir captura…', command=self.ask_open).pack(side='left', padx=4)
        # Mismo renglón para los comandos en vivo y para la barra de revisión (se alternan).
        slot = ttk.Frame(main)
        slot.pack(fill='x', pady=6)
        self.commands = ttk.Frame(slot)
        self.commands.pack(fill='x')
        self.command_buttons = []
        for label, command in [('SIM en Nucleo', 's'), ('Recalibrar (2 s)', 'c'), ('ADC real', 'r')]:
            button = ttk.Button(self.commands, text=label, command=lambda c=command: self.send(c), state='disabled')
            button.pack(side='left', padx=(0, 8))
            self.command_buttons.append(button)
        self.record_button = ttk.Button(self.commands, text='Guardar CSV…', command=self.toggle_record)
        self.record_button.pack(side='right')
        self.review_bar = ttk.Frame(slot)
        ttk.Label(self.review_bar, text='Revisión', font=('Segoe UI', 10, 'bold'), foreground='#ffcb70').pack(side='left')
        self.span_name = tk.StringVar(value='1 min')
        span_box = ttk.Combobox(self.review_bar, textvariable=self.span_name, values=list(SPANS), width=7, state='readonly')
        span_box.pack(side='left', padx=8)
        span_box.bind('<<ComboboxSelected>>', lambda e: self.update_scale())
        self.offset = tk.DoubleVar(value=0)
        self.scale = ttk.Scale(self.review_bar, from_=0, to=0, variable=self.offset, command=lambda v: self.mark_dirty())
        self.scale.pack(side='left', fill='x', expand=True, padx=8)
        self.range_text = tk.StringVar()
        ttk.Label(self.review_bar, textvariable=self.range_text, width=22).pack(side='left')
        ttk.Button(self.review_bar, text='Cerrar revisión', command=self.close_review).pack(side='left', padx=(8, 0))
        root.bind('<Left>', lambda e: self.step(-1))
        root.bind('<Right>', lambda e: self.step(1))
        self.source = tk.StringVar(value='DESCONECTADO')
        self.values = tk.StringVar(value='Esperando datos')
        self.status = tk.StringVar(value='Usa Demo en PC para ver las gráficas sin tarjeta, o Abrir captura para revisar un CSV guardado.')
        ttk.Label(main, textvariable=self.source, font=('Segoe UI', 13, 'bold'), foreground='#56d9c1').pack(anchor='w', pady=(2, 0))
        ttk.Label(main, textvariable=self.values, font=('Segoe UI', 10)).pack(anchor='w')
        # Las notas de abajo se empacan antes que las gráficas para que nunca queden cortadas.
        ttk.Label(main, text='Reportes resumidos: no muestran la onda completa a 2 kHz. El EMG no mide fuerza ni fatiga; la fuerza viene de los FSR (modelo aproximado).', foreground=MUTED).pack(side='bottom', anchor='w')
        ttk.Label(main, textvariable=self.status, wraplength=1200).pack(side='bottom', anchor='w', pady=(8, 0))
        body = ttk.Frame(main)
        body.pack(fill='both', expand=True)
        side = ttk.Frame(body)
        side.pack(side='right', fill='y', padx=(14, 0))
        charts = ttk.Frame(body)
        charts.pack(side='left', fill='both', expand=True)
        self.charts = []
        for title, series, floor in [
            ('EMG · nivel: una muestra por reporte y media de ventana', [('adc', '#60baff', 'adc'), ('media', '#56d9c1', 'media')], 50),
            ('EMG · amplitud: RMS total y RMS sin componente continua', [('rms', '#bd9cff', 'rms'), ('rms_ac', '#ffcb70', 'rms_ac')], 50),
            ('EMG · activación: RMS respecto al reposo y umbral experimental', [('delta_rms', '#56d9c1', 'delta_rms'), ('umbral', '#ff8097', 'umbral')], 50),
            ('EMG · riel: muestras de la ventana pegadas al tope del ADC (de 100)', [('riel', '#ff6b6b', 'riel')], 10),
            ('Fuerza · cada FSR, en mN (firmware EMG_FSR)', [('f1_mN', '#ff9f5a', 'FSR1'), ('f2_mN', '#ffd166', 'FSR2'),
                                                             ('f3_mN', '#7bdff2', 'FSR3'), ('f4_mN', '#c792ea', 'FSR4')], 50)]:
            canvas = tk.Canvas(charts, bg=PANEL, highlightthickness=0, height=90)
            canvas.pack(fill='both', expand=True, pady=(6, 0))
            canvas.bind('<Configure>', lambda e: self.mark_dirty())
            self.charts.append((canvas, title, series, floor))
        # Tabla por minuto: el último valor de cada minuto; el resto se ignora.
        ttk.Label(side, text='Signos vitales por minuto', font=('Segoe UI', 12, 'bold')).pack(anchor='w', pady=(8, 0))
        ttk.Label(side, text='Último valor de cada minuto; los demás se ignoran. "--" = sin dato válido en ese instante.',
                  foreground=MUTED, wraplength=330).pack(anchor='w', pady=(2, 6))
        ttk.Button(side, text='Copiar tabla', command=self.copy_table).pack(side='bottom', anchor='w', pady=(6, 0))
        self.minute_hint = tk.StringVar()
        ttk.Label(side, textvariable=self.minute_hint, foreground=MUTED, wraplength=330).pack(side='bottom', anchor='w', pady=(6, 0))
        table_frame = ttk.Frame(side)
        table_frame.pack(fill='both', expand=True)
        self.table = ttk.Treeview(table_frame, columns=('min', 'bpm', 'spo2', 'temp', 'pulso'), show='headings', height=8)
        for col, text, width in [('min', 'Minuto', 86), ('bpm', 'BPM', 46), ('spo2', 'SpO₂ %', 60), ('temp', 'T obj °C', 64), ('pulso', 'Pulso', 96)]:
            self.table.heading(col, text=text)
            self.table.column(col, width=width, anchor='center', stretch=False)
        scrollbar = ttk.Scrollbar(table_frame, orient='vertical', command=self.table.yview)
        self.table.configure(yscrollcommand=scrollbar.set)
        self.table.pack(side='left', fill='both', expand=True)
        scrollbar.pack(side='right', fill='y')
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.reset_view()
        self.refresh_ports()
        if demo:
            self.start_demo()
        root.after(50, self.tick)

    def refresh_ports(self):
        try:
            from serial.tools import list_ports
            self.ports = {f'{p.device} — {p.description}': p.device for p in list_ports.comports()}
        except ImportError:
            self.ports = {}
            self.status.set(f'Falta pyserial en {sys.executable}. Instálalo con INSTALAR_SERIAL.bat. La demo funciona sin él.')
        else:
            if self.ports:
                self.status.set(f'{len(self.ports)} puerto(s) encontrado(s). Selecciona STLink Virtual COM Port y pulsa Conectar.')
            else:
                self.status.set('No se encontraron puertos COM. Revisa el cable USB de ST-LINK.')
        self.combo['values'] = list(self.ports)
        if self.ports and self.port_name.get() not in self.ports:
            self.combo.current(0)

    def reset_view(self):
        self.points.clear()
        self.last_row = None
        self.last_received = None
        self.bad_lines = 0
        self.buffer.clear()
        self.t0 = None
        self.minutes = MinuteLog()
        self.table.delete(*self.table.get_children())
        self.minute_hint.set('En vivo se agrega una fila al cerrar cada minuto, contado con el reloj de la PC desde el primer reporte.')
        self.dirty = True

    def disconnect(self):
        if self.port is not None:
            self.port.close()
            self.port = None
        self.demo = False
        if self.review_rows is not None:
            self.close_review()
        for button in self.command_buttons:
            button['state'] = 'disabled'
        self.stop_record()
        self.source.set('DESCONECTADO · gráfica detenida')
        self.status.set('Conexión cerrada.')

    def connect(self):
        name = self.ports.get(self.port_name.get())
        if not name:
            messagebox.showinfo('Puerto COM', 'Conecta la Nucleo por ST-LINK y pulsa Buscar COM.')
            return
        self.disconnect()
        self.reset_view()
        try:
            import serial
            self.port = serial.Serial(name, 115200, timeout=0, write_timeout=0.2)
            for button in self.command_buttons:
                button['state'] = 'normal'
            self.source.set(f'{name} · esperando CSV de la Nucleo')
            self.status.set('Conectado a 115200, 8N1. No abras este COM en otra terminal.')
        except (ImportError, OSError) as exc:
            messagebox.showerror('No se pudo conectar', str(exc))

    def start_demo(self):
        self.disconnect()
        self.reset_view()
        self.demo = True
        self.demo_n = 0
        self.next_demo = time.monotonic()
        self.source.set('DEMO EN PC · datos sintéticos, sin tarjeta')
        self.status.set('Demostración ilustrativa: alterna reposo y activación cada 3 segundos.')

    # ---------- Revisión de capturas guardadas ----------

    def ask_open(self):
        filename = filedialog.askopenfilename(title='Abrir captura guardada', initialdir=str(Path(__file__).parent),
                                              filetypes=[('CSV', '*.csv'), ('Todos', '*.*')])
        if filename:
            self.open_capture(filename)

    def open_capture(self, path):
        try:
            rows, bad, clock = load_capture(path)
        except (OSError, ValueError, csv.Error, UnicodeError) as exc:
            messagebox.showerror('No se pudo abrir', str(exc))
            return
        if not rows:
            messagebox.showinfo('Captura vacía', 'El archivo no tiene filas de datos del firmware.')
            return
        self.disconnect()
        self.reset_view()
        self.review_rows = rows
        self.review_t = [r['t'] for r in rows]
        log = MinuteLog()
        for row in rows:
            done = log.push(row)
            if done:
                self.add_minute(*done)
        if log.last['t'] >= (log.minute + 1) * 60 - 0.5:   # el último minuto cuenta solo si se completó
            self.add_minute(log.minute, log.last)
        if 'bpm' in rows[-1]:
            self.minute_hint.set(f'{len(self.table.get_children())} minuto(s) completo(s). Si el último quedó incompleto, no se muestra.')
        duration = self.review_t[-1] - self.review_t[0]
        rail = sum(r['riel'] > 0 for r in rows)
        self.source.set(f'REVISIÓN · {Path(path).name}')
        self.status.set(f'{len(rows)} filas · {fmt_time(duration)} · riel en {rail} filas · líneas inválidas {bad} · tiempo: {clock}. '
                        'Mueve la barra o usa ← → para recorrer la captura.')
        self.commands.pack_forget()
        self.review_bar.pack(fill='x')
        self.offset.set(0)
        self.update_scale()

    def close_review(self):
        self.review_rows = None
        self.review_t = []
        self.review_bar.pack_forget()
        self.commands.pack(fill='x')
        self.reset_view()
        self.source.set('DESCONECTADO')
        self.values.set('Esperando datos')

    def span(self):
        if self.review_rows is None:
            return LIVE_SPAN
        return SPANS[self.span_name.get()] or max(1.0, self.review_t[-1] - self.review_t[0])

    def update_scale(self):
        if self.review_rows is None:
            return
        top = max(0.0, self.review_t[-1] - self.review_t[0] - self.span())
        self.scale.configure(to=top)
        if self.offset.get() > top:
            self.offset.set(top)
        self.mark_dirty()

    def step(self, direction):
        try:
            if self.review_rows is None or isinstance(self.root.focus_get(), ttk.Combobox):
                return
        except KeyError:        # focus_get falla con la lista desplegable abierta
            return
        top = float(self.scale.cget('to'))
        self.offset.set(min(top, max(0.0, self.offset.get() + direction * self.span() / 2)))
        self.mark_dirty()

    def mark_dirty(self):
        self.dirty = True

    # ---------- Tabla por minuto ----------

    def add_minute(self, minute, row):
        if 'bpm' not in row:
            self.minute_hint.set('Sin columnas de pulso ni temperatura: firmware sin sensores I2C, o demo.')
            return
        ok = row['pulso_estado'] == 4
        bpm = str(row['bpm']) if ok and row['bpm'] else '--'
        spo2 = str(row['spo2']) if ok and row['spo2'] else '--'
        temp = f"{row['t_obj_cC'] / 100:.1f}" if row['temp_estado'] == 1 else '--'
        item = self.table.insert('', 'end', values=(f'{minute}:00–{minute + 1}:00', bpm, spo2, temp,
                                                     PULSE_TEXT.get(row['pulso_estado'], '?')))
        self.table.see(item)

    def copy_table(self):
        lines = ['Minuto\tBPM\tSpO2 %\tT obj °C\tPulso']
        lines += ['\t'.join(str(v) for v in self.table.item(i, 'values')) for i in self.table.get_children()]
        self.root.clipboard_clear()
        self.root.clipboard_append('\n'.join(lines))
        self.status.set(f'Tabla copiada ({len(lines) - 1} minutos). Se puede pegar en Excel o en el reporte.')

    # ---------- En vivo ----------

    def send(self, command):
        if self.port is None:
            return
        if command == 'r' and not messagebox.askokcancel('Entrada ADC real', 'Selecciona ADC real solo tras verificar que SIG está acondicionado a 0–3.3 V. ¿Continuar?'):
            return
        try:
            self.port.write(command.encode('ascii'))
            self.status.set('Comando enviado; esperando los datos para confirmar el estado.')
        except OSError as exc:
            self.disconnect()
            self.status.set(f'Error de comunicación: {exc}')

    def accept(self, row):
        if self.last_row and (row['modo'] != self.last_row['modo'] or row['tiempo_ms'] < self.last_row['tiempo_ms']):
            self.points.clear()
        now = time.monotonic()
        if self.t0 is None:
            self.t0 = now
        row['t'] = now - self.t0
        done = self.minutes.push(row)
        if done:
            self.add_minute(*done)
        self.points.append(row)
        self.last_row = row
        self.last_received = now
        if self.writer:
            try:
                self.writer.writerow(row)
            except OSError as exc:
                self.stop_record()
                messagebox.showerror('Error al guardar CSV', str(exc))
        source = 'DEMO EN PC · DATOS SINTÉTICOS' if self.demo else ('NUCLEO · SIMULACIÓN' if row['modo'] == 'SIM' else 'NUCLEO · ADC REAL')
        state = 'CALIBRANDO' if not row['calibrado'] else ('ACTIVO' if row['activo'] else 'REPOSO')
        self.source.set(f'{source}  |  {state}')
        text = (f"ADC {row['adc']}   ·   {row['mV']} mV nominales   ·   RMS {row['rms']}   ·   ΔRMS {row['delta_rms']}   ·   Umbral {row['umbral']}"
                + (f"   ·   Fuerza máx {row['fuerza_max'] / 1000:.1f} N" if 'f1_mN' in row else ''))
        if 'bpm' in row:
            pulse = (f"Pulso {row['bpm']} BPM · SpO2 {row['spo2'] or '--'} % · PI {row['pi_x100'] / 100:.2f} %"
                     if row['pulso_estado'] == 4 else f"Pulso: {PULSE_TEXT.get(row['pulso_estado'], '?')}")
            temp = (f"T objeto {row['t_obj_cC'] / 100:.1f} °C · T ambiente {row['t_amb_cC'] / 100:.1f} °C"
                    if row['temp_estado'] == 1 else f"Temperatura: {TEMP_TEXT.get(row['temp_estado'], '?')}")
            text += f"\n{pulse}   ·   {temp}   ·   Errores I2C {row['errores_i2c']}"
        self.values.set(text)
        errors = sum(row[k] for k in ('perdidas', 'errores_adc', 'reportes_perdidos', 'errores_uart'))
        self.status.set(f"{'ALERTA' if errors or row['riel'] else 'Recibiendo'} · Riel: {row['riel']} · Pérdidas: {row['perdidas']} · Errores ADC: {row['errores_adc']} · Reportes perdidos: {row['reportes_perdidos']} · Errores UART: {row['errores_uart']} · Líneas inválidas PC: {self.bad_lines}" + (' · GUARDANDO CSV' if self.capture else ''))

    def tick(self):
        if self.review_rows is not None:
            if self.dirty:
                self.draw()
                self.dirty = False
            self.root.after(50, self.tick)
            return
        if self.demo:
            now = time.monotonic()
            for _ in range(10):
                if now < self.next_demo:
                    break
                self.accept(demo_row(self.demo_n))
                self.demo_n += 1
                self.next_demo += 0.05
        elif self.port is not None:
            try:
                self.buffer.extend(self.port.read(min(self.port.in_waiting, 32768)))
                while b'\n' in self.buffer:
                    raw, _, rest = self.buffer.partition(b'\n')
                    self.buffer = bytearray(rest)
                    try:
                        row = parse_line(raw.decode('ascii'))
                        if row:
                            self.accept(row)
                    except (UnicodeError, ValueError):
                        self.bad_lines += 1
                if len(self.buffer) > 8192:
                    self.buffer.clear()
                    self.bad_lines += 1
                if self.last_received and time.monotonic() - self.last_received > 2:
                    self.source.set('SIN DATOS RECIENTES · gráfica detenida')
            except OSError as exc:
                self.disconnect()
                self.status.set(f'Conexión interrumpida: {exc}')
        self.draw()
        self.root.after(50, self.tick)

    def visible(self):
        """(filas, función de tiempo, inicio, duración) de lo que se dibuja."""
        span = self.span()
        if self.review_rows is not None:
            start = self.review_t[0] + self.offset.get()
            lo = bisect.bisect_left(self.review_t, start)
            hi = bisect.bisect_right(self.review_t, start + span)
            self.range_text.set(f'{fmt_time(start)} – {fmt_time(start + span)} de {fmt_time(self.review_t[-1])}')
            return self.review_rows[lo:hi], (lambda r: r['t']), start, span
        xof = lambda r: r['tiempo_ms'] / 1000
        end = xof(self.points[-1]) if self.points else span
        start = max(0, end - span)
        return [r for r in self.points if xof(r) >= start], xof, start, span

    def draw(self):
        rows, xof, start, span = self.visible()
        if self.review_rows is not None:
            if rows:
                self.values.set(f"Vista: {len(rows)} filas · riel en {sum(r['riel'] > 0 for r in rows)} · activo en {sum(r['activo'] for r in rows)}"
                                f" · fuerza máx {max(r['fuerza_max'] for r in rows) / 1000:.1f} N · rms_ac máx {max(r['rms_ac'] for r in rows)}")
            else:
                self.values.set('Vista sin filas (hueco en la captura).')
        for canvas, title, series, floor in self.charts:
            canvas.delete('all')
            w, h = canvas.winfo_width(), canvas.winfo_height()
            left, right, top, bottom = 55, w - 20, 30, h - 22
            canvas.create_text(14, 13, text=title, anchor='w', fill=TEXT, font=('Segoe UI', 10, 'bold'))
            present = [s for s in series if rows and s[0] in rows[0]]
            ymax = max(floor, max((r[key] * 1.15 for r in rows for key, _, _ in present), default=floor))
            for i in range(3):
                y = top + (bottom - top) * i / 2
                canvas.create_line(left, y, right, y, fill='#2c3c52')
                canvas.create_text(left - 8, y, text=str(round(ymax * (1 - i / 2))), fill=MUTED, anchor='e')
            for i in range(4):
                x = left + (right - left) * i / 3
                canvas.create_text(x, bottom + 11, text=fmt_time(start + i * span / 3), fill=MUTED)
            if right <= left or bottom <= top:
                continue
            for j, (key, color, label) in enumerate(present):
                canvas.create_text(right - (len(present) - 1 - j) * 70, 13, text=label, fill=color, anchor='e')
                coords = []
                for x, v in envelope(rows, key, xof, start, span, int(right - left)):
                    coords.extend([left + (x - start) / span * (right - left), bottom - v / ymax * (bottom - top)])
                if len(coords) >= 4:
                    canvas.create_line(*coords, fill=color, width=2 if len(rows) <= 600 else 1)

    def toggle_record(self):
        if self.capture:
            self.stop_record()
            return
        filename = filedialog.asksaveasfilename(title='Guardar reportes recibidos', initialdir=str(Path(__file__).parent), initialfile='myogrip_reportes.csv', defaultextension='.csv', filetypes=[('CSV', '*.csv')])
        if not filename:
            return
        try:
            self.capture = open(filename, 'w', newline='', encoding='utf-8')
            # fuerza_max y t solo son para la vista; el CSV guarda las columnas del firmware.
            self.writer = csv.DictWriter(self.capture, fieldnames=['origen_pc'] + FIELDS + FSR_FIELDS + VIT_FIELDS, extrasaction='ignore')
            self.writer.writeheader()
            # La columna adicional identifica explícitamente las capturas sintéticas de PC.
            base_writer = self.writer
            is_demo = self.demo
            class TaggedWriter:
                def writerow(_, row):
                    base_writer.writerow(dict(row, origen_pc='DEMO_PC' if is_demo else 'NUCLEO'))
            self.writer = TaggedWriter()
            self.record_button['text'] = 'Detener CSV'
        except OSError as exc:
            self.stop_record()
            messagebox.showerror('No se pudo guardar', str(exc))

    def stop_record(self):
        if self.capture:
            self.capture.close()
        self.capture = self.writer = None
        if hasattr(self, 'record_button'):
            self.record_button['text'] = 'Guardar CSV…'

    def close(self):
        self.disconnect()
        self.root.destroy()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--demo', action='store_true')
    parser.add_argument('--abrir', metavar='CSV', help='Abre una captura guardada para revisarla')
    parser.add_argument('--smoke-test', action='store_true', help='Abre demo (o la captura de --abrir), comprueba y cierra')
    args = parser.parse_args()
    root = tk.Tk()
    app = Monitor(root, demo=(args.demo or args.smoke_test) and not args.abrir)
    if args.abrir:
        app.open_capture(args.abrir)
    if args.smoke_test:
        def finish():
            assert len(app.charts) == 5, 'Faltan gráficas'
            if args.abrir:
                assert app.review_rows, 'No se cargó la captura'
                for name in SPANS:          # dibuja con cada ventana de revisión
                    app.span_name.set(name)
                    app.update_scale()
                    app.draw()
                minutes = [app.table.item(i, 'values') for i in app.table.get_children()]
                print(f'OK: revisión de {len(app.review_rows)} filas, cinco gráficas, {len(minutes)} minutos en la tabla')
                for m in minutes:
                    print('   ', *m)
            else:
                assert len(app.points) >= 10, 'No se recibieron datos demo'
                print('OK: ventana, demo, recepción y dibujo de cinco gráficas (con riel y fuerza por FSR)')
            app.close()
        root.after(1500, finish)
    root.mainloop()
