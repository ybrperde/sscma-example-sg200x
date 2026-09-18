# Integración OV5647 5MP @ 15 fps en el nodo Camera de Node-RED

Este documento describe cómo llevar el modo **2592×1944 @ 15 fps** (ya validado con `video_demo`) al desplegable **Resolution** del nodo `camera` de Node-RED en reCamera, **incluyendo uso con un nodo `model` (IA)**.

No basta con añadir una opción en el HTML. Node-RED no habla con `video_demo`: habla con el servicio **`sscma-node`** por MQTT, y ese servicio usa el mismo stack de vídeo que `video_demo`. Hay que tocar tres capas (UI, servicio, HAL/sensor) y respetar los límites de ION y de canal VPSS/VENC que ya vimos en 5MP.

**Objetivo de producto:** RTSP/grabación a 5MP **y** inferencia en el mismo flow. El modelo **no** infiere a 2592×1944 (el NPU pide 320/640 típicos). El diseño es sensor 5MP + H.264 5MP + RAW **escalado** al input del modelo.

---

## 1. Qué está comprobado y qué no

### Ya funciona (`video_demo`)

- Sensor OV5647 en modo 5MP (`OV5647 5MP 15fps … Init OK!`).
- Pipeline **VI/ISP/VPSS/VENC 2592×1944**, **15 fps**, `COMPRESS_MODE_NONE`.
- Modo **`VI_ONLINE_VPSS_ONLINE`**, entrada VPSS desde ISP, **sin bind VI→VPSS**.
- Pool VB: **2 bloques** NV21 de 2592×1944 (~15 MB). Más bloques fallan (`vb_ioctl_init` / `SYS_ION_ALLOC`).
- H.264 en **`VIDEO_CH0`** (no CH2), GOP 15, bitrate ~4000, stream buf 1 MB.
- RTSP `rtsp://<ip>:8554/live0` reproducible en VLC.

### No está comprobado (bloquea la IA)

- Segunda salida VPSS **escalada** (p. ej. 640×640 RGB) **a la vez** que H.264 5MP en CH0.
- Eso es el experimento de `video_demo` **antes** de tocar Node-RED (paso 3).

### Pendiente de copiar al dispositivo

