// VitaCam - HTTP MJPEG IP Camera for PlayStation Vita
// Supports dynamic resolution/FPS/camera toggles and battery saving black screen mode.

#include <psp2/camera.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/jpegenc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugScreen.h"

#define SERVER_PORT 80
#define NET_INIT_SIZE (1 * 1024 * 1024)

#define CAM_W 640
#define CAM_H 480

static SceUID g_cam_mem_uid = -1;
static void *g_cam_buf = NULL;

static SceUID g_jpg_mem_uid = -1;
static void *g_jpg_buf = NULL;
static SceSize g_jpg_size = 256 * 1024;

static SceCameraInfo g_cam_info;
static SceCameraRead g_cam_read;

static SceJpegEncoderContext g_jpeg_ctx = NULL;

static char g_ip_str[16];
static int g_actual_port = 80;
static int g_screen_mode = 0; // 0: HUD, 1: Black Screen
static int g_webcam_off = 0;  // 0: ON, 1: OFF (Privacy Mode)

static int g_res_choices[] = {
  SCE_CAMERA_RESOLUTION_640_480,
  SCE_CAMERA_RESOLUTION_320_240,
  SCE_CAMERA_RESOLUTION_160_120
};
static int g_res_widths[] = { 640, 320, 160 };
static int g_res_heights[] = { 480, 240, 120 };
static int g_res_idx = 0; // default 640x480

static int g_fps_choices[] = {
  SCE_CAMERA_FRAMERATE_30_FPS,
  SCE_CAMERA_FRAMERATE_60_FPS,
  SCE_CAMERA_FRAMERATE_15_FPS
};
static const char *g_fps_names[] = { "30 FPS", "60 FPS", "15 FPS" };
static int g_fps_idx = 0; // default 30 FPS

static const char *g_mode_names[] = {
  "Padrao (Default)",
  "Nightmode",
  "Brightness Boost",
  "Extreme Night"
};
static int g_cam_mode = 0; // 0 = Padrao, 1 = Nightmode, 2 = Brightness Boost, 3 = Extreme Night

static int g_cam_device = SCE_CAMERA_DEVICE_FRONT; // default front
static uint32_t g_last_buttons = 0;

static void apply_camera_mode(void) {
  int brightness = 128;
  int nightmode = SCE_CAMERA_NIGHTMODE_OFF;
  int ev = 0;

  if (g_cam_mode == 1) { // Nightmode
    brightness = 128;
    nightmode = SCE_CAMERA_NIGHTMODE_LESS10;
    ev = 0;
  } else if (g_cam_mode == 2) { // Brightness Boost
    brightness = 180;
    nightmode = SCE_CAMERA_NIGHTMODE_OFF;
    ev = 10; // +1.0 EV
  } else if (g_cam_mode == 3) { // Extreme Night
    brightness = 180;
    nightmode = SCE_CAMERA_NIGHTMODE_LESS10;
    ev = 10; // +1.0 EV
  }

  sceCameraSetBrightness(g_cam_device, brightness);
  sceCameraSetNightmode(g_cam_device, nightmode);
  sceCameraSetEV(g_cam_device, ev);
}

