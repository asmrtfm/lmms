# LMMS Instrument: xpressive

Plugin name (internal): `xpressive`

## Default Parameters

| Parameter | Default |
|---|---|
| `A1` | `1` |
| `A2` | `1` |
| `A3` | `1` |
| `O1` | `sinew(integrate(f*(1+0.05sinew(12t))))*(2^(-(1.1+A2)*t)*(0.4+0.1(1+A3)+0.4sinew((2.5+2A1)t))^2)` |
| `O2` | `expw(integrate(f*atan(500t)*2/pi))*0.5+0.12` |
| `PAN1` | `1` |
| `PAN2` | `-1` |
| `RELTRANS` | `50` |
| `W1` | `""` |
| `W1sample` | `""` |
| `W2` | `""` |
| `W2sample` | `""` |
| `W3` | `""` |
| `W3sample` | `""` |
| `interpolateW1` | `0` |
| `interpolateW2` | `0` |
| `interpolateW3` | `0` |
| `smoothW1` | `0` |
| `smoothW2` | `0` |
| `smoothW3` | `0` |
| `version` | `0.1` |
