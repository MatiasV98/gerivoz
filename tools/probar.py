#!/usr/bin/env python
"""Chequeo previo de las dos placas antes de salir a probar alcance.

Uso:
    python tools/probar.py COM5 COM10            # chequeo completo
    python tools/probar.py COM5 COM10 pulsador   # solo probar el pulsador
    python tools/probar.py COM5 COM10 enlace     # solo probar el enlace RF

Necesita pyserial. Si no lo tenes en el Python del sistema, usar el de
PlatformIO, que ya lo trae:
    ~/.platformio/penv/Scripts/python.exe tools/probar.py COM5 COM10
"""
import sys, time

try:
    import serial
except ImportError:
    sys.exit("Falta pyserial. Probar con el python de PlatformIO:\n"
             "  ~/.platformio/penv/Scripts/python.exe tools/probar.py COM5 COM10")


def abrir(puerto, resetear=False):
    """Abre el puerto. Por defecto NO resetea la placa, para no perder el
    estado (por ejemplo la potencia que se bajo con 'w')."""
    s = serial.Serial()
    s.port = puerto
    s.baudrate = 921600
    s.timeout = 0.05
    s.dtr = False
    s.rts = False
    s.open()
    if resetear:
        s.setRTS(True); time.sleep(0.12); s.setRTS(False)
    s.reset_input_buffer()
    return s


def leer(puertos, seg, enviar=None, cada=None):
    """Lee de varios puertos a la vez durante 'seg' segundos."""
    buf = {p: b"" for p in puertos}
    t0 = ult = time.time()
    while time.time() - t0 < seg:
        for p, s in puertos.items():
            buf[p] += s.read(4096)
        if enviar and cada and time.time() - ult > cada:
            enviar[0].write(enviar[1]); enviar[0].flush()
            ult = time.time()
    return buf


def texto(b):
    return b.decode("utf-8", errors="replace")


def mostrar(titulo, filtro, buf):
    print(f"\n--- {titulo} ---")
    for linea in texto(buf).splitlines():
        if any(f in linea for f in filtro):
            print("   " + linea.strip())


def arranque(pa, pb):
    print("\n=== 1. Arranque ===")
    for nombre, puerto in (("NODO A", pa), ("NODO B", pb)):
        s = abrir(puerto, resetear=True)
        b = leer({puerto: s}, 5.0)[puerto]
        s.close()
        mostrar(f"{nombre} ({puerto})", ["NODO", "CC1101:", "Frecuencia",
                                         "Data rate", "Calibracion",
                                         "Audio inicializado"], b)


def pulsador(pa, pb):
    print("\n=== 2. Pulsador (PTT) ===")
    print("    APRETA Y SOLTA el pulsador de cada placa durante los proximos")
    print("    20 segundos. Tiene que aparecer 'TX audio' en la que apretes.")
    a, b = abrir(pa), abrir(pb)
    buf = leer({pa: a, pb: b}, 20.0)
    a.close(); b.close()
    for nombre, p in (("NODO A", pa), ("NODO B", pb)):
        visto = "TX audio" in texto(buf[p])
        print(f"   {nombre} ({p}): pulsador {'OK' if visto else 'NO DETECTADO'}")


def enlace(pa, pb):
    print("\n=== 3. Enlace RF ===")
    print("    OJO: con las placas juntas hay que bajar la potencia o el")
    print("    receptor se satura. Se baja a 0 dBm para esta prueba.")
    a, b = abrir(pa), abrir(pb)

    for _ in range(10):                      # llevar el emisor a 0 dBm
        a.write(b"w"); a.flush()
        if "Potencia TX = 0 dBm" in texto(leer({pa: a}, 0.8)[pa]):
            print("   emisor en 0 dBm")
            break

    for s in (a, b):
        s.write(b"c"); s.flush()
    leer({pa: a, pb: b}, 0.6)

    buf = leer({pa: a, pb: b}, 4.0, enviar=(a, b"t"), cada=0.6)
    b.write(b"s"); b.flush()
    buf2 = leer({pa: a, pb: b}, 2.5)
    a.close(); b.close()
    mostrar("recibido en B", ["PULSO de", "Recibidos validos",
                              "Syncs", "CRC malo", "Desbordes"],
            buf[pb] + buf2[pb])
    print("\n   Para el alcance real hay que REINICIAR las placas, que vuelven")
    print("   a 10 dBm. A distancia no se saturan.")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pa, pb = sys.argv[1], sys.argv[2]
    que = sys.argv[3] if len(sys.argv) > 3 else "todo"
    if que in ("todo", "arranque"):  arranque(pa, pb)
    if que in ("todo", "pulsador"):  pulsador(pa, pb)
    if que in ("todo", "enlace"):    enlace(pa, pb)