static void update_hud_display(void) {
  if (g_screen_mode != 0)
    return;

  psvDebugScreenClear(0);
  psvDebugScreenPrintf("\n\n");
  psvDebugScreenPrintf("   VitaCam HTTP MJPEG Server\n");
  psvDebugScreenPrintf("   -------------------------\n\n");
  psvDebugScreenPrintf("   Open your browser and navigate to:\n");
  char url_mdns[64];
  char url_ip[64];
  if (g_actual_port == 80) {
    snprintf(url_mdns, sizeof(url_mdns), "http://vitacam.local/");
    snprintf(url_ip, sizeof(url_ip), "http://%s/", g_ip_str);
  } else {
    snprintf(url_mdns, sizeof(url_mdns), "http://vitacam.local:%d/", g_actual_port);
    snprintf(url_ip, sizeof(url_ip), "http://%s:%d/", g_ip_str, g_actual_port);
  }
  psvDebugScreenPrintf("   %s\n", url_mdns);
  psvDebugScreenPrintf("   %s\n\n", url_ip);

  psvDebugScreenPrintf("   [ Current Configuration ]\n");
  psvDebugScreenPrintf("   Camera: %s\n", (g_cam_device == SCE_CAMERA_DEVICE_FRONT) ? "FRONT" : "BACK");
  psvDebugScreenPrintf("   Resolution: %dx%d\n", g_res_widths[g_res_idx], g_res_heights[g_res_idx]);
  psvDebugScreenPrintf("   Framerate: %s\n", g_fps_names[g_fps_idx]);
  psvDebugScreenPrintf("   Camera Mode: %s\n", g_mode_names[g_cam_mode]);
  psvDebugScreenPrintf("   Webcam Stream: %s\n\n", g_webcam_off ? "OFF (Privacy Mode)" : "ON");

  psvDebugScreenPrintf("   [ Controls ]\n");
  psvDebugScreenPrintf("   D-PAD UP/DOWN : Cycle Resolution\n");
  psvDebugScreenPrintf("   D-PAD LEFT/RIGHT : Cycle Framerate\n");
  psvDebugScreenPrintf("   L / R Triggers : Cycle Camera Mode (Both for Default)\n");
  psvDebugScreenPrintf("   TRIANGLE : Toggle Camera (Front/Back)\n");
  psvDebugScreenPrintf("   CIRCLE : Turn Screen On/Off (battery save)\n");
  psvDebugScreenPrintf("   SQUARE : Toggle Webcam Stream (Privacy Mode)\n");
}

static int camera_apply_settings(void) {
  // Stop and close current camera devices (both front and back, to be safe)
  sceCameraStop(SCE_CAMERA_DEVICE_FRONT);
  sceCameraClose(SCE_CAMERA_DEVICE_FRONT);
  sceCameraStop(SCE_CAMERA_DEVICE_BACK);
  sceCameraClose(SCE_CAMERA_DEVICE_BACK);

  int w = g_res_widths[g_res_idx];
  int h = g_res_heights[g_res_idx];

  memset(&g_cam_info, 0, sizeof(g_cam_info));
  g_cam_info.size = sizeof(SceCameraInfo);
  g_cam_info.priority = SCE_CAMERA_PRIORITY_SHARE;
  g_cam_info.format = SCE_CAMERA_FORMAT_YUV422_PLANE;
  g_cam_info.resolution = g_res_choices[g_res_idx];
  g_cam_info.framerate = g_fps_choices[g_fps_idx];
  g_cam_info.width = w;
  g_cam_info.height = h;
  g_cam_info.pIBase = g_cam_buf;
  g_cam_info.sizeIBase = w * h;
  g_cam_info.pUBase = (void *)((char *)g_cam_buf + (w * h));
  g_cam_info.sizeUBase = (w * h) / 2;
  g_cam_info.pVBase = (void *)((char *)g_cam_buf + (w * h) + ((w * h) / 2));
  g_cam_info.sizeVBase = (w * h) / 2;
  g_cam_info.pitch = 0;
  g_cam_info.buffer = 0;

  int ret = sceCameraOpen(g_cam_device, &g_cam_info);
  if (ret < 0) {
    psvDebugScreenPrintf("  [!] sceCameraOpen failed: 0x%08X\n", ret);
    return ret;
  }

  ret = sceCameraStart(g_cam_device);
  if (ret < 0) {
    psvDebugScreenPrintf("  [!] sceCameraStart failed: 0x%08X\n", ret);
    return ret;
  }

  sceCameraSetISO(g_cam_device, SCE_CAMERA_ISO_AUTO);
  sceCameraSetGain(g_cam_device, SCE_CAMERA_GAIN_AUTO);
  sceCameraSetAutoControlHold(g_cam_device, 0);
  sceCameraSetBacklight(g_cam_device, 1);
  apply_camera_mode();

  if (g_jpeg_ctx) {
    free(g_jpeg_ctx);
    g_jpeg_ctx = NULL;
  }
  int ctx_size = sceJpegEncoderGetContextSize();
  g_jpeg_ctx = malloc(ctx_size);
  ret = sceJpegEncoderInit(g_jpeg_ctx, w, h,
                           SCE_JPEGENC_PIXELFORMAT_YCBCR422, g_jpg_buf,
                           g_jpg_size);
  if (ret < 0) {
    psvDebugScreenPrintf("  [!] sceJpegEncoderInit failed: 0x%08X\n", ret);
    return ret;
  }

  sceJpegEncoderSetCompressionRatio(g_jpeg_ctx, 128);

  update_hud_display();

  return 0;
}

