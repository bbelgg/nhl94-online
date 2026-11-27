# As boot_h2h.inc, with Optperlen forced to 30-second periods (offline tests only).
WAIT 600
PRESS START 5
WAIT 120
PRESS START 5
WAIT 120
PRESS DOWN 5
WAIT 20
PRESS RIGHT 5
WAIT 20
PRESS RIGHT 5
WAIT 20
PRESS RIGHT 5
WAIT 20
PRESS START 5
WAIT 240
PRESS START 5
WAIT 240
# 30-second periods (Optperlen = 3) so period 2 (home attacks DOWN) comes quickly
WRITE_RAM16 0xFFD050 0x0003
PRESS START 5
