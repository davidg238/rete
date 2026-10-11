"""Run layoutdump/capture.py with a serial port that resets an ESP32 devkit (EN via RTS) once open."""
import runpy, sys, time, serial
class ResetOnOpen(serial.Serial):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.dtr = False; self.rts = True; time.sleep(0.15)   # EN low, IO0 high: run mode
        self.rts = False; time.sleep(0.4)                     # past the strapping sample before DTR moves
_orig = serial.Serial
serial.Serial = ResetOnOpen
sys.argv = sys.argv[1:]
runpy.run_path(sys.argv[0], run_name="__main__")