static void handle_controls(void) {
  SceCtrlData pad;
  sceCtrlPeekBufferPositive(0, &pad, 1);

  uint32_t pressed = pad.buttons & ~g_last_buttons;
  g_last_buttons = pad.buttons;

  int changed = 0;

  if (pressed & SCE_CTRL_CIRCLE) {
    g_screen_mode = (g_screen_mode + 1) % 2;
    if (g_screen_mode == 0) {
      update_hud_display();
    } else {
      psvDebugScreenClear(0);
    }
  }

  if (pressed & SCE_CTRL_SQUARE) {
    g_webcam_off = !g_webcam_off;
    if (g_screen_mode == 0) {
      update_hud_display();
    }
  }

  if (g_screen_mode != 1) {
    if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
      g_res_idx = (g_res_idx + 1) % 3;
      changed = 1;
    }
    if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
      g_fps_idx = (g_fps_idx + 1) % 3;
      changed = 1;
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
      g_cam_device = (g_cam_device == SCE_CAMERA_DEVICE_FRONT) ? SCE_CAMERA_DEVICE_BACK : SCE_CAMERA_DEVICE_FRONT;
      changed = 1;
    }
    if ((pad.buttons & SCE_CTRL_LTRIGGER) && (pad.buttons & SCE_CTRL_RTRIGGER)) {
      if ((pressed & SCE_CTRL_LTRIGGER) || (pressed & SCE_CTRL_RTRIGGER)) {
        g_cam_mode = 0; // Padrao
        apply_camera_mode();
        update_hud_display();
      }
    } else {
      if (pressed & SCE_CTRL_LTRIGGER) {
        g_cam_mode = (g_cam_mode == 0) ? 3 : g_cam_mode - 1;
        apply_camera_mode();
        update_hud_display();
      }
      if (pressed & SCE_CTRL_RTRIGGER) {
        g_cam_mode = (g_cam_mode + 1) % 4;
        apply_camera_mode();
        update_hud_display();
      }
    }
  }

  if (changed) {
    camera_apply_settings();
  }
}

static int net_init(void) {
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  char *net_mem = malloc(NET_INIT_SIZE);

  SceNetInitParam param;
  param.memory = net_mem;
  param.size = NET_INIT_SIZE;
  param.flags = 0;

  int ret = sceNetInit(&param);
  if (ret < 0)
    return ret;

  ret = sceNetCtlInit();
  if (ret < 0)
    return ret;

  SceNetCtlInfo info;
  ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
  if (ret < 0)
    return ret;

  strncpy(g_ip_str, info.ip_address, sizeof(g_ip_str));

  return 0;
}

static int camera_open(void) {
  if (g_cam_mem_uid < 0) {
    int cam_size = (CAM_W * CAM_H * 2 + 0x3FFFF) & ~0x3FFFF;
    g_cam_mem_uid = sceKernelAllocMemBlock(
        "CamBuf", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cam_size, NULL);
    if (g_cam_mem_uid < 0)
      return g_cam_mem_uid;
    sceKernelGetMemBlockBase(g_cam_mem_uid, &g_cam_buf);
  }
  if (g_jpg_mem_uid < 0) {
    int jpg_size = (g_jpg_size + 0x3FFFF) & ~0x3FFFF;
    g_jpg_mem_uid = sceKernelAllocMemBlock(
        "JpgBuf", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, jpg_size, NULL);
    if (g_jpg_mem_uid < 0)
      return g_jpg_mem_uid;
    sceKernelGetMemBlockBase(g_jpg_mem_uid, &g_jpg_buf);
  }

  return camera_apply_settings();
}

/* ── mDNS Thread ───────────────────────────────────────────────── */

