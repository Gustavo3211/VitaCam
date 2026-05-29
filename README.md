# VitaCam

VitaCam turns your PlayStation Vita into a wireless webcam for your PC (OBS Studio, Discord, web browsers).

## How to use as a Webcam in OBS
1. Connect your PS Vita and PC to the same Wi-Fi network.
2. Open VitaCam on your Vita.
3. In OBS Studio, add a new **Browser** source.
4. Set the URL to `http://vitacam.local:8080/` (or use the IP shown on your Vita screen, e.g., `http://192.168.x.x:8080/`).
5. Start the virtual camera on OBS Studio :D

## How it works

### 1. Camera & Encoding
- **Format**: The camera captures frames in YUV422 planar format directly into CDRAM.
- **Hardware Acceleration**: The Vita's JPEG hardware encoder (`SceJpegEnc`) compresses YUV frames into JPEG bytes in real-time, keeping CPU usage minimal.
- **Settings**: Resolution, frame rate, and lens selection (Front/Back) are applied dynamically by stopping, reconfiguring, and restarting the camera device.

### 2. Network & Server
- **Server**: A non-blocking TCP socket server handles requests. It serves a control page on `/`, status checks on `/status`, and the MJPEG stream on `/stream`.
- **mDNS**: A background thread listens on UDP port 5353, responding to DNS queries for `vitacam.local` with the Vita's IP address.
- **Privacy Mode**: When active, the `/status` endpoint reports that streaming is off, causing the web UI to overlay a black screen and hide the feed.

## Physical Controls

- **D-PAD UP / DOWN**: Cycle resolutions (640x480, 320x240, 160x120)
- **D-PAD LEFT / RIGHT**: Cycle framerates (30 FPS, 60 FPS, 15 FPS)
- **L / R Triggers**: Cycle camera mode presets (Default, Nightmode, Brightness Boost, Extreme Night; press both triggers simultaneously to reset to Default)
- **TRIANGLE**: Toggle camera lens (Front / Rear)
- **CIRCLE**: Toggle Vita screen on/off (black screen mode to conserve battery and prevent burn-in)
- **SQUARE**: Toggle privacy mode (streams black frames at source and displays indicator overlay in the browser)

## Building and Installing

### Compile
```bash
mkdir build
cd build
cmake ..
make -j4
```

### Installation
1. Install the generated `VitaCam.vpk` using VitaShell.
2. Connect the PS Vita to your Wi-Fi network.
3. Launch the application and open `http://vitacam.local:8080` in your web browser.