- El desplegable 5MP está en `nodes/camera.html` / `nodes/camera.js` (**paso 5**); hay que sustituir los del paquete en la cámara.
- Overlay letterbox en `sscma-node` (**paso 6**): hace falta el `.deb` nuevo.
- El paquete UI de producción vive en [node-red-contrib-nodes](https://github.com/Seeed-Studio/node-red-contrib-nodes).

---

## 2. Arquitectura: de qué repo es cada pieza

```
┌─────────────────────────────────────────────────────────────────┐
│ Node-RED (navegador)                                            │
│  nodo camera → desplegable Resolution / FPS                     │
│  paquete: node-red-contrib-sscma                                │
│  repo: https://github.com/Seeed-Studio/node-red-contrib-nodes   │
│  en dispositivo:                                                │
│    /home/recamera/.node-red/node_modules/node-red-contrib-sscma │
└──────────────────────────────┬──────────────────────────────────┘
                               │ MQTT
                               │ sscma/v0/<client>/node/in/<id>
                               │ { type:3, name:"create",
                               │   data:{ type:"camera",
                               │          config:{ option:0, fps:15 } } }
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│ sscma-node  (este repo: solutions/sscma-node)                   │
│  CameraNode::onCreate() interpreta option / fps                 │
│  CHN_RAW=0  CHN_JPEG=1  CHN_H264=2   (lógicos Node-RED)         │
│  En 5MP el mapeo físico NO es 1:1 (ver §4)                      │
│  setupVideo(hw, param) → components/sophgo/video                │
└──────────────────────────────┬──────────────────────────────────┘
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│ HAL vídeo (este repo: components/sophgo/video)                  │
│  video_paramparse.c  VI/ISP/VPSS/VB/VENC                        │
│  libsns_ov5647.so    sensor/cv181x/ov_ov5647                    │
└─────────────────────────────────────────────────────────────────┘
```

Firmware completo: [Seeed-Studio/reCamera-OS](https://github.com/Seeed-Studio/reCamera-OS) empaqueta `sscma-node`, las librerías MPI (`libsns_ov5647.so`) y Node-RED. Para iterar no hace falta reconstruir todo el OS: se puede instalar un `.deb` de `sscma-node`, copiar el `.so` y parchear el nodo en el dispositivo.

Protocolo: `docs/sscma-node-protocol.md`. El campo `option` del nodo camera es un **enumerado**. Hoy Node-RED lo envía como **entero** (`parseInt(n.option)`).

---

## 3. Por qué no se puede “solo añadir 5MP al dropdown”

### 3.1 Mapeo actual de `option`

En `solutions/sscma-node/main/node/camera.cpp`, `CameraNode::onCreate()`:

| `option` | Resolución H.264 / JPEG | FPS por defecto del case |
|----------|-------------------------|--------------------------|
| `0` (default / `"1080p"`) | 1920×1080 | 30, luego se sobrescribe con `fps` del config |
| `1` (`"720p"`) | 1280×720 | idem |
| `2` (`"360p"` en string; 640×480 en el `switch`) | 640×480 | idem |
| `3` (`"5mp"` / `"2592"`) | H.264 2592×1944; JPEG 640×640 | **15**, y un `fps` > 15 no pisa |

El HTML del nodo (`camera.html` del paquete contrib) usa `0/1/2` = 1080p / 720p / 480p. El parseo por string busca `"360p"` pero el `switch` de `2` es 640×480: conviene unificar al añadir 5MP.

`fps` llega aparte (`5/10/15/30`) y se aplica a RAW, JPEG y H.264 **después** del `switch`. Si se elige 5MP hay que **forzar 15** y no dejar que un FPS 30 del desplegable pise el modo del sensor.

### 3.2 H.264 de Node-RED va al canal 2; 5MP solo funcionó en canal 0

Constantes en `camera.h` (hoy lógico = físico):

```text
CHN_RAW  = 0   → VPSS/VENC físico 0  (RGB para el nodo model)
CHN_JPEG = 1   → físico 1            (preview / save foto)
CHN_H264 = 2   → físico 2            (stream RTSP / websocket / save vídeo)
```

`stream.cpp` y `save.cpp` hacen `attach(CHN_H264)`. El nodo `model` hace `attach(CHN_RAW)` pidiendo **el tamaño de entrada del modelo**, no el del sensor (`model.cpp`, `onStart`).

`setupVideo(i, …)` usa el índice lógico como canal hardware.

En `video_demo`, **CH2 a 2592 no entrega frames** (en CV181x CH2 queda ~1080p). El 5MP estable fue **`setupVideo(VIDEO_CH0, …)`**.

Si se pone 2592×1944 en `channels_[CHN_H264]` y se llama `setupVideo(2, …)`, el Create puede ir bien y el RTSP quedarse vacío. Hay que **remapear el H.264 de 5MP al canal físico 0**.

Eso choca con el RAW de IA si se deja también en CH0. El callback de VENC indexa `channels_[VencChn]` con el **canal hardware**. Sin tabla lógico↔físico, los frames 5MP en CH0 se publicarían como RAW y el nodo `stream` (CH2) no vería nada.

### 3.3 ION: no caben tres copias a 5MP; sí caben 5MP + un downscale

ION es el pool de memoria de vídeo. Un frame 5MP NV21 son ~7,6 MB.

- 8 bloques a 5MP (~61 MB) → falla.
- 2 bloques 5MP (~15 MB) + online → funciona (`video_demo`, un solo canal H.264).

Un flow Node-RED típico enciende H.264 + JPEG + RAW. **Tres salidas a 2592×1944 no caben.**

Eso **no** impide la IA. El modelo no necesita RAW 5MP. Necesita RGB/NV21 al tamaño del tensor (p. ej. 640×640, ~1 MB). El presupuesto a validar es:

- 2 bloques 5MP para encode en CH0
- bloques **pequeños** para RAW (y JPEG de preview si se usa)

JPEG de preview **a 5MP** a la vez que H.264 5MP sigue fuera (ION). El preview debe ir a resolución de modelo / 640 / 1080p, no a 2592.

Nota: el fallo de escala que vimos (encode 2592×1936 vs VI 2592×1944) fue **el mismo canal** desalineado 8 px. No prueba que una **segunda** salida VPSS no pueda escalar 2592→640. Eso es exactamente el test del paso 3.

### 3.4 El HAL de este branch está “clavado” a 5MP

`video_paramparse.c` (compartido por `video_demo` **y** `sscma-node`) ya carga VI/VPSS a 2592×1944. Eso está bien para el demo, pero **rompe el default 1080p** de Node-RED si se publica tal cual.

Hay que hacer el tamaño de VI **dependiente de `option`**:

- `option` 0/1/2 → sensor 1080p (comportamiento de producción).
- `option` 3 (5MP) → sensor 2592×1944, VB acorde, online, H.264 en CH0, RAW escalado en CH1/CH2.

`initVideo()` / `app_ipcam_Param_Load()` se llama **antes** de parsear `option`. Hace falta o bien recargar parámetros tras el parseo, o una API tipo `setVideoSensorMode(w, h, fps)` **antes** de `startVideo()`.

### 3.5 Otros gotchas ya vistos en 5MP

- **`cmos_set_wdr_mode(WDR_MODE_NONE)`** no debe devolver el sensor a 1080p.
- **`cmos_set_image_mode`**: ignorar un switch a 1080p **a mitad de stream** (PQ/térmico). En un deploy nuevo de Node-RED el pipeline se recrea: arrancar ya en 1080p o ya en 5MP, no mezclar en caliente.
- Callback térmico `CVI_SYS_RegisterThermalCallback` + PQ bin 1080p (`ov_ov5647_sdr.bin`) puede bajar VI a 1920×1080 a los ~45 s. Mantenerlo desactivado en 5MP (como ahora en `isp.c`) o recortar el `SetPubAttr` para no cambiar tamaño.
- Tabla 5MP del sensor: secuencia MIPI/PLL CVI 1080p (`0x3036=0x64`, pads off → stream on → MIPI on) con crop/HTS/VTS 2592×1944 / 2844 / 1968. No clonar a ciegas la tabla Raspberry Pi.
- Instalar **`libsns_ov5647.so` nuevo** en `/mnt/system/usr/lib/` (y `/mnt/system/lib/` si existe). Si el `.so` es el de fábrica, el log `Init OK!` no lleva el modo 5MP.

---

## 4. Diseño objetivo (5MP + IA)

Nuevo valor de enumerado, **sin romper 0/1/2**. En 5MP, **lógico ≠ físico**:

| `option` | UI | Sensor / VI | H.264 (lógico) | H.264 físico | RAW / model (lógico) | RAW físico | JPEG preview |
|----------|----|-------------|----------------|--------------|----------------------|------------|--------------|
| 0 | 1920×1080 (1080p) | 1920×1080 | 1920×1080 | CH2 | tamaño modelo | CH0 | sí, ~1080p o preview del model |
| 1 | 1280×720 (720p) | 1920×1080 (scale) | 1280×720 | CH2 | tamaño modelo | CH0 | sí |
| 2 | 640×480 (480p) | 1920×1080 (scale) | 640×480 | CH2 | tamaño modelo | CH0 | sí |
| **3** | **2592×1944 (5MP) @ 15 fps** | **2592×1944 @ 15** | **2592×1944 @ 15** | **CH0** | **tamaño modelo (p. ej. 640×640)** | **CH1 o CH2** | **no 5MP**; mismo tamaño que preview/modelo |

`stream` / `save` siguen con `attach(CHN_H264)`. `model` sigue con `attach(CHN_RAW)` y `config(CHN_RAW, img->width, img->height, …)`. Solo `CameraNode` traduce a canal hardware.

En la UI, al elegir 5MP:

- Fijar FPS a **15** y deshabilitar 30.
- El flow camera → model → stream es **válido**.
- No ofrecer preview JPEG a 2592×1944.

Las cajas del modelo salen en coordenadas del tensor. Si el overlay va sobre el RTSP 5MP, mapear 640 (o el input) → 2592×1944 (letterbox/crop igual que el VPSS). A 1080p ya existe un mapeo similar modelo→preview.

Contrato MQTT (ejemplo):

```json
{
  "type": 3,
  "name": "create",
  "data": {
    "type": "camera",
    "config": {
      "option": 3,
      "fps": 15,
      "audio": true
    }
  }
}
```

También aceptar string `"5mp"` / `"2592"` por si algún cliente no manda entero.

---

## 5. Pasos de implementación

Orden: sensor → HAL seleccionable → **prueba dual-canal en `video_demo`** → `sscma-node` → UI Node-RED → OS. No integrar Node-RED hasta que el paso 3 pase en hardware.

### Paso 1 — Sensor `libsns_ov5647.so` (ya hecho en este branch)

Fuentes: `sensor/cv181x/ov_ov5647/`.

Comprobar que el firmware de destino carga **este** `.so`, no el de `cvi_mpi` de reCamera-OS.

Rebuild:

```bash
docker run --rm --platform linux/amd64 --entrypoint sh \
  -v "$PWD:/workspace" sscma-sg200x -c '
SDK=/workspace/.sdk/sg2002_recamera_emmc
SRC=/workspace/sensor/cv181x/ov_ov5647
CC=riscv64-unknown-linux-musl-gcc
$CC -fPIC -O2 -D__CV181X__ -mcpu=c906fdv \
  -march=rv64gcv0p7_zfh_xthead -mabi=lp64d \
  -I$SDK/cvi_mpi/include -I$SDK/cvi_mpi/include/isp/cv181x \
  -I$SDK/cvi_mpi/include/linux \
  -I$SDK/osdrv/interdrv/include/common/uapi \
  -I$SDK/osdrv/interdrv/include/chip/cv181x/uapi -I$SRC \
  -shared -o $SRC/libsns_ov5647.so \
  $SRC/ov5647_cmos.c $SRC/ov5647_sensor_ctl.c'
```

En el dispositivo:

```bash
cp libsns_ov5647.so /mnt/system/usr/lib/
# si existe:
cp libsns_ov5647.so /mnt/system/lib/
```

En un build de **reCamera-OS**, sustituir el `.so` en el staging de `cvi_mpi` / overlay para que sobreviva a un flash.

### Paso 2 — HAL: modo VI seleccionable, no solo 5MP

**Estado:** implementado en este branch (`setVideoSensorOutput` / `app_ipcam_Param_SetSensorOutput`). `Param_Load()` queda en 1080p@30; `video_demo` pide 5MP después del load. En `VI_ONLINE_VPSS_ONLINE`, el pool VB de cada canal es el tamaño de **esa** salida (el RGB 640 ya no se infla a 5MP).

Archivos:

- `components/sophgo/video/src/video_paramparse.c`
- `components/sophgo/video/video.c` / `video.h`
- `components/sophgo/video/src/sensors.c`
- `components/sophgo/video/src/isp.c` (callback térmico)

Trabajo:

1. Extraer de las constantes fijas (2592×1944) una función, por ejemplo:

   `app_ipcam_Param_SetSensorOutput(u32 w, u32 h, float fps)`

   que actualice:

   - `vi->astChnInfo[0].u32Width/Height/f32Fps`
   - `vpss` grp `u32MaxW/H` (2592×1944 en modo 5MP, para que CH0 quepa)
   - `sns_cfg_ov5647.s32Framerate`
   - pub attr ISP / FPS OV5647 (hoy 15 en 5MP; 25 por defecto genérico rompe AE)

2. Defaults de `app_ipcam_Param_Load()`: **1080p** (producción Node-RED). `video_demo` llama a `SetSensorOutput(2592, 1944, 15)` **después** del load y **antes** de `startVideo()`.

3. VB: en 5MP, 2 bloques NV21 del tamaño del sensor para el encode; pools extra solo del tamaño del RAW/JPEG pequeño. No habilitar pools 5MP en canales no usados (`bEnable = 0`). En 1080p, el esquema actual de 3 canales.

4. 5MP: `VI_ONLINE_VPSS_ONLINE`, `VPSS_INPUT_ISP`, `COMPRESS_MODE_NONE`.

5. No reactivar el callback térmico en 5MP hasta tener PQ 5MP.

6. CH0 de encode debe coincidir con VI (2592×1944), no 1936: el desajuste de 8 px mató frames.

### Paso 3 — Prueba dual-canal en `video_demo` (antes de Node-RED)

**Estado:** implementado en `solutions/video_demo/main/main.cpp` (H.264 5MP en CH0 + RGB 640×640 en CH1, dump `/tmp/ch1_640x640.ppm`). Si CH1 no entrega frames, recompilar con `-DVIDEO_DEMO_RAW_CH=VIDEO_CH2`.

Este paso decide si el diseño del §4 es viable en este SoC. No tocar `sscma-node` hasta tenerlo verde.

En `solutions/video_demo/main/main.cpp`, además del H.264 5MP en **CH0** (RTSP `live0`):

1. `setupVideo` de un segundo canal (**CH1 primero**; si falla, CH2):

   - formato `VIDEO_FORMAT_RGB888` o `NV21` (el que use el nodo model)
   - tamaño **640×640** (o 640×480); 15 fps
   - handler que cuente frames y, opcional, vuelque un JPEG/PPM a `/tmp` cada N frames

2. VB: no reservar un tercer pool a 5MP. El pool del canal secundario debe ser 640×640.

3. Criterio de éxito (dejar corriendo ≥ 2 min):

   | Check | OK |
   |-------|----|
   | Sensor sigue en 5MP | log `OV5647 5MP`, VI no cae a 1920×1080 |
   | RTSP CH0 | VLC / `ffprobe` 2592×1944 ~15 fps |
   | Canal RGB | contador de frames subiendo; dump 640×640 válido |
   | ION | no `SYS_ION_ALLOC` / `vb_ioctl_init` fail |
   | Estabilidad | los dos flujos a la vez, sin que CH0 se pare |

4. Si CH1 no escala en online: repetir en CH2 (techo ~1080p, 640 cabe).

5. Si ni CH1 ni CH2 entregan el downscale: documentar el fallo (tamaño VPSS, wrap, online vs offline) **antes** de rediseñar (offline, segundo grupo VPSS). No seguir a Node-RED a ciegas.

6. Opcional: tercer canal JPEG 640 (preview). **Estado:** en `video_demo` (`VIDEO_DEMO_JPEG=1`, CH2). Si ION falla al `startVideo` (`vb_ioctl_init` / `SYS_ION_ALLOC`), recompilar con `-DVIDEO_DEMO_JPEG=0`. El RGB/IA en CH1 no depende del JPEG.

`sscma-node` y `video_demo` **no** pueden correr a la vez. Para este test:

```bash
/etc/init.d/S91sscma-node stop
/etc/init.d/S03node-red stop
```

### Paso 4 — `sscma-node`: `option == 3` y tabla lógico↔físico

**Estado: implementado** en `camera.cpp` / `camera.h` y documentado en el protocolo. Desplegable en `nodes/camera.html` (paso 5). Overlay letterbox en `model.cpp` (paso 6). Falta el test en dispositivo desde Node-RED (§6.1–6.3).

Archivos:

- `solutions/sscma-node/main/node/camera.cpp`
- `solutions/sscma-node/main/node/camera.h` (mapa `logical → hw`)
- `docs/sscma-node-protocol.md` (documentar `option: 3`)

En `onCreate()`:

```cpp
} else if (option.find("5mp") != std::string::npos ||
           option.find("2592") != std::string::npos) {
    option_ = 3;
}
```

Y si `option` es número, aceptar `3`.

Nuevo `case 3:` (tamaños por defecto; el model pisa RAW al arrancar):

```cpp
channels_[CHN_H264].format = MA_PIXEL_FORMAT_H264;
channels_[CHN_H264].width  = 2592;
channels_[CHN_H264].height = 1944;
channels_[CHN_H264].fps    = 15;
channels_[CHN_JPEG].format = MA_PIXEL_FORMAT_JPEG;
channels_[CHN_JPEG].width  = 640;   // no 5MP
channels_[CHN_JPEG].height = 640;
channels_[CHN_JPEG].fps    = 15;
channels_[CHN_RAW].fps     = 15;
fps_ = 15;
```

Después del `switch`, si `option_ == 3`, no aplicar un `fps_ > 15`.

**Antes** de `CAMERA_INIT()` / `startVideo()`:

```text
app_ipcam_Param_SetSensorOutput(2592, 1944, 15);
```

**Remap en `onStart()`** (tras el paso 3, usar el canal RGB que funcionó):

```text
hw(CHN_H264) = VIDEO_CH0
hw(CHN_RAW)  = VIDEO_CH1   // o CH2 si el test lo exigió
hw(CHN_JPEG) = el otro canal pequeño, o deshabilitado

setupVideo(hw, param)   // param de RAW = tamaño que pidió model
registerVideoFrameHandler(hw, ...)
```

**Callbacks:** hardware → lógico:

```text
logical = map_hw_to_logical(VencChn / VpssChn)
post to channels_[logical]
```

Así `stream.cpp`, `save.cpp` y `model.cpp` no cambian el `attach()`.

Si no hay nodo `model`, no habilitar el canal RAW (ahorra ION). Si lo hay, `config(CHN_RAW, w, h, …)` ya pone el tamaño del tensor **antes** de `startVideo` si el orden de start del grafo lo permite; si el camera arranca antes que el model, hay que reconfigurar VPSS o arrancar vídeo después de resolver dependencias (hoy el model configura RAW en su `onStart`: verificar que eso ocurre **antes** de `CAMERA_INIT` o retrasar `startVideo` hasta que RAW esté configurado).

Bitrate/GOP: alinear con `video_demo` (4000 kbps, GOP 15, stream buf 1 MB) en 5MP.

Rebuild e instalación típica:

```bash
docker run --rm --platform linux/amd64 -e PACKAGE=1 \
  -v "$PWD:/workspace" sscma-sg200x sscma-node
# solutions/sscma-node/build/sscma-node_*_riscv64.deb
```

En el dispositivo (versión de paquete suele no subir sola: `opkg remove` + `opkg install`):

```bash
/etc/init.d/S91sscma-node stop
/etc/init.d/S03node-red stop
opkg remove sscma-node
opkg install ./sscma-node_*.deb
# copiar libsns_ov5647.so si no va en el deb
/etc/init.d/S91sscma-node start
/etc/init.d/S03node-red start
```

### Paso 5 — Desplegable Node-RED (`node-red-contrib-sscma`)

**Estado: implementado** en las copias locales `nodes/camera.html` y `nodes/camera.js`. Hay que copiarlas al dispositivo y hacer hard refresh.

Repo: [Seeed-Studio/node-red-contrib-nodes](https://github.com/Seeed-Studio/node-red-contrib-nodes), paquete `node-red-contrib-sscma`.

En dispositivo:

```text
/home/recamera/.node-red/node_modules/node-red-contrib-sscma/nodes/camera.html
/home/recamera/.node-red/node_modules/node-red-contrib-sscma/nodes/camera.js
```

`camera.js` ya manda entero:

```javascript
node.config = {
  option: parseInt(n.option),
  fps: parseInt(n.fps),
  ...
};
```

No hace falta cambiar el protocolo si el HTML añade `option = 3`.

En `camera.html`:

1. Nuevo `<option value="3">2592x1944 (5MP) @ 15fps</option>`.
2. `validoption = ["0", "1", "2", "3"]`.
3. En `oneditprepare`: si `option === "3"`, forzar FPS 15 y deshabilitar 30.
4. Help: 5MP es el stream; la IA usa el input del modelo (escalado). Se puede conectar `model`.
5. Wiki Seeed (1080p@15 recomendado por estabilidad): actualizar cuando el modo esté validado en device.

Para un firmware de reCamera-OS, meter el paquete parcheado en el rootfs de Node-RED, no solo un `sed` en `/home/recamera`.

```bash
/etc/init.d/S03node-red restart
```

Hard refresh del editor (caché del navegador).

### Paso 6 — Overlay de cajas y supervisor

**Estado: implementado.** El nodo `preview` de Node-RED (websocket 8090) y el template del supervisor dibujan `data.boxes` sobre el JPEG de preview, en el espacio de `data.resolution`. No se queman cajas en el RTSP.

- `model.cpp` mapea el tensor (p. ej. 640×640, letterbox `ASPECT_RATIO_AUTO` desde el VI) al JPEG de preview. En 5MP el sensor es 4:3; un preview 16:9 (1280×720) queda con pillarbox y las cajas siguen al contenido. Sin debug, las cajas salen en espacio H.264 (2592×1944).
- JPEG de preview en 5MP no puede ser 2592×1944 (ION). Se recorta a ≤1280×960; 1920×1080 se baja a 640×640.
- `data.stream_resolution` es siempre el tamaño H.264 (`[2592,1944]` en modo 5MP).
- Supervisor: el SVG ya toma `viewBox` de `data.resolution`; el `<image>` ahora usa también ese tamaño. El `1920×1080` inicial es solo el fallback antes del primer frame.

### Paso 7 — reCamera-OS (imagen de fábrica)

Cuando 5MP + model esté estable en dispositivo:

1. Fork/overlay de [reCamera-OS](https://github.com/Seeed-Studio/reCamera-OS).
2. Sustituir `libsns_ov5647.so` en MPI.
3. Empaquetar `sscma-node` nuevo.
4. Empaquetar `node-red-contrib-sscma` con el dropdown 5MP.
5. PQ bin 5MP (`ov_ov5647_sdr.bin`) cuando exista; hasta entonces dejar el callback térmico fuera en 5MP.

Este repo (`sscma-example-sg200x`) es el que hay que mergear primero; el OS solo consume artefactos.

---

## 6. Plan de pruebas

Entorno: mismo que `video_demo` (p. ej. `192.168.0.16`).

### 6.0 Dual-canal (`video_demo`, paso 3)

Obligatorio antes de Node-RED. Ver tabla del paso 3.

### 6.1 Regresión 1080p (no romper producción)

1. Nodo camera: 1920×1080, 15 fps, `stream` + `model`.
2. Deploy.
3. Log `sscma-node`: H.264 canal físico 2, 1920×1080.
4. Sensor: init 1080p, no `OV5647 5MP`.
5. VLC / `ffprobe` → 1920×1080.
6. Detección igual que antes.

### 6.2 Modo 5MP + stream

1. Resolution **2592×1944 (5MP)**, FPS 15. Flow: camera → stream.
2. Sensor: `OV5647 5MP 15fps … Init OK!`.
3. H.264 en **canal físico 0**, 2592×1944 @ 15.
4. VLC / `ffprobe`: **2592×1944**, ~15 fps.
5. 2–3 min: VI **no** cae a 1920×1080.
6. Sin errores ION.

### 6.3 Modo 5MP + model (objetivo)

1. Flow: camera (5MP) → model → stream (y preview si el JPEG pequeño está activo).
2. Log model: `config channel RAW width 640 height 640` (o el input real), **canal físico 1 o 2**.
3. Inferencia con fps > 0 (no cola vacía de RAW).
4. RTSP sigue a 2592×1944 a la vez.
5. Overlay (nodo `preview` o dashboard supervisor): cajas alineadas con el JPEG. En 5MP usar preview **640×640** (o 1280×720; las cajas compensan el pillarbox). No elegir 1920×1080 (se recorta).
6. Redeploy 5MP → 1080p: el sensor vuelve a 1080p y el model sigue en CH0 físico.

### 6.4 Casos borde

- 5MP sin model: no reservar pool RAW.
- Preview JPEG 5MP: no habilitar (ION).
- `video_demo` tras cambios del HAL: 5MP CH0 y, si está el dual-canal, RGB 640.

---

## 7. Orden de PRs sugerido

1. **Este repo, HAL + `video_demo` dual-canal.** Gate: §6.0 verde.
2. **Este repo, `sscma-node` `option 3` + remap + protocolo.** Gate: §6.1–6.3.
3. **node-red-contrib-nodes:** dropdown + lock FPS 15.
4. **reCamera-OS:** `.so`, paquetes y (más adelante) PQ 5MP.

No abrir el PR del OS hasta que 6.1–6.3 pasen en hardware.

---

## 8. Fuera de alcance (de verdad)

- Inferencia NPU a 2592×1944 (el modelo no lo pide; ION/NPU no lo aguantan).
- Preview JPEG 5MP **y** H.264 5MP a la vez.
- 5MP @ 30 fps (el modo del sensor es ~15).
- Encode 5MP en CH2.
- Bin PQ 5MP y callback térmico que no pise el tamaño.
- Overlay quemado en el bitstream RTSP 5MP (las cajas van por websocket/JPEG de preview).

**Dentro de alcance:** 5MP + nodo `model` con RAW escalado.

---

## 9. Referencia rápida de archivos

| Capa | Ruta | Qué tocar |
|------|------|-----------|
| Sensor | `sensor/cv181x/ov_ov5647/*` | Ya 5MP; empaquetar `.so` |
| HAL VI/VPSS/VB | `components/sophgo/video/src/video_paramparse.c` | Modo dinámico 1080p vs 5MP; VB mixto |
| HAL ISP | `components/sophgo/video/src/isp.c` | Térmico off en 5MP |
| HAL sensor pub | `components/sophgo/video/src/sensors.c` | FPS 15 OV5647 5MP |
| API vídeo | `components/sophgo/video/video.c` | `SetSensorOutput` + VB por canal |
| Demo | `solutions/video_demo/main/main.cpp` | CH0 5MP H.264 + CH1/CH2 RGB 640 |
| Servicio | `solutions/sscma-node/main/node/camera.cpp` | `option=3`, mapa lógico↔físico |
| Model/stream/save | `model.cpp`, `stream.cpp`, `save.cpp` | `attach()` igual; overlay letterbox en `model.cpp` |
| Protocolo | `docs/sscma-node-protocol.md` | Documentar option 3 |
| UI | `node-red-contrib-sscma/nodes/camera.html` (otro repo) | Dropdown value 3 |
| OS | reCamera-OS overlays | `.so` + debs + nodo |

---

## 10. Criterio de “hecho”

Se considera integrado cuando, **desde Node-RED, sin `video_demo`**:

1. El desplegable muestra **2592×1944 (5MP) @ 15fps**.
2. Camera → stream entrega H.264 **2592×1944 ~15 fps**.
3. Camera → **model** → stream: hay detecciones **y** el RTSP sigue a 5MP.
4. 1080p en el desplegable se comporta como producción (model en CH0 físico, H.264 en CH2).
5. Sin OOM ION y sin caída a 1080p a los pocos segundos en 5MP.