static int mdns_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;

  int sock = sceNetSocket("mDNS", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM,
                          SCE_NET_IPPROTO_UDP);
  if (sock < 0)
    return sceKernelExitDeleteThread(0);

  int opt = 1;
  sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &opt,
                   sizeof(opt));

  SceNetSockaddrIn bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_len = sizeof(bind_addr);
  bind_addr.sin_family = SCE_NET_AF_INET;
  bind_addr.sin_port = sceNetHtons(5353);
  bind_addr.sin_addr.s_addr = sceNetHtonl(0);

  if (sceNetBind(sock, (SceNetSockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    sceNetSocketClose(sock);
    return sceKernelExitDeleteThread(0);
  }

  SceNetIpMreq mreq;
  sceNetInetPton(SCE_NET_AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
  mreq.imr_interface.s_addr = sceNetHtonl(0);
  sceNetSetsockopt(sock, SCE_NET_IPPROTO_IP, SCE_NET_IP_ADD_MEMBERSHIP, &mreq,
                   sizeof(mreq));

  // Nome DNS correto em wire format: \x07vitacam\x05local\x00
  const uint8_t query_name[] = {7, 'v', 'i', 't', 'a', 'c', 'a', 'm',
                                5, 'l', 'o', 'c', 'a', 'l', 0};

  uint8_t buf[512];

  while (1) {
    SceNetSockaddrIn from;
    unsigned int fromlen = sizeof(from);
    int n = sceNetRecvfrom(sock, buf, sizeof(buf), 0, (SceNetSockaddr *)&from,
                           &fromlen);
    if (n < 12)
      continue;

    // Só processa queries (bit QR = 0), ignora respostas
    uint16_t flags = (buf[2] << 8) | buf[3];
    uint16_t qdcount = (buf[4] << 8) | buf[5];
    if ((flags & 0x8000) != 0 || qdcount == 0)
      continue;

    // Procura o nome vitacam.local no payload
    int match = 0;
    for (int i = 12; i <= n - (int)sizeof(query_name); i++) {
      if (memcmp(buf + i, query_name, sizeof(query_name)) == 0) {
        match = 1;
        break;
      }
    }
    if (!match)
      continue;

    // Resolve IP do string pra garantir byte order correto
    SceNetInAddr my_ip;
    sceNetInetPton(SCE_NET_AF_INET, g_ip_str, &my_ip);

    // Monta resposta mDNS A record
    // Header(12) + name(15) + type(2) + class(2) + ttl(4) + rdlen(2) + ip(4) =
    // 41 bytes
    uint8_t resp[41];
    memset(resp, 0, sizeof(resp));

    // Header
    resp[0] = buf[0];
    resp[1] = buf[1]; // Transaction ID copiado da query
    resp[2] = 0x84;
    resp[3] = 0x00; // Flags: Response + Authoritative
    resp[4] = 0x00;
    resp[5] = 0x00; // Questions: 0
    resp[6] = 0x00;
    resp[7] = 0x01; // Answers: 1
    resp[8] = 0x00;
    resp[9] = 0x00; // Authority: 0
    resp[10] = 0x00;
    resp[11] = 0x00; // Additional: 0

    // Name
    memcpy(resp + 12, query_name,
           sizeof(query_name)); // 15 bytes, termina em offset 27

    // Type A
    resp[27] = 0x00;
    resp[28] = 0x01;
    // Class IN + cache flush
    resp[29] = 0x80;
    resp[30] = 0x01;
    // TTL: 120s
    resp[31] = 0x00;
    resp[32] = 0x00;
    resp[33] = 0x00;
    resp[34] = 0x78;
    // RDATA length: 4
    resp[35] = 0x00;
    resp[36] = 0x04;
    // IP — memcpy direto do s_addr que já está em network byte order
    memcpy(resp + 37, &my_ip.s_addr, 4);

    // Envia unicast pra quem perguntou
    sceNetSendto(sock, resp, sizeof(resp), 0, (SceNetSockaddr *)&from, fromlen);

    // Envia multicast pra 224.0.0.251 — necessário pro macOS/iOS aceitar
    SceNetSockaddrIn mc_dst;
    memset(&mc_dst, 0, sizeof(mc_dst));
    mc_dst.sin_len = sizeof(mc_dst);
    mc_dst.sin_family = SCE_NET_AF_INET;
    mc_dst.sin_port = sceNetHtons(5353);
    sceNetInetPton(SCE_NET_AF_INET, "224.0.0.251", &mc_dst.sin_addr);
    sceNetSendto(sock, resp, sizeof(resp), 0, (SceNetSockaddr *)&mc_dst,
                 sizeof(mc_dst));
  }

  sceNetSocketClose(sock);
  return 0;
}

/* ── HTTP Server ───────────────────────────────────────────────── */

static int net_send_all(int sock, const void *data, int len) {
  const uint8_t *p = (const uint8_t *)data;
  int remaining = len;
  while (remaining > 0) {
    int sent = sceNetSend(sock, p, remaining, 0);
    if (sent <= 0)
      return -1;
    p += sent;
    remaining -= sent;
  }
  return len;
}

static void serve_html(int client) {
  const char *html =
      "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
      "<!DOCTYPE html><html><head><title>VitaCam</title>"
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
      "<style>"
      "body { margin: 0; background: #000; width: 100vw; height: 100vh; overflow: hidden; display: flex; justify-content: center; align-items: center; font-family: sans-serif; }"
      "img { width: 100%; height: 100%; object-fit: cover; cursor: pointer; transition: object-fit 0.2s ease; z-index: 1; }"
      "#overlay { position: absolute; top: 0; left: 0; width: 100%; height: 100%; background: #000; color: #fff; display: none; flex-direction: column; justify-content: center; align-items: center; z-index: 9999; text-align: center; }"
      "</style></head>"
      "<body>"
      "<div id=\"overlay\">"
      "<h2 style=\"font-size: 28px; margin-bottom: 8px;\">C&acirc;mera Desligada</h2>"
      "<p style=\"font-size: 16px; color: #888; margin-top: 0;\">Modo de Privacidade Ativo no PS Vita</p>"
      "</div>"
      "<img src=\"/stream\" onclick=\"this.style.objectFit = this.style.objectFit === 'cover' ? 'contain' : 'cover';\" title=\"Click to toggle Cover/Contain\">"
      "<script>"
      "function checkStatus() {"
      "  fetch('/status')"
      "    .then(r => r.json())"
      "    .then(data => {"
      "      const ov = document.getElementById('overlay');"
      "      ov.style.display = data.webcam_off ? 'flex' : 'none';"
      "    })"
      "    .catch(e => {});"
      "}"
      "setInterval(checkStatus, 500);"
      "</script>"
      "</body></html>";
  net_send_all(client, html, strlen(html));
}

static void serve_mjpeg(int client) {
  const char *hdr =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=vitacam\r\n"
      "Connection: close\r\n"
      "\r\n";
  if (net_send_all(client, hdr, strlen(hdr)) < 0)
    return;

  memset(&g_cam_read, 0, sizeof(g_cam_read));
  g_cam_read.size = sizeof(SceCameraRead);
  g_cam_read.mode = 0;

  char chunk_hdr[128];
  while (1) {
    handle_controls();

    if (g_webcam_off) {
      int w = g_res_widths[g_res_idx];
      int h = g_res_heights[g_res_idx];
      memset(g_cam_buf, 0, w * h);
      memset((char *)g_cam_buf + (w * h), 128, (w * h) / 2);
      memset((char *)g_cam_buf + (w * h) + ((w * h) / 2), 128, (w * h) / 2);
      sceKernelDelayThread(33 * 1000);
    } else {
      int ret = sceCameraRead(g_cam_device, &g_cam_read);
      if (ret < 0) {
        psvDebugScreenPrintf("  [!] sceCameraRead error: 0x%08X\n", ret);
        sceKernelDelayThread(500000); // 500ms
        continue;
      }
    }

    int jpg_size = sceJpegEncoderEncode(g_jpeg_ctx, g_cam_buf);
    if (jpg_size <= 0) {
      psvDebugScreenPrintf("  [!] JpegEncode error: %d (0x%08X)\n", jpg_size, jpg_size);
      sceKernelDelayThread(500000); // 500ms
      continue;
    }

    snprintf(chunk_hdr, sizeof(chunk_hdr),
             "--vitacam\r\n"
             "Content-Type: image/jpeg\r\n"
             "Content-Length: %d\r\n\r\n",
             jpg_size);

    if (net_send_all(client, chunk_hdr, strlen(chunk_hdr)) < 0)
      break;
    if (net_send_all(client, g_jpg_buf, jpg_size) < 0)
      break;
    if (net_send_all(client, "\r\n", 2) < 0)
      break;
  }
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void) {
  psvDebugScreenInit();
  PsvDebugScreenFont *font = psvDebugScreenGetFont();
  if (font) {
    PsvDebugScreenFont *font2x = psvDebugScreenScaleFont2x(font);
    if (font2x) {
      psvDebugScreenSetFont(font2x);
    }
  }

  psvDebugScreenPrintf("\n  [ VitaCam ]\n  Initializing...\n");

  if (net_init() < 0) {
    psvDebugScreenPrintf("Network init failed. Are you connected to WiFi?\n");
    sceKernelDelayThread(5000000);
    return 1;
  }

  if (camera_open() < 0) {
    psvDebugScreenPrintf("Camera init failed.\n");
    sceKernelDelayThread(5000000);
    return 1;
  }

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  // Setup server socket
  int server = sceNetSocket("HTTP", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
  int opt = 1;
  sceNetSetsockopt(server, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &opt,
                   sizeof(opt));
  
  // Set server socket to non-blocking
  sceNetSetsockopt(server, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &opt, sizeof(opt));

  int srv_buf = 262144;
  sceNetSetsockopt(server, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, &srv_buf, sizeof(srv_buf));
  sceNetSetsockopt(server, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &srv_buf, sizeof(srv_buf));

  SceNetSockaddrIn addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_len = sizeof(addr);
  addr.sin_family = SCE_NET_AF_INET;
  addr.sin_addr.s_addr = sceNetHtonl(0);
  // Tenta porta 80, se falhar (sem UNSAFE), fallback para 8080
  addr.sin_port = sceNetHtons(80);
  int bind_ret = sceNetBind(server, (SceNetSockaddr *)&addr, sizeof(addr));
  if (bind_ret < 0) {
    addr.sin_port = sceNetHtons(8080);
    bind_ret = sceNetBind(server, (SceNetSockaddr *)&addr, sizeof(addr));
    if (bind_ret < 0) {
      psvDebugScreenPrintf("Failed to bind port 80 and 8080: 0x%08X\n", bind_ret);
      sceKernelDelayThread(5000000);
      return 1;
    }
    g_actual_port = 8080;
  } else {
    g_actual_port = 80;
  }

  sceNetListen(server, 1);

  // Inicia thread mDNS
  SceUID mdns_thid = sceKernelCreateThread("mDNS_Thread", mdns_thread,
                                           0x10000100, 0x10000, 0, 0, NULL);
  sceKernelStartThread(mdns_thid, 0, NULL);

  update_hud_display();

  while (1) {
    handle_controls();

    SceNetSockaddrIn client_addr;
    unsigned int addr_len = sizeof(client_addr);

    // Non-blocking accept
    int client =
        sceNetAccept(server, (SceNetSockaddr *)&client_addr, &addr_len);
    if (client < 0) {
      sceKernelDelayThread(10 * 1000); // 10ms sleep
      continue;
    }

    // Set client socket to blocking for reading headers
    int opt_blocking = 0;
    sceNetSetsockopt(client, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &opt_blocking, sizeof(opt_blocking));

    int nodelay = 1;
    sceNetSetsockopt(client, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &nodelay,
                     sizeof(nodelay));

    int snd_buf = 262144;
    sceNetSetsockopt(client, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, &snd_buf, sizeof(snd_buf));
    sceNetSetsockopt(client, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &snd_buf, sizeof(snd_buf));

    // Read headers 1 byte at a time to prevent blocking on partial read
    char req_buf[2048];
    int total = 0;
    memset(req_buf, 0, sizeof(req_buf));

    while (total < (sizeof(req_buf) - 1)) {
      int r = sceNetRecv(client, req_buf + total, 1, 0);
      if (r <= 0) {
        break;
      }

      total += r;
      req_buf[total] = '\0';

      if (strstr(req_buf, "\r\n\r\n"))
        break;
    }

    if (strstr(req_buf, "GET /status")) {
      char resp[128];
      snprintf(resp, sizeof(resp),
               "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Connection: close\r\n\r\n"
               "{\"webcam_off\":%d}", g_webcam_off);
      net_send_all(client, resp, strlen(resp));
    } else {
      psvDebugScreenPrintf("  [*] Client connected!\n");
      psvDebugScreenPrintf("  [*] Request size: %d\n", total);

      if (strstr(req_buf, "GET /stream")) {
        psvDebugScreenPrintf("  [*] Serving MJPEG stream...\n");
        serve_mjpeg(client);
      } else {
        psvDebugScreenPrintf("  [*] Serving HTML index...\n");
        serve_html(client);
      }
      psvDebugScreenPrintf("  [*] Connection closed.\n");
    }

    sceNetSocketClose(client);
  }

  return 0;
}