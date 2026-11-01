#!/usr/bin/env python3
"""Registrador para la prueba de alcance ('p' en la placa medidora).

Le pone hora a cada linea que informa la placa y la guarda en un archivo, para
poder reconstruir la sesion despues. De PuTTY no se puede.

Montaje:
    - La placa que CAMINA emite: se le aprieta 'e' antes de salir.
    - La placa de la NOTEBOOK mide: se le aprieta 'p'. Es esta.
      Todas las cifras (RSSI, LQI, perdida) existen solo del lado del que
      recibe, asi que el que recibe tiene que estar donde esta la pantalla.

Uso:
    python tools/medir_alcance.py COM5
    python tools/medir_alcance.py COM5 --salida patio.txt

Mientras corre, se puede escribir en la consola:
    30 <Enter>      anota "MARCA 30 m" en el registro
    cualquier texto anota ese texto
    q <Enter>       termina

Asi queda cada ventana asociada a una distancia. Al final imprime un resumen
por tramo entre marcas.

OJO: hay que cerrar PuTTY antes, que los puertos COM no se comparten. Y el
puerto se mueve al reconectar: identificar la placa por el banner 'NODO n',
nunca por el numero de COM.
"""

import argparse
import re
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("Falta pyserial. Usar el Python de PlatformIO:\n"
             "  ~/.platformio/penv/Scripts/python.exe tools/medir_alcance.py COM5")

PATRON = re.compile(
    r"rx=(\d+)/(\d+) perdida=([\d.]+)%"
    r"(?:.*RSSI prom=(-?\d+).*LQI prom=(\d+))?"
)

marca_actual = "(sin marca)"
seguir = True


def leer_consola():
    """Hilo aparte: lo que se escriba queda como marca de distancia."""
    global marca_actual, seguir
    for linea in sys.stdin:
        texto = linea.strip()
        if texto.lower() in ("q", "quit", "salir"):
            seguir = False
            return
        if texto:
            marca_actual = f"{texto} m" if texto.replace(".", "").isdigit() else texto
            print(f"    >>> MARCA: {marca_actual}")


def main():
    global seguir

    ap = argparse.ArgumentParser()
    ap.add_argument("puerto", help="COM de la placa MEDIDORA (la de la notebook)")
    ap.add_argument("--salida", default="alcance.txt", help="archivo de registro")
    ap.add_argument("--baudios", type=int, default=921600)
    args = ap.parse_args()

    # Por tramo: [recibidos, esperados, suma de RSSI, cuantos RSSI, peor LQI]
    tramos = {}

    hilo = threading.Thread(target=leer_consola, daemon=True)
    hilo.start()

    print(f"Registrando {args.puerto} en {args.salida}")
    print("Escribir la distancia y Enter para marcar. 'q' para terminar.\n")

    # Abrir el puerto resetea la placa, asi que hay que volver a apretar 'p'.
    # Se manda desde aca para no depender de que alguien se acuerde.
    with serial.Serial(args.puerto, args.baudios, timeout=0.2) as s, \
            open(args.salida, "a", encoding="utf-8") as f:
        f.write(f"\n===== sesion {time.strftime('%Y-%m-%d %H:%M:%S')} "
                f"en {args.puerto} =====\n")
        time.sleep(2.5)                       # que termine de bootear
        s.reset_input_buffer()
        s.write(b"p")                         # activar el medidor
        s.flush()

        resto = ""
        while seguir:
            datos = s.read(4096).decode("utf-8", "replace")
            if not datos:
                continue
            resto += datos
            while "\n" in resto:
                linea, resto = resto.split("\n", 1)
                linea = linea.rstrip()
                if not linea:
                    continue

                sello = time.strftime("%H:%M:%S")
                salida = f"[{sello}] [{marca_actual}] {linea}"
                print(salida)
                f.write(salida + "\n")
                f.flush()

                m = PATRON.search(linea)
                if m:
                    t = tramos.setdefault(marca_actual, [0, 0, 0, 0, 0])
                    t[0] += int(m.group(1))
                    t[1] += int(m.group(2))
                    if m.group(4) is not None:
                        t[2] += int(m.group(4))
                        t[3] += 1
                        t[4] = max(t[4], int(m.group(5)))

        s.write(b"p")                         # dejar el medidor como estaba
        s.flush()

    if not tramos:
        print("\nNo llego ninguna ventana. Verificar que la otra placa tenga 'e' activado.")
        return

    print(f"\n{'tramo':>14} | {'recibidos':>11} | {'perdida':>8} | "
          f"{'RSSI prom':>10} | {'peor LQI':>8}")
    print("-" * 66)
    for nombre, (rx, esp, rssi_suma, n_rssi, lqi_peor) in tramos.items():
        perdida = 100.0 * (esp - rx) / esp if esp else 0.0
        rssi = f"{rssi_suma // n_rssi:>9}" if n_rssi else "        --"
        print(f"{nombre:>14} | {rx:>5}/{esp:<5} | {perdida:>7.1f}% | "
              f"{rssi} | {lqi_peor:>8}")
    print(f"\nRegistro completo en {args.salida}")
    print("Referencia en espacio libre (10 dBm, 0 dBi): "
          "30 m -> -45 dBm | 100 m -> -55 dBm")
    print("La diferencia contra el RSSI medido es lo que se pierde en antena "
          "y frente de RF.")


if __name__ == "__main__":
    main()
